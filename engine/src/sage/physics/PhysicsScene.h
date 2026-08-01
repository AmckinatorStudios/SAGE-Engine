#pragma once
#include <memory>
#include <utility>
#include <vector>
#include <entt/entt.hpp>
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
//
// СОСТАВ МИРА ЖИВОЙ. Сущности появляются и исчезают ПОСЛЕ старта симуляции —
// скрипт спавнит мусор, обломки, брошенные предметы, — и каждая такая сущность
// с RigidBodyComponent должна получить тело, а исчезнувшая — его отдать. Раньше
// тела строились только в конструкторе: всё, что игра порождала в рантайме,
// физики не получало вовсе (и молча — компонент есть, тела нет), а удалённые
// сущности навсегда оставляли тело в мире. Теперь Step() сам сверяет состав
// (см. SyncBodies) — от игры не требуется ничего, кроме навесить компонент.
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
    // Заводит тела для сущностей, у которых есть RigidBodyComponent, но ещё нет
    // тела, и убирает тела сущностей, которых больше нет в сцене. Зовётся из
    // Step() каждый кадр — состав мира меняется скриптами на ходу.
    void SyncBodies(Scene& scene);

    std::unique_ptr<sage::physics::PhysicsWorld> m_world;
    // Что кому принадлежит: по этой паре Step() понимает, чьё тело осиротело.
    // Хранится вектором, а не картой: список короткий, обход идёт целиком
    // каждый кадр, и порядок в памяти важнее скорости точечного поиска.
    std::vector<std::pair<entt::entity, sage::physics::BodyHandle>> m_tracked;
    int m_bodyCount = 0;
    int m_jointCount = 0;
};
