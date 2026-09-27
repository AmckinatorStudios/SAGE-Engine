#pragma once
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
class Shape;
template <class T> class RefConst;
} // namespace JPH

namespace sage::physics {

class JoltBPLayerInterface;      // определены в .cpp
class JoltObjectVsBPFilter;
class JoltObjectLayerPairFilter;

// ---------------------------------------------------------------------------
// JoltWorld — основной физический бэкенд поверх jrouwe/JoltPhysics. Полноценная
// физика: динамика твёрдых тел с вращением, честные контакты (CCD, малый допуск
// проникновения), формы Box/Sphere/Capsule, составные, выпуклые оболочки и
// сетки треугольников из моделей. Реализует тот же интерфейс PhysicsWorld, что
// встроенный и Null — вызывающий код (PhysicsScene) их не различает.
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

    void SetAngularVelocity(BodyHandle body, const glm::vec3& w) override;
    glm::vec3 GetAngularVelocity(BodyHandle body) const override;
    void AddImpulseAtPoint(BodyHandle body, const glm::vec3& impulse, const glm::vec3& point) override;
    void AddAngularImpulse(BodyHandle body, const glm::vec3& impulse) override;
    void AddForce(BodyHandle body, const glm::vec3& force) override;
    void AddForceAtPoint(BodyHandle body, const glm::vec3& force, const glm::vec3& point) override;
    void AddTorque(BodyHandle body, const glm::vec3& torque) override;
    bool IsSleeping(BodyHandle body) const override;

    // Сколько внутренних шагов сделал последний Step (для тестов шага).
    int LastSubSteps() const { return m_lastSubSteps; }

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
    int m_lastSubSteps = 0;

    // Сила и момент, заказанные на следующий Step, и их ИМПУЛЬС, ещё не
    // отданный телу (кадр мог оказаться короче минимального шага — тогда
    // время и импульс копятся до следующего кадра, а не теряются).
    struct PendingForce {
        glm::vec3 Force{0.0f}, Torque{0.0f};
        glm::vec3 LinearImpulse{0.0f}, AngularImpulse{0.0f};
    };
    std::unordered_map<BodyHandle, PendingForce> m_forces;

    // Куда кинематическое тело должно прийти к концу следующего Step. Цель
    // раскладывается по подшагам (см. Step): один MoveKinematic на весь кадр
    // при двух подшагах уводил тело за цель на целый шаг, и следующий кадр
    // возвращал его обратно — платформа дрожала на 30 кадрах в секунду.
    struct KinematicTarget {
        glm::vec3 Position{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::unordered_map<BodyHandle, KinematicTarget> m_kinTargets;
    std::unordered_set<BodyHandle> m_kinMoving;

    // Готовые формы из геометрии моделей: одна и та же геометрия (сто
    // одинаковых камней) даёт одну форму, а не сто деревьев треугольников.
    // Указатель на геометрию держим вместе с формой, чтобы адрес-ключ не
    // освободился и не достался чужой геометрии.
    struct CachedShape;
    std::unordered_map<const void*, std::unique_ptr<CachedShape>> m_shapeCache;
    JPH::RefConst<JPH::Shape> ShapeFromGeometry(const BodyDesc& desc, bool dynamic);
    // Снимает соединение из системы и будит его тела.
    void ReleaseConstraint(JPH::Constraint* constraint);

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
