#pragma once
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>
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
    void AddImpulse(BodyHandle body, const glm::vec3& impulse) override;

    JointHandle CreateJoint(const JointDesc& desc) override; // не поддержаны (warn+invalid)
    void RemoveJoint(JointHandle joint) override;
    bool SupportsJoints() const override { return false; }

    // Луч по AABB тел (slab-метод). Форма здесь и так приближается коробкой,
    // поэтому луч точен ровно настолько же, насколько и вся эта физика.
    //
    // Реализован не «для галочки»: на Simple идут headless-тесты и сборки без
    // Jolt, и если бы луч работал только у Jolt, половина проверок движка не
    // могла бы его касаться вовсе.
    bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 RayHit& out, LayerMask mask) const override;
    int OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                      LayerMask mask) const override;
    bool SupportsQueries() const override { return true; }

    void PollContacts(std::vector<ContactEvent>& out) override;
    bool SupportsContacts() const override { return true; }

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
        LayerMask Layer = kLayerDefault;
        bool Sensor = false;
        bool Alive = true;
    };

    void SubStep(float dt);

    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    std::unordered_map<BodyHandle, Body> m_bodies;

    // Пары, которые касались на прошлом шаге, — по ним отличается «начал
    // касаться» от «продолжает». Без этого событие Begin приходило бы каждый
    // кадр, пока предмет лежит на полу, и «звук удара» звучал бы непрерывно.
    void NoteTouching(BodyHandle a, BodyHandle b, const Body& ba, const Body& bb);
    void NoteSeparated(BodyHandle a, BodyHandle b);
    std::set<std::pair<BodyHandle, BodyHandle>> m_touching;
    std::vector<ContactEvent> m_events;
    BodyHandle m_next = 1;
    float m_accum = 0.0f;
    bool m_warnedNoJoints = false; // предупреждение о неподдержке joints — один раз
};

} // namespace sage::physics
