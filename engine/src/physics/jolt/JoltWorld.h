#pragma once
#include <memory>
#include <unordered_map>

#include "sage/physics/PhysicsWorld.h"

// Jolt-типы прячем в .cpp (тяжёлые заголовки, специфичные макросы) — здесь
// только предобъявления через непрозрачные указатели, чтобы этот заголовок
// оставался лёгким и не тянул весь Jolt в остальной движок.
namespace JPH {
class PhysicsSystem;
class TempAllocator;
class JobSystem;
class BodyInterface;
class Constraint;
} // namespace JPH

namespace sage::physics {

class JoltBPLayerInterface;      // определены в .cpp
class JoltObjectVsBPFilter;
class JoltObjectLayerPairFilter;
class JoltContactListener;

// ---------------------------------------------------------------------------
// JoltWorld — основной физический бэкенд поверх jrouwe/JoltPhysics. Полноценная
// физика: динамика твёрдых тел с вращением, честные контакты, формы Box/Sphere/
// Capsule. Реализует тот же интерфейс PhysicsWorld, что и Builtin/Null —
// вызывающий код (PhysicsScene) их не различает.
//
// Весь код, специфичный для Jolt, изолирован в этом файле и JoltWorld.cpp
// (как OpenGL изолирован в rhi/opengl/) — подключается только при сборке с
// SAGE_PHYSICS_JOLT=ON.
// ---------------------------------------------------------------------------
class JoltWorld : public PhysicsWorld {
public:
    JoltWorld();
    ~JoltWorld() override;

    const char* BackendName() const override { return "Jolt"; }
    bool IsAvailable() const override { return true; }
    void SetGravity(const glm::vec3& gravity) override;

    BodyHandle CreateBody(const BodyDesc& desc) override;
    void RemoveBody(BodyHandle body) override;
    void Step(float dt) override;

    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const override;
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) override;
    glm::vec3 GetLinearVelocity(BodyHandle body) const override;
    void AddImpulse(BodyHandle body, const glm::vec3& impulse) override;
    RayHitInfo Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist) const override;
    std::vector<ContactEvent> DrainContactEvents() override;

    JointHandle CreateJoint(const JointDesc& desc) override;
    void RemoveJoint(JointHandle joint) override;
    bool SupportsJoints() const override { return true; }

private:
    std::unique_ptr<JPH::PhysicsSystem> m_system;
    std::unique_ptr<JPH::TempAllocator> m_tempAllocator;
    std::unique_ptr<JPH::JobSystem> m_jobSystem;

    // Фильтры слоёв — обязательные коллбеки Jolt (какие слои сталкиваются).
    std::unique_ptr<JoltBPLayerInterface> m_bpLayers;
    std::unique_ptr<JoltObjectVsBPFilter> m_objectVsBpFilter;
    std::unique_ptr<JoltObjectLayerPairFilter> m_objectLayerFilter;
    // Слушатель контактов: копит события начала/конца касания (см. .cpp).
    std::unique_ptr<JoltContactListener> m_contactListener;

    // Наш BodyHandle (uint32) -> Jolt BodyID (тоже uint32, но иной смысл).
    std::unordered_map<BodyHandle, uint32_t> m_bodies;
    BodyHandle m_next = 1;
    // Наш JointHandle -> Jolt Constraint (держим ссылку, чтобы жил в системе).
    std::unordered_map<JointHandle, JPH::Constraint*> m_joints;
    JointHandle m_nextJoint = 1;
    float m_accum = 0.0f;
};

} // namespace sage::physics
