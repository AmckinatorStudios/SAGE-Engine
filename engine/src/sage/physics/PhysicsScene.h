#pragma once
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>
#include "sage/physics/PhysicsWorld.h"

class Scene;

namespace sage::physics {
// Касание «капсула персонажа (ось [a, b], радиус r) — тело desc». Открыто ради
// теста: геометрия зон, по которой персонаж входит в триггер.
bool CapsuleTouchesBodyForTest(const glm::vec3& a, const glm::vec3& b, float r, const BodyDesc& desc);
}

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
    // Вращение и силы — см. PhysicsWorld (сила действует весь следующий Step).
    void SetAngularVelocity(sage::physics::BodyHandle body, const glm::vec3& w) {
        if (m_world) m_world->SetAngularVelocity(body, w);
    }
    glm::vec3 GetAngularVelocity(sage::physics::BodyHandle body) const {
        return m_world ? m_world->GetAngularVelocity(body) : glm::vec3(0.0f);
    }
    void AddImpulseAtPoint(sage::physics::BodyHandle body, const glm::vec3& impulse, const glm::vec3& point) {
        if (m_world) m_world->AddImpulseAtPoint(body, impulse, point);
    }
    void AddAngularImpulse(sage::physics::BodyHandle body, const glm::vec3& impulse) {
        if (m_world) m_world->AddAngularImpulse(body, impulse);
    }
    void AddForce(sage::physics::BodyHandle body, const glm::vec3& force) {
        if (m_world) m_world->AddForce(body, force);
    }
    void AddForceAtPoint(sage::physics::BodyHandle body, const glm::vec3& force, const glm::vec3& point) {
        if (m_world) m_world->AddForceAtPoint(body, force, point);
    }
    void AddTorque(sage::physics::BodyHandle body, const glm::vec3& torque) {
        if (m_world) m_world->AddTorque(body, torque);
    }
    bool IsSleeping(sage::physics::BodyHandle body) const { return m_world && m_world->IsSleeping(body); }
    // Мир целиком — для тестов и инструментов, которым нужен бэкенд напрямую.
    sage::physics::PhysicsWorld* World() { return m_world.get(); }

    // Геометрия коллайдера ConvexHull/Mesh сущности: точки уже с масштабом,
    // вершины сварены. nullptr — брать не из чего (нет меша, файл не открылся).
    // Кэшируется на время жизни сцены физики: сто одинаковых камней читают
    // модель один раз.
    sage::physics::MeshGeometryPtr ColliderGeometry(Scene& scene, entt::entity e, const glm::vec3& scale);
    void SetGravity(const glm::vec3& g) {
        if (m_world) m_world->SetGravity(g);
    }

    // ПЕРЕСТАВИТЬ ТЕЛО ТУДА, ГДЕ СТОИТ ЕГО СУЩНОСТЬ.
    //
    // Пока игра идёт, хозяин положения — физика: каждый шаг она пишет позу тела
    // в Transform. Поэтому правка Transform снаружи (гизмо в редакторе, скрипт,
    // телепорт) жила ровно до следующего кадра — объект возвращался, будто его
    // никто и не трогал, и выглядело это как «во время игры двигать нельзя».
    //
    // Скорость обнуляется намеренно: тело ПЕРЕСТАВИЛИ, а не бросили. Иначе
    // предмет, поднятый мышью на метр, улетает с той скоростью, которую набрал,
    // пока падал.
    void TeleportEntity(Scene& scene, entt::entity e);
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

    // --- Триггер-зоны --------------------------------------------------------
    //
    // КТО СЕЙЧАС ВНУТРИ, а не только «кто вошёл/вышел». Пары ведёт сама сцена,
    // а не бэкенд, и на то три причины:
    //  • персонаж (контроллер) — не тело мира, и ни один бэкенд не сообщал о
    //    его входе в зону: игрок проходил сквозь «кнопку» молча, хотя это
    //    самый частый случай триггера вообще;
    //  • удалённый объект уходил из зоны без «вышел» — счётчик «сколько
    //    внутри» у скрипта навсегда оставался на единицу больше;
    //  • маска слоёв зоны (RigidBodyComponent::TriggerMask) должна одинаково
    //    отсеивать гостей у всех бэкендов.
    // События Enter/Exit в Contacts() строятся по разнице этого списка между
    // шагами, так что расходиться с ним они не могут.
    struct TriggerOverlap {
        entt::entity Trigger = entt::null;   // зона (сенсор)
        entt::entity Other = entt::null;     // гость: тело или персонаж
        bool Entered = false;                // вошёл на этом шаге (для Stay — ещё не «внутри»)
    };
    const std::vector<TriggerOverlap>& TriggerOverlaps() const { return m_inside; }
    // Кто сейчас в зоне trigger. Возвращает число.
    int ObjectsInTrigger(entt::entity trigger, std::vector<entt::entity>& out) const;
    // В чьих зонах сейчас стоит объект. Возвращает число.
    int TriggersOf(entt::entity other, std::vector<entt::entity>& out) const;
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
    // Соединения: строит недостающие и пересобирает те, чьи тела сменились.
    void SyncJoints(Scene& scene);
    // Тяготение, прыжок, опора, склон и ступенька контроллеров персонажа.
    // Между «состав мира» и шагом мира: скрипт уже сказал, куда идти, а
    // столкновения ещё не посчитаны.
    void StepCharacters(Scene& scene, float dt);
    void PullCharacters(Scene& scene);
    // Пересчитывает m_inside по событиям сенсоров бэкенда и по персонажам и
    // дописывает в m_contacts разницу с прошлым шагом.
    void UpdateTriggers(Scene& scene);
    // Сообщение бэкенда о касании сенсора: пополняет/чистит m_bodyInside.
    void NoteSensorContact(Scene& scene, entt::entity a, entt::entity b, bool begin);

    std::unique_ptr<sage::physics::PhysicsWorld> m_world;
    // Что кому принадлежит: по этой паре Step() понимает, чьё тело осиротело.
    // Хранится вектором, а не картой: список короткий, обход идёт целиком
    // каждый кадр, и порядок в памяти важнее скорости точечного поиска.
    //
    // Рядом — снимок настроек, из которых тело построено. Правка в инспекторе
    // во время игры (масса, форма, замороженные оси, размер) раньше не делала
    // НИЧЕГО до следующего запуска: тело строилось один раз. Теперь SyncBodies
    // сверяет снимок и пересобирает тело на ходу, сохраняя его скорость.
    struct Tracked {
        entt::entity Entity = entt::null;
        sage::physics::BodyHandle Body = sage::physics::kInvalidBody;
        std::string Settings;   // см. SettingsKey в PhysicsScene.cpp
    };
    std::vector<Tracked> m_tracked;

    // Геометрия коллайдеров: исходная (по модели, без масштаба) и готовая (с
    // масштабом). Две ступени — чтобы растянутая копия камня не перечитывала
    // файл модели, а только пересчитывала точки.
    std::unordered_map<std::string, std::shared_ptr<const sage::physics::MeshGeometry>> m_meshSources;
    std::unordered_map<std::string, sage::physics::MeshGeometryPtr> m_meshScaled;
    int m_bodyCount = 0;
    int m_jointCount = 0;
    // Живые соединения: чьё оно и между какими телами построено. По телам
    // SyncJoints узнаёт, что соединение держится за пересобранное тело.
    struct JointLink {
        entt::entity Owner = entt::null;
        sage::physics::BodyHandle A = sage::physics::kInvalidBody, B = sage::physics::kInvalidBody;
    };
    std::unordered_map<sage::physics::JointHandle, JointLink> m_jointBodies;

    // Обратная карта «тело -> сущность» для перевода результатов запросов.
    // Строится в SyncBodies рядом с m_tracked: два источника правды о том, чьё
    // тело чьё, разошлись бы при первом же удалении сущности.
    std::unordered_map<sage::physics::BodyHandle, entt::entity> m_bodyToEntity;
    std::vector<EntityContact> m_contacts;
    std::vector<sage::physics::ContactEvent> m_rawContacts;   // буфер, чтобы не выделять каждый кадр
    std::vector<std::pair<entt::entity, sage::physics::CharacterHandle>> m_characters;
    // Пары «зона — гость», что касались по сообщениям бэкенда (тело с телом).
    // Отдельно от персонажей: те пересчитываются геометрией каждый шаг.
    std::vector<std::pair<entt::entity, entt::entity>> m_bodyInside;
    std::vector<TriggerOverlap> m_inside;
};
