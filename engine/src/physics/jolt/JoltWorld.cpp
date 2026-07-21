#include "physics/jolt/JoltWorld.h"

// Jolt требует, чтобы его собственный зонтичный заголовок включался ПЕРВЫМ.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>

#include <cstdarg>
#include <mutex>
#include <thread>

#include "sage/core/Log.h"

using namespace sage::physics;

// ============================================================================
//  Слои столкновений Jolt (минимальная схема: подвижное / неподвижное).
//    NON_MOVING — статика (пол/стены); MOVING — динамика и кинематика.
//  Broadphase-слои повторяют объектные один-к-одному. Неподвижное с
//  неподвижным не сталкивается — всё остальное сталкивается.
// ============================================================================
namespace {

namespace ObjectLayers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM = 2;
} // namespace ObjectLayers

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM = 2;
} // namespace BroadPhaseLayers

// Трассировка Jolt -> наш лог (иначе она уходит в printf).
static void JoltTrace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_INFO("Jolt") << buf;
}

// Разовая инициализация глобального состояния Jolt (аллокатор, фабрика типов).
// Живёт на весь процесс — несколько миров переиспользуют её (защита флагом).
static void EnsureJoltGlobals() {
    static bool initialized = false;
    if (initialized) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    initialized = true;
}

} // namespace

// ============================================================================
//  Реализации обязательных фильтров слоёв (объявлены в JoltWorld.h).
// ============================================================================
namespace sage::physics {

class JoltBPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == ObjectLayers::NON_MOVING ? BroadPhaseLayers::NON_MOVING
                                                 : BroadPhaseLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class JoltObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override {
        if (layer == ObjectLayers::NON_MOVING) return bpLayer == BroadPhaseLayers::MOVING;
        return true; // MOVING сталкивается со всем
    }
};

class JoltObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == ObjectLayers::NON_MOVING) return b == ObjectLayers::MOVING;
        return true; // MOVING сталкивается со всем
    }
};

// ---------------------------------------------------------------------------
// JoltContactListener — переводит колбэки контактов Jolt в бэкенд-независимые
// ContactEvent. Колбэки приходят из ПОТОКОВ решателя во время Update — все
// обращения под мьютексом. Пара идентифицируется BodyID (OnContactRemoved не
// даёт тел, только id), наружу отдаются НАШИ хэндлы из Body::GetUserData.
// Компаунд-формы дают несколько подшейп-пар — события дедуплицируются по паре
// тел (счётчик подшейп-пар на пару).
// ---------------------------------------------------------------------------
class JoltContactListener : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold&, JPH::ContactSettings&) override {
        uint64_t key = PairKey(a.GetID(), b.GetID());
        std::lock_guard<std::mutex> lock(m_mutex);
        auto [it, inserted] = m_pairs.emplace(
            key, std::make_pair((uint32_t)a.GetUserData(), (uint32_t)b.GetUserData()));
        m_refs[key]++;
        if (inserted) m_events.push_back({it->second.first, it->second.second, true});
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& p) override {
        uint64_t key = PairKey(p.GetBody1ID(), p.GetBody2ID());
        std::lock_guard<std::mutex> lock(m_mutex);
        auto rit = m_refs.find(key);
        if (rit == m_refs.end()) return;
        if (--rit->second > 0) return; // пара ещё касается другими подшейпами
        m_refs.erase(rit);
        auto it = m_pairs.find(key);
        if (it != m_pairs.end()) {
            m_events.push_back({it->second.first, it->second.second, false});
            m_pairs.erase(it);
        }
    }

    std::vector<ContactEvent> Drain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ContactEvent> out;
        out.swap(m_events);
        return out;
    }

private:
    static uint64_t PairKey(JPH::BodyID a, JPH::BodyID b) {
        uint32_t x = a.GetIndexAndSequenceNumber(), y = b.GetIndexAndSequenceNumber();
        if (x > y) std::swap(x, y);
        return ((uint64_t)x << 32) | y;
    }

    std::mutex m_mutex;
    std::vector<ContactEvent> m_events;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> m_pairs; // ключ -> наши хэндлы
    std::unordered_map<uint64_t, int> m_refs;                            // подшейп-пар на пару
};

} // namespace sage::physics

