#pragma once
#include <memory>
#include "sage/physics/PhysicsWorld.h"

class Scene;

// ---------------------------------------------------------------------------
// PhysicsScene — мост между ECS-сценой и физическим миром (аналог того, как
// ScriptEngine связывает сцену со скриптами). Создаётся на время симуляции
// (Play-режим редактора / игра): в конструкторе строит тела для всех сущностей
// с RigidBodyComponent, каждый кадр Step() шагает мир и синхронизирует:
//   • Dynamic  — позиция/поворот тела -> Transform сущности;
//   • Kinematic — Transform сущности -> тело (двигается скриптом);
//   • Static   — неподвижно.
// Форма берётся из ColliderComponent (или единичный бокс по Transform.Scale),
// размеры масштабируются на Transform.Scale.
// ---------------------------------------------------------------------------
class PhysicsScene {
public:
    PhysicsScene(sage::physics::Backend backend, Scene& scene);

    void Step(Scene& scene, float dt);

    bool Available() const { return m_world && m_world->IsAvailable(); }
    const char* BackendName() const { return m_world ? m_world->BackendName() : "None"; }
    int BodyCount() const { return m_bodyCount; }

    // --- Рантайм-управление телами по хэндлу (для скриптинга) ----------------
    // ScriptEngine достаёт BodyHandle из RigidBodyComponent.RuntimeBody сущности
    // и рулит её скоростью/гравитацией мира прямо из Lua. Невалидный хэндл или
    // Null-бэкенд — безопасный no-op / нулевая скорость.
    void SetLinearVelocity(sage::physics::BodyHandle body, const glm::vec3& v) {
        if (m_world) m_world->SetLinearVelocity(body, v);
    }
    glm::vec3 GetLinearVelocity(sage::physics::BodyHandle body) const {
        return m_world ? m_world->GetLinearVelocity(body) : glm::vec3(0.0f);
    }
    void AddImpulse(sage::physics::BodyHandle body, const glm::vec3& impulse) {
        if (m_world) m_world->AddImpulse(body, impulse);
    }
    void SetGravity(const glm::vec3& g) {
        if (m_world) m_world->SetGravity(g);
    }
    bool SupportsJoints() const { return m_world && m_world->SupportsJoints(); }
    int JointCount() const { return m_jointCount; }

private:
    std::unique_ptr<sage::physics::PhysicsWorld> m_world;
    int m_bodyCount = 0;
    int m_jointCount = 0;
};
