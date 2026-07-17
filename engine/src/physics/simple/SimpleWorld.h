#pragma once
#include <unordered_map>
#include <vector>
#include "sage/physics/PhysicsWorld.h"

namespace sage::physics {

// SimpleWorld — встроенный лёгкий физический бэкенд без внешних зависимостей.
// Доступен всегда (в т.ч. когда Jolt не собран). Возможности сознательно
// ограничены аркадным уровнем:
//   • гравитация + интегрирование скорости динамических тел;
//   • столкновение динамики со СТАТИКОЙ/КИНЕМАТИКОЙ как AABB (сфера/капсула
//     приближаются боксом), разрешение по оси минимального проникновения с
//     упругостью и трением;
//   • без вращательной динамики (тела не кувыркаются) и без честного
//     контакта динамика-динамика — для этого есть Jolt.
// Этого достаточно, чтобы предметы падали, приземлялись и складывались на
// полу/платформах; для полноценной физики выбирают Backend::Jolt.
class SimpleWorld : public PhysicsWorld {
public:
    const char* BackendName() const override { return "Simple"; }
    bool IsAvailable() const override { return true; }
    void SetGravity(const glm::vec3& gravity) override { m_gravity = gravity; }

    BodyHandle CreateBody(const BodyDesc& desc) override;
    void RemoveBody(BodyHandle body) override;
    void Step(float dt) override;

    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const override;
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) override;
    glm::vec3 GetLinearVelocity(BodyHandle body) const override;

private:
    struct Body {
        BodyType Type;
        glm::vec3 Half;      // AABB-полуразмеры коллайдера (приближение формы)
        glm::vec3 Position;
        glm::quat Rotation;
        glm::vec3 Velocity{0.0f};
        float InvMass;       // 0 для static/kinematic
        float Friction;
        float Restitution;
        bool Alive = true;
    };

    void SubStep(float dt);

    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    std::unordered_map<BodyHandle, Body> m_bodies;
    BodyHandle m_next = 1;
    float m_accum = 0.0f;
};

} // namespace sage::physics
