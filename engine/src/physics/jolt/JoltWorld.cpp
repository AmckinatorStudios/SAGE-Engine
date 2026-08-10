#include "physics/jolt/JoltWorld.h"

#include <algorithm>

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
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <mutex>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>

#include <cstdarg>
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

// ---------------------------------------------------------------------------
//  Слушатель контактов
// ---------------------------------------------------------------------------
//
// Jolt зовёт эти методы ИЗ РАБОЧИХ ПОТОКОВ посреди шага симуляции. Делать здесь
// что-либо, кроме записи в защищённый буфер, нельзя: сцена в этот момент
// принадлежит физике, а скрипты вообще однопоточные. Поэтому события только
// копятся, а разбирает их главный поток в PollContacts.
class JoltWorld::ContactCollector : public JPH::ContactListener {
public:
    explicit ContactCollector(JoltWorld* owner) : m_owner(owner) {}

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override {
        (void)settings;
        Push(a, b, manifold, ContactEvent::Phase::Begin);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        ContactEvent e;
        e.When = ContactEvent::Phase::End;
        e.A = m_owner->HandleOf(pair.GetBody1ID().GetIndexAndSequenceNumber());
        e.B = m_owner->HandleOf(pair.GetBody2ID().GetIndexAndSequenceNumber());
        if (e.A == kInvalidBody || e.B == kInvalidBody) return;  // тело уже удалили
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(e);
    }

    void Take(std::vector<ContactEvent>& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.swap(m_events);
        m_events.clear();
    }

private:
    void Push(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
              ContactEvent::Phase phase) {
        ContactEvent e;
        e.When = phase;
        e.A = m_owner->HandleOf(a.GetID().GetIndexAndSequenceNumber());
        e.B = m_owner->HandleOf(b.GetID().GetIndexAndSequenceNumber());
        if (e.A == kInvalidBody || e.B == kInvalidBody) return;
        const JPH::Vec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        e.Point = glm::vec3(p.GetX(), p.GetY(), p.GetZ());
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        e.Normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
        e.Sensor = a.IsSensor() || b.IsSensor();
        // Сила удара — по скорости сближения и массам. Точное значение импульса
        // Jolt отдаёт только в OnContactPersisted следующего шага, а событие
        // «врезался» нужно в тот же кадр: игра по нему играет звук и трясёт
        // камеру, и опоздание на кадр слышно.
        if (!e.Sensor) {
            const JPH::Vec3 va = a.GetLinearVelocity();
            const JPH::Vec3 vb = b.GetLinearVelocity();
            const JPH::Vec3 rel = va - vb;
            const float closing = std::abs(rel.Dot(manifold.mWorldSpaceNormal));
            const float ma = a.GetMotionType() == JPH::EMotionType::Dynamic
                                 ? 1.0f / std::max(a.GetMotionProperties()->GetInverseMass(), 1e-6f)
                                 : 0.0f;
            const float mb = b.GetMotionType() == JPH::EMotionType::Dynamic
                                 ? 1.0f / std::max(b.GetMotionProperties()->GetInverseMass(), 1e-6f)
                                 : 0.0f;
            const float m = (ma > 0.0f && mb > 0.0f) ? (ma * mb) / (ma + mb) : std::max(ma, mb);
            e.Impulse = closing * m;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        // Предохранитель: за шаг может случиться сколько угодно контактов, а
        // очередь читает главный поток. Расти без предела ей нельзя — иначе
        // куча песка на полу съедает память быстрее, чем игра успевает читать.
        if (m_events.size() < 8192) m_events.push_back(e);
    }

    JoltWorld* m_owner;
    std::vector<ContactEvent> m_events;
    std::mutex m_mutex;
};

// Виртуальный контроллер персонажа Jolt плюс наши настройки.
struct JoltWorld::CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> Ptr;
    LayerMask CollidesWith = kAllLayers;
    // Высота ступеньки хранится ЗДЕСЬ, потому что Jolt получает её не при
    // создании контроллера, а на каждый шаг (ExtendedUpdateSettings). Раньше в
    // этом месте стояла константа 0.35, и поле CharacterDesc::StepHeight не
    // читал никто: игра выставляла ступеньку, движок её молча игнорировал.
    float StepHeight = 0.35f;
};

BodyHandle JoltWorld::HandleOf(uint32_t joltId) const {
    auto it = m_byJoltId.find(joltId);
    return it == m_byJoltId.end() ? kInvalidBody : it->second;
}

