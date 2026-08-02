#pragma once
#include <memory>
#include <utility>
#include <unordered_map>
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

    // --- Запросы к миру в терминах СУЩНОСТЕЙ ---------------------------------
    //
    // Мир физики отвечает дескрипторами тел, а игра думает сущностями. Перевод
    // делается здесь и один раз: иначе каждый вызывающий держал бы свою карту
    // «тело -> сущность», и первая же расхождение с ней дало бы попадание в
    // «никуда» — самый неприятный вид ошибки, потому что он выглядит как
    // промах, а не как сбой.
    struct EntityHit {
        bool Hit = false;
        entt::entity Entity = entt::null;
        glm::vec3 Point{0.0f};
        glm::vec3 Normal{0.0f};
        float Distance = 0.0f;
    };
    EntityHit Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                      sage::physics::LayerMask mask = sage::physics::kAllLayers) const;
    int OverlapSphere(const glm::vec3& center, float radius, std::vector<entt::entity>& out,
                      sage::physics::LayerMask mask = sage::physics::kAllLayers) const;
    bool SupportsQueries() const { return m_world && m_world->SupportsQueries(); }

    // События столкновений за последний Step, уже переведённые в сущности.
    // Живут до следующего Step — игра читает их, когда ей удобно.
    struct EntityContact {
        bool Begin = true;
        entt::entity A = entt::null;
        entt::entity B = entt::null;
        glm::vec3 Point{0.0f};
        glm::vec3 Normal{0.0f};
        float Impulse = 0.0f;
        bool Sensor = false;
    };
    const std::vector<EntityContact>& Contacts() const { return m_contacts; }
    bool SupportsContacts() const { return m_world && m_world->SupportsContacts(); }

    // --- Контроллер персонажа ------------------------------------------------
    sage::physics::CharacterHandle CreateCharacter(const sage::physics::CharacterDesc& desc) {
        return m_world ? m_world->CreateCharacter(desc) : sage::physics::kInvalidCharacter;
    }
    void RemoveCharacter(sage::physics::CharacterHandle c) {
        if (m_world) m_world->RemoveCharacter(c);
    }
    void MoveCharacter(sage::physics::CharacterHandle c, const glm::vec3& v, float dt) {
        if (m_world) m_world->MoveCharacter(c, v, dt);
    }
    sage::physics::CharacterState CharacterState(sage::physics::CharacterHandle c) const {
        return m_world ? m_world->GetCharacterState(c) : sage::physics::CharacterState{};
    }
    void SetCharacterPosition(sage::physics::CharacterHandle c, const glm::vec3& p) {
        if (m_world) m_world->SetCharacterPosition(c, p);
    }
    bool SupportsCharacters() const { return m_world && m_world->SupportsCharacters(); }

private:
    // Заводит тела для сущностей, у которых есть RigidBodyComponent, но ещё нет
    // тела, и убирает тела сущностей, которых больше нет в сцене. Зовётся из
    // Step() каждый кадр — состав мира меняется скриптами на ходу.
    void SyncBodies(Scene& scene);
    void SyncCharacters(Scene& scene);
    void PullCharacters(Scene& scene);

    std::unique_ptr<sage::physics::PhysicsWorld> m_world;
    // Что кому принадлежит: по этой паре Step() понимает, чьё тело осиротело.
    // Хранится вектором, а не картой: список короткий, обход идёт целиком
    // каждый кадр, и порядок в памяти важнее скорости точечного поиска.
    std::vector<std::pair<entt::entity, sage::physics::BodyHandle>> m_tracked;
    int m_bodyCount = 0;
    int m_jointCount = 0;

    // Обратная карта «тело -> сущность» для перевода результатов запросов.
    // Строится в SyncBodies рядом с m_tracked: два источника правды о том, чьё
    // тело чьё, разошлись бы при первом же удалении сущности.
    std::unordered_map<sage::physics::BodyHandle, entt::entity> m_bodyToEntity;
    std::vector<EntityContact> m_contacts;
    std::vector<sage::physics::ContactEvent> m_rawContacts;   // буфер, чтобы не выделять каждый кадр
    std::vector<std::pair<entt::entity, sage::physics::CharacterHandle>> m_characters;
};
