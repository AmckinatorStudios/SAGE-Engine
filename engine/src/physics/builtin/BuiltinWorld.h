#pragma once
#include <unordered_map>
#include <vector>
#include "sage/physics/PhysicsWorld.h"

namespace sage::physics {

// BuiltinWorld — встроенный лёгкий физический бэкенд без внешних зависимостей.
// Доступен всегда (в т.ч. когда Jolt не собран). Честный, но нарочно
// линейный (без вращения) импульсный решатель:
//   • гравитация + полу-неявное интегрирование скорости/позиции;
//   • ЧЕСТНЫЙ контакт динамика-динамика И динамика-статика/кинематика через
//     последовательные импульсы (sequential impulses): нормальный импульс с
//     упругостью + кулоновское трение по касательной, позиционная коррекция
//     с распределением по обратной массе — динамика реально расталкивается и
//     складывается в стопки, тяжёлое давит на лёгкое;
//   • формы: настоящие Sphere и Capsule (осевой отрезок вдоль Y + радиус,
//     через ближайшие точки отрезков) + AABB-бокс; составное тело приближается
//     объемлющим AABB;
//   • несколько итераций решателя на под-шаг — для устойчивых стопок/куч.
// Сознательно НЕ моделируется: вращательная динамика (тела не кувыркаются) и
// соединения (joints/ragdoll) — для этого выбирают Backend::Jolt. Этого хватает
// для аркадной физики предметов, снарядов, куч ящиков и толкания игроком.
class BuiltinWorld : public PhysicsWorld {
public:
    const char* BackendName() const override { return "Builtin"; }
    bool IsAvailable() const override { return true; }
    void SetGravity(const glm::vec3& gravity) override { m_gravity = gravity; }

    BodyHandle CreateBody(const BodyDesc& desc) override;
    void RemoveBody(BodyHandle body) override;
    void Step(float dt) override;

    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const override;
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) override;
    glm::vec3 GetLinearVelocity(BodyHandle body) const override;
    void AddImpulse(BodyHandle body, const glm::vec3& impulse) override;

    JointHandle CreateJoint(const JointDesc& desc) override; // не поддержаны (warn+invalid)
    void RemoveJoint(JointHandle joint) override;
    bool SupportsJoints() const override { return false; }

private:
    // Форма коллайдера: AABB-бокс, сфера или вертикальная капсула
    // (отрезок вдоль Y + радиус). Составное тело и капсула-как-бокс уходят в Box.
    enum class Shape : uint8_t { Box, Sphere, Capsule };

    struct Body {
        BodyType Type;
        Shape Kind = Shape::Box;
        glm::vec3 Half{0.5f};  // AABB-полуразмеры (Box / bound составного тела)
        float Radius = 0.5f;   // Sphere / Capsule
        float HalfHeight = 0.5f; // Capsule: полу-высота цилиндрической части (ось Y)
        glm::vec3 Position;
        glm::quat Rotation;
        glm::vec3 Velocity{0.0f};
        float InvMass;         // 0 для static/kinematic
        float Friction;
        float Restitution;
        bool Alive = true;
    };

    // Контакт двух тел: нормаль направлена от A к B, Depth — глубина проникновения.
    struct Contact {
        Body* A;
        Body* B;
        glm::vec3 Normal;
        float Depth;
    };

    void SubStep(float dt);

    // Генерация контакта (нормаль от A к B) и последовательные импульсы —
    // static, работают только с полями тел.
    static bool Collide(const Body& A, const Body& B, glm::vec3& normal, float& depth);
    static void SolveVelocity(const Contact& c);
    static void CorrectPosition(const Contact& c);

    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    std::unordered_map<BodyHandle, Body> m_bodies;
    BodyHandle m_next = 1;
    float m_accum = 0.0f;
    bool m_warnedNoJoints = false; // предупреждение о неподдержке joints — один раз
};

} // namespace sage::physics