LayerMask JoltWorld::LayerOf(BodyHandle body) const {
    auto it = m_layers.find(body);
    return it == m_layers.end() ? kLayerDefault : it->second;
}

// Фильтр тел по НАШЕЙ маске слоёв. Отдельный фильтр, а не проверка результата
// постфактум: узкая фаза, отбросив тело сразу, не считает по нему пересечение,
// а «ближайшее» в её ответе уже учитывает отбор — иначе пришлось бы повторять
// запрос с обрезанной дальностью, пока не найдётся подходящее.
namespace {
class MaskBodyFilter : public JPH::BodyFilter {
public:
    MaskBodyFilter(const JoltWorld* world, LayerMask mask) : m_world(world), m_mask(mask) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        const BodyHandle h = m_world->HandleOf(body.GetID().GetIndexAndSequenceNumber());
        return h != kInvalidBody && (m_world->LayerOf(h) & m_mask) != 0;
    }

private:
    const JoltWorld* m_world;
    LayerMask m_mask;
};
} // namespace

bool JoltWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                        RayHit& out, LayerMask mask) const {
    out = RayHit{};
    if (!m_system) return false;
    const float len = glm::length(direction);
    if (len < 1e-6f || maxDistance <= 0.0f) return false;
    const glm::vec3 dir = direction / len;

    const JPH::RRayCast ray{
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance)};

    JPH::RayCastResult hit;
    const MaskBodyFilter filter(this, mask);
    if (!m_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return false;

    const BodyHandle handle = HandleOf(hit.mBodyID.GetIndexAndSequenceNumber());
    if (handle == kInvalidBody) return false;

    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out.Hit = true;
    out.Body = handle;
    out.Point = glm::vec3(point.GetX(), point.GetY(), point.GetZ());
    out.Distance = hit.mFraction * maxDistance;
    // Нормаль спрашиваем у тела под блокировкой: узкая фаза отдаёт только
    // дескриптор подформы, а поверхность знает само тело в своей позе.
    JPH::BodyLockRead lock(m_system->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
        out.Normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    }
    return true;
}

int JoltWorld::OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                             LayerMask mask) const {
    out.clear();
    if (!m_system || radius <= 0.0f) return 0;
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> bodies;
    m_system->GetBroadPhaseQuery().CollideSphere(JPH::Vec3(center.x, center.y, center.z), radius,
                                                 bodies);
    for (const JPH::BodyID& id : bodies.mHits) {
        const BodyHandle handle = HandleOf(id.GetIndexAndSequenceNumber());
        if (handle == kInvalidBody || (LayerOf(handle) & mask) == 0) continue;
        out.push_back(handle);
    }
    return (int)out.size();
}

void JoltWorld::PollContacts(std::vector<ContactEvent>& out) {
    out.clear();
    if (m_contacts) m_contacts->Take(out);
}

CharacterHandle JoltWorld::CreateCharacter(const CharacterDesc& desc) {
    if (!m_system) return kInvalidCharacter;
    // Капсула стоит на земле нижней точкой, а начало координат Jolt держит в
    // центре: поэтому форма поднимается на половину высоты. Иначе персонаж
    // рождается по пояс в полу и первым же шагом выталкивается вверх.
    const float halfCyl = std::max(0.05f, desc.Height * 0.5f - desc.Radius);
    JPH::Ref<JPH::Shape> shape =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, halfCyl + desc.Radius, 0),
                                            JPH::Quat::sIdentity(),
                                            new JPH::CapsuleShape(halfCyl, desc.Radius))
            .Create()
            .Get();

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMaxSlopeAngle = glm::radians(desc.MaxSlopeDeg);
    settings->mMass = desc.Mass;
    // Плоскость опоры чуть ниже подошвы: без неё контроллер «не видит» пол,
    // когда стоит на нём вплотную, и считает себя в воздухе через кадр.
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3(0, 1, 0), -desc.Radius);

    auto entry = std::make_unique<CharacterEntry>();
    entry->CollidesWith = desc.CollidesWith;
    // Не выше девяти десятых роста: ступенька в собственный рост — это уже не
    // шаг, а перепрыгивание препятствия целиком, и лечится оно прыжком.
    entry->StepHeight = std::clamp(desc.StepHeight, 0.0f, desc.Height * 0.9f);
    entry->Ptr = new JPH::CharacterVirtual(
        settings, JPH::RVec3(desc.Position.x, desc.Position.y, desc.Position.z),
        JPH::Quat::sIdentity(), m_system.get());

    const CharacterHandle handle = m_nextCharacter++;
    m_characters[handle] = std::move(entry);
    return handle;
}

