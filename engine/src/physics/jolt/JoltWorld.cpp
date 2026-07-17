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
#include <Jolt/Physics/Body/BodyCreationSettings.h>

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
}

JoltWorld::~JoltWorld() {
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

    // Форма коллайдера.
    JPH::ShapeRefC shape;
    switch (desc.Shape) {
        case ShapeType::Sphere: {
            float r = glm::max(desc.Radius, 0.01f);
            shape = JPH::SphereShapeSettings(r).Create().Get();
            break;
        }
        case ShapeType::Capsule: {
            float r = glm::max(desc.Radius, 0.01f);
            float hh = glm::max(desc.HalfHeight, 0.01f);
            shape = JPH::CapsuleShapeSettings(hh, r).Create().Get();
            break;
        }
        case ShapeType::Box:
        default: {
            glm::vec3 he = glm::max(desc.HalfExtents, glm::vec3(0.01f));
            shape = JPH::BoxShapeSettings(JPH::Vec3(he.x, he.y, he.z)).Create().Get();
            break;
        }
    }

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

    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    JPH::BodyID id = bi.CreateAndAddBody(
        settings, desc.Type == BodyType::Static ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate);
    if (id.IsInvalid()) return kInvalidBody;

    BodyHandle h = m_next++;
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
    // не зависит от кадрового dt (как в Simple-бэкенде).
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
