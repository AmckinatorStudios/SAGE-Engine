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

private:
    std::unique_ptr<sage::physics::PhysicsWorld> m_world;
    int m_bodyCount = 0;
};