void JoltWorld::RemoveCharacter(CharacterHandle character) { m_characters.erase(character); }

void JoltWorld::MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) {
    auto it = m_characters.find(character);
    if (it == m_characters.end() || !m_system || dt <= 0.0f) return;
    JPH::CharacterVirtual* ch = it->second->Ptr;
    ch->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));

    JPH::CharacterVirtual::ExtendedUpdateSettings upd;
    // Шаг вверх/вниз — то, чем контроллер отличается от капсулы на пружине:
    // на ступеньку он ВЗБИРАЕТСЯ, а не упирается и не подпрыгивает. Высоту
    // задаёт игра (CharacterDesc::StepHeight); здесь стояла константа, и
    // настройка не работала вовсе.
    upd.mWalkStairsStepUp = JPH::Vec3(0, it->second->StepHeight, 0);
    ch->ExtendedUpdate(dt, m_system->GetGravity(), upd,
                       m_system->GetDefaultBroadPhaseLayerFilter(ObjectLayers::MOVING),
                       m_system->GetDefaultLayerFilter(ObjectLayers::MOVING), {}, {}, *m_tempAllocator);
}

CharacterState JoltWorld::GetCharacterState(CharacterHandle character) const {
    CharacterState st;
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return st;
    const JPH::CharacterVirtual* ch = it->second->Ptr;
    const JPH::RVec3 p = ch->GetPosition();
    const JPH::Vec3 v = ch->GetLinearVelocity();
    const JPH::Vec3 n = ch->GetGroundNormal();
    st.Position = glm::vec3(p.GetX(), p.GetY(), p.GetZ());
    st.Velocity = glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    st.GroundNormal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    st.Grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    st.OnSteepSlope = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnSteepGround;
    return st;
}

void JoltWorld::SetCharacterPosition(CharacterHandle character, const glm::vec3& position) {
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return;
    it->second->Ptr->SetPosition(JPH::RVec3(position.x, position.y, position.z));
}

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

    // Слушатель контактов ставится сразу: события копятся с первого же шага, и
    // «включить их потом» значило бы потерять всё, что случилось до включения.
    m_contacts = std::make_unique<ContactCollector>(this);
    m_system->SetContactListener(m_contacts.get());
}

JoltWorld::~JoltWorld() {
    // Отпускаем удержанные ссылки на соединения (AddRef при создании). Сами
    // Constraint'ы уедут вместе с PhysicsSystem; наш Release балансирует ref-счёт.
    for (auto& kv : m_joints)
        if (kv.second) kv.second->Release();
    m_joints.clear();
    // Тела удаляются вместе с PhysicsSystem. Глобальную фабрику Jolt намеренно
    // НЕ рушим — она разделяется всеми мирами на время жизни процесса.
    // Персонажи держат ссылку на систему — снимаем их ДО неё.
    m_characters.clear();
    if (m_system) m_system->SetContactListener(nullptr);
    m_system.reset();
    m_contacts.reset();
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
    // Сенсор обнаруживает касание, но не отталкивает. Событие о нём приходит
    // тем же PollContacts — игре незачем знать, зона это или стена, пока она
    // не решит, что с этим делать.
    settings.mIsSensor = desc.Sensor;
    if (desc.Sensor) settings.mCollideKinematicVsNonDynamic = true;
    if (desc.Type == BodyType::Dynamic && desc.Mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.Mass;
    }

    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    JPH::BodyID id = bi.CreateAndAddBody(
        settings, desc.Type == BodyType::Static ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate);
    if (id.IsInvalid()) return kInvalidBody;

    BodyHandle h = m_next++;
    const uint32_t joltId = id.GetIndexAndSequenceNumber();
    m_bodies[h] = joltId;
    m_byJoltId[joltId] = h;
    m_layers[h] = desc.Layer;
    return h;
}

void JoltWorld::RemoveBody(BodyHandle body) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    m_byJoltId.erase(it->second);
    m_layers.erase(body);
    m_bodies.erase(it);
}

void JoltWorld::Step(float dt) {
    if (!m_system) return;
    // Фиксированный внутренний шаг 1/60 с аккумулятором — стабильность симуляции
    // не зависит от кадрового dt (как и во встроенном бэкенде).
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