// ============================================================================
//  JoltWorld
// ============================================================================
namespace {

JPH::RVec3 ToJolt(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat ToJoltQuat(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
// В одинарной точности RVec3 == Vec3 — один перегруз покрывает оба (GetPosition
// возвращает RVec3, GetLinearVelocity — Vec3).
glm::vec3 FromJolt(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
glm::quat FromJoltQuat(const JPH::Quat& q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

// Одна выпуклая форма Jolt по параметрам (box/sphere/capsule) — общий код для
// одиночных и составных тел. Возвращает null при ошибке (вызывающий пропустит
// подшейп/тело). Тонкие боксы безопасны: convex radius адаптируется под
// наименьшую полуось (иначе Jolt делает форму вырожденной и падает при сборке
// compound — реальный баг, найденный на «гантели» со сплюснутой перекладиной).
JPH::ShapeRefC MakeShape(ShapeType shape, const glm::vec3& halfExtents, float radius, float halfHeight) {
    JPH::Shape::ShapeResult res;
    switch (shape) {
        case ShapeType::Sphere:
            res = JPH::SphereShapeSettings(glm::max(radius, 0.01f)).Create();
            break;
        case ShapeType::Capsule:
            res = JPH::CapsuleShapeSettings(glm::max(halfHeight, 0.01f), glm::max(radius, 0.01f)).Create();
            break;
        case ShapeType::Box:
        default: {
            glm::vec3 he = glm::max(halfExtents, glm::vec3(0.01f));
            float minHalf = glm::min(he.x, glm::min(he.y, he.z));
            // Convex radius не больше 0.05 (дефолт Jolt) и не больше 90% самой
            // тонкой полуоси — тонкая перекладина/пластина остаётся валидной.
            float convex = glm::min(0.05f, minHalf * 0.9f);
            JPH::BoxShapeSettings s(JPH::Vec3(he.x, he.y, he.z), convex);
            res = s.Create();
            break;
        }
    }
    if (res.HasError()) {
        LOG_ERROR("Jolt") << "Форма не создана: " << res.GetError().c_str();
        return {};
    }
    return res.Get();
}

// Перпендикуляр к оси (для рамок Hinge/Slider, которым нужна нормаль ⟂ оси).
JPH::Vec3 PerpendicularTo(const JPH::Vec3& axis) {
    JPH::Vec3 a = axis.NormalizedOr(JPH::Vec3::sAxisY());
    JPH::Vec3 ref = std::abs(a.GetX()) < 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
    return a.Cross(ref).NormalizedOr(JPH::Vec3::sAxisZ());
}

} // namespace

JoltWorld::JoltWorld() {
    EnsureJoltGlobals();

    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    unsigned threads = std::thread::hardware_concurrency();
    m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        (int)(threads > 1 ? threads - 1 : 1));

    m_bpLayers = std::make_unique<JoltBPLayerInterface>();
    m_objectVsBpFilter = std::make_unique<JoltObjectVsBPFilter>();
    m_objectLayerFilter = std::make_unique<JoltObjectLayerPairFilter>();

    m_system = std::make_unique<JPH::PhysicsSystem>();
    const JPH::uint kMaxBodies = 4096;
    const JPH::uint kNumBodyMutexes = 0; // авто
    const JPH::uint kMaxBodyPairs = 4096;
    const JPH::uint kMaxContacts = 4096;
    m_system->Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContacts,
                   *m_bpLayers, *m_objectVsBpFilter, *m_objectLayerFilter);

    // События контактов -> скрипты (OnCollisionEnter/Exit через PhysicsScene).
    m_contactListener = std::make_unique<JoltContactListener>();
    m_system->SetContactListener(m_contactListener.get());
}

JoltWorld::~JoltWorld() {
    // Отпускаем удержанные ссылки на соединения (AddRef при создании). Сами
    // Constraint'ы уедут вместе с PhysicsSystem; наш Release балансирует ref-счёт.
    for (auto& kv : m_joints)
        if (kv.second) kv.second->Release();
    m_joints.clear();
    // Тела удаляются вместе с PhysicsSystem. Глобальную фабрику Jolt намеренно
    // НЕ рушим — она разделяется всеми мирами на время жизни процесса.
    m_system.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();
}

void JoltWorld::SetGravity(const glm::vec3& gravity) {
    if (m_system) m_system->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
}

BodyHandle JoltWorld::CreateBody(const BodyDesc& desc) {
    if (!m_system) return kInvalidBody;

    // Форма коллайдера: одиночная или СОСТАВНАЯ (StaticCompoundShape из
    // дочерних форм с локальными смещениями — «молоток», кластер примитивов).
    JPH::ShapeRefC shape;
    if (!desc.Children.empty()) {
        JPH::StaticCompoundShapeSettings compound;
        // Держим ссылки на дочерние формы живыми до Create() (компаунду нужен ≥1
        // валидный подшейп; StaticCompoundShape требует минимум 2 — при одной
        // добавляем её же копию, чтобы не падать).
        std::vector<JPH::ShapeRefC> keepAlive;
        for (const ChildShape& c : desc.Children) {
            JPH::ShapeRefC child = MakeShape(c.Shape, c.HalfExtents, c.Radius, c.HalfHeight);
            if (!child) continue;
            keepAlive.push_back(child);
            compound.AddShape(JPH::Vec3(c.Position.x, c.Position.y, c.Position.z),
                              ToJoltQuat(c.Rotation), child.GetPtr());
        }
        if (keepAlive.empty()) {
            LOG_ERROR("Jolt") << "Составная форма без валидных подшейпов";
            return kInvalidBody;
        }
        // StaticCompoundShape требует ≥2 подшейпа — дублируем единственный.
        if (keepAlive.size() == 1) {
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), keepAlive[0].GetPtr());
        }
        JPH::Shape::ShapeResult res = compound.Create();
        if (res.HasError()) {
            LOG_ERROR("Jolt") << "Составная форма не создана: " << res.GetError().c_str();
            return kInvalidBody;
        }
        shape = res.Get();
    } else {
        shape = MakeShape(desc.Shape, desc.HalfExtents, desc.Radius, desc.HalfHeight);
    }
    if (!shape) return kInvalidBody; // форма не создалась — тела не будет (не падаем)

