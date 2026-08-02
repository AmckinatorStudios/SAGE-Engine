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

// ---------------------------------------------------------------------------
// JoltWorld — основной физический бэкенд поверх jrouwe/JoltPhysics. Полноценная
// физика: динамика твёрдых тел с вращением, честные контакты, формы Box/Sphere/
// Capsule. Реализует тот же интерфейс PhysicsWorld, что и Simple/Null —
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

    JointHandle CreateJoint(const JointDesc& desc) override;
    void RemoveJoint(JointHandle joint) override;
    bool SupportsJoints() const override { return true; }

    bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 RayHit& out, LayerMask mask) const override;
    int OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                      LayerMask mask) const override;
    bool SupportsQueries() const override { return true; }

    void PollContacts(std::vector<ContactEvent>& out) override;
    bool SupportsContacts() const override { return true; }

    CharacterHandle CreateCharacter(const CharacterDesc& desc) override;
    void RemoveCharacter(CharacterHandle character) override;
    void MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) override;
    CharacterState GetCharacterState(CharacterHandle character) const override;
    void SetCharacterPosition(CharacterHandle character, const glm::vec3& position) override;
    bool SupportsCharacters() const override { return true; }

    // Наш дескриптор по Jolt BodyID — нужен слушателю контактов и лучам,
    // которые возвращают наружу только наши хендлы, а не внутренние ID.
    BodyHandle HandleOf(uint32_t joltId) const;
    LayerMask LayerOf(BodyHandle body) const;

private:
    class ContactCollector;   // слушатель контактов Jolt (см. .cpp)
    struct CharacterEntry;
    std::unique_ptr<JPH::PhysicsSystem> m_system;
    std::unique_ptr<JPH::TempAllocator> m_tempAllocator;
    std::unique_ptr<JPH::JobSystem> m_jobSystem;

    // Фильтры слоёв — обязательные коллбеки Jolt (какие слои сталкиваются).
    std::unique_ptr<JoltBPLayerInterface> m_bpLayers;
    std::unique_ptr<JoltObjectVsBPFilter> m_objectVsBpFilter;
    std::unique_ptr<JoltObjectLayerPairFilter> m_objectLayerFilter;

    // Наш BodyHandle (uint32) -> Jolt BodyID (тоже uint32, но иной смысл).
    std::unordered_map<BodyHandle, uint32_t> m_bodies;
    BodyHandle m_next = 1;
    // Наш JointHandle -> Jolt Constraint (держим ссылку, чтобы жил в системе).
    std::unordered_map<JointHandle, JPH::Constraint*> m_joints;
    JointHandle m_nextJoint = 1;
    float m_accum = 0.0f;

    // Обратная карта Jolt BodyID -> наш хендл и слои тел. Слои держим у себя, а
    // не в системе слоёв Jolt: та отвечает за то, ЧТО с чем сталкивается на
    // уровне широкой фазы, и переучивать её под пользовательские маски значит
    // трогать самую хрупкую часть настройки. Фильтровать результат запроса
    // одним `&` дешевле и ничего не ломает.
    std::unordered_map<uint32_t, BodyHandle> m_byJoltId;
    std::unordered_map<BodyHandle, LayerMask> m_layers;

    std::unique_ptr<ContactCollector> m_contacts;
    std::unordered_map<CharacterHandle, std::unique_ptr<CharacterEntry>> m_characters;
    CharacterHandle m_nextCharacter = 1;
};

} // namespace sage::physics