    JPH::EMotionType motion = JPH::EMotionType::Dynamic;
    JPH::ObjectLayer layer = ObjectLayers::MOVING;
    switch (desc.Type) {
        case BodyType::Static:    motion = JPH::EMotionType::Static;    layer = ObjectLayers::NON_MOVING; break;
        case BodyType::Kinematic: motion = JPH::EMotionType::Kinematic; layer = ObjectLayers::MOVING; break;
        case BodyType::Dynamic:   motion = JPH::EMotionType::Dynamic;   layer = ObjectLayers::MOVING; break;
    }

    JPH::BodyCreationSettings settings(shape, ToJolt(desc.Position), ToJoltQuat(desc.Rotation),
                                       motion, layer);
    settings.mFriction = desc.Friction;
    settings.mRestitution = desc.Restitution;
    if (desc.Type == BodyType::Dynamic && desc.Mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.Mass;
    }

    // Наш хэндл — в user data тела: рейкаст и контакт-листенер отдают его
    // наружу без обратных карт.
    BodyHandle h = m_next++;
    settings.mUserData = (JPH::uint64)h;

    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    JPH::BodyID id = bi.CreateAndAddBody(
        settings, desc.Type == BodyType::Static ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate);
    if (id.IsInvalid()) return kInvalidBody;

    m_bodies[h] = id.GetIndexAndSequenceNumber();
    return h;
}

void JoltWorld::RemoveBody(BodyHandle body) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    m_bodies.erase(it);
}

void JoltWorld::Step(float dt) {
    if (!m_system) return;
    // Фиксированный внутренний шаг 1/60 с аккумулятором — стабильность симуляции
    // не зависит от кадрового dt (как во встроенном бэкенде).
    const float fixed = 1.0f / 60.0f;
    m_accum += glm::min(dt, 0.25f);
    int guard = 0;
    while (m_accum >= fixed && guard < 8) {
        m_system->Update(fixed, 1, m_tempAllocator.get(), m_jobSystem.get());
        m_accum -= fixed;
        ++guard;
    }
}

void JoltWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    const JPH::BodyInterface& bi = m_system->GetBodyInterface();
    position = FromJolt(bi.GetPosition(id));
    rotation = FromJoltQuat(bi.GetRotation(id));
}

void JoltWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    // Kinematic ведут «мягко» (MoveKinematic корректно толкает динамику); прочие
    // тела телепортируем жёстко.
    if (bi.GetMotionType(id) == JPH::EMotionType::Kinematic) {
        bi.MoveKinematic(id, ToJolt(position), ToJoltQuat(rotation), 1.0f / 60.0f);
    } else {
        bi.SetPositionAndRotation(id, ToJolt(position), ToJoltQuat(rotation), JPH::EActivation::Activate);
    }
}

void JoltWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().SetLinearVelocity(id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

glm::vec3 JoltWorld::GetLinearVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return glm::vec3(0.0f);
    JPH::BodyID id(it->second);
    return FromJolt(m_system->GetBodyInterface().GetLinearVelocity(id));
}

void JoltWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

JointHandle JoltWorld::CreateJoint(const JointDesc& desc) {
    if (!m_system) return kInvalidJoint;

    // Тело A обязательно; тело B == invalid -> крепим к неподвижному «миру».
    auto ia = m_bodies.find(desc.BodyA);
    if (ia == m_bodies.end()) return kInvalidJoint;

    const JPH::BodyLockInterface& lock = m_system->GetBodyLockInterfaceNoLock();
    JPH::Body* a = lock.TryGetBody(JPH::BodyID(ia->second));
    if (!a) return kInvalidJoint;

    JPH::Body* b = &JPH::Body::sFixedToWorld;
    if (desc.BodyB != kInvalidBody) {
        auto ib = m_bodies.find(desc.BodyB);
        if (ib == m_bodies.end()) return kInvalidJoint;
        b = lock.TryGetBody(JPH::BodyID(ib->second));
        if (!b) return kInvalidJoint;
    }

    JPH::RVec3 anchor = ToJolt(desc.Anchor);
    JPH::Vec3 axis = JPH::Vec3(desc.Axis.x, desc.Axis.y, desc.Axis.z).NormalizedOr(JPH::Vec3::sAxisY());

    JPH::Constraint* constraint = nullptr;
    switch (desc.Type) {
        case JointType::Fixed: {
            JPH::FixedConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true; // фиксируем текущее взаимное положение тел
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Point: {
            JPH::PointConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Hinge: {
            JPH::HingeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mHingeAxis1 = s.mHingeAxis2 = axis;
            JPH::Vec3 normal = PerpendicularTo(axis);
            s.mNormalAxis1 = s.mNormalAxis2 = normal;
            if (desc.UseLimits) {
                s.mLimitsMin = glm::radians(desc.MinLimit);
                s.mLimitsMax = glm::radians(desc.MaxLimit);
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Slider: {
            JPH::SliderConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.SetSliderAxis(axis); // выставляет ось скольжения + нормали
            if (desc.UseLimits) {
                s.mLimitsMin = desc.MinLimit;
                s.mLimitsMax = desc.MaxLimit;
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Distance: {
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mMinDistance = desc.MinDistance;
            s.mMaxDistance = desc.MaxDistance;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Cone: {
            JPH::ConeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mTwistAxis1 = s.mTwistAxis2 = axis;
            s.mHalfConeAngle = glm::radians(glm::clamp(desc.ConeHalfAngle, 0.0f, 180.0f));
            constraint = s.Create(*a, *b);
            break;
        }
    }
    if (!constraint) return kInvalidJoint;

    constraint->AddRef(); // держим ссылку сами (карта m_joints) — иначе удалится
    m_system->AddConstraint(constraint);
    JointHandle h = m_nextJoint++;
    m_joints[h] = constraint;
    return h;
}

void JoltWorld::RemoveJoint(JointHandle joint) {
    auto it = m_joints.find(joint);
    if (it == m_joints.end() || !m_system) return;
    m_system->RemoveConstraint(it->second);
    it->second->Release();
    m_joints.erase(it);
}

RayHitInfo JoltWorld::Raycast(const glm::vec3& origin, const glm::vec3& dirIn, float maxDist) const {
    RayHitInfo out;
    if (!m_system || maxDist <= 0.0f) return out;
    glm::vec3 dir = dirIn;
    float len = glm::length(dir);
    if (len < 1e-8f) return out;
    dir /= len;

    JPH::RRayCast ray(ToJolt(origin), JPH::Vec3(dir.x, dir.y, dir.z) * maxDist);
    JPH::RayCastResult hit;
    if (!m_system->GetNarrowPhaseQuery().CastRay(ray, hit)) return out;

    out.Hit = true;
    out.Distance = hit.mFraction * maxDist;
    out.Position = origin + dir * out.Distance;
    // Нормаль и наш хэндл — под блокировкой тела (требование интерфейса Jolt).
    JPH::BodyLockRead lock(m_system->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        out.Body = (BodyHandle)body.GetUserData();
        JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
        out.Normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    }
    return out;
}

std::vector<ContactEvent> JoltWorld::DrainContactEvents() {
    return m_contactListener ? m_contactListener->Drain()
                             : std::vector<ContactEvent>{};
}
