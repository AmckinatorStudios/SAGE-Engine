#include "sage/physics/PhysicsScene.h"

#include <algorithm>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <sstream>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/MeshData.h"
#include "sage/render/ModelLoader.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

using namespace sage::physics;

// Персонажи в физике сцены: контакты, подъём, шаг, возврат в сцену (см. PhysicsScene.h).

// КАСАНИЯ ПЕРСОНАЖА -> OnCollisionEnter/Exit.
//
// Контроллер — не тело мира, и очередь контактов бэкенда о нём молчит: персонаж
// упирался в стену, вставал на ящик, бился головой о балку, а скрипт не узнавал
// ни о чём. Бэкенд отвечает на вопрос «кого касается», разница с прошлым шагом
// — это вход и выход. Пары — те же EntityContact, что у тел, поэтому скриптам
// всё равно, тело ударилось или персонаж.
void PhysicsScene::UpdateCharacterContacts(Scene& scene) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> now;
    for (const auto& [entity, handle] : m_characters) {
        if (!reg.valid(entity)) continue;
        auto* cc = reg.try_get<CharacterControllerComponent>(entity);
        if (!cc) continue;
        if (!cc->CollisionEvents) {
            cc->Touching.clear();
            continue;
        }
        m_world->CharacterContacts(handle, m_touchBuffer);
        now.clear();
        for (BodyHandle body : m_touchBuffer) {
            auto it = m_bodyToEntity.find(body);
            if (it == m_bodyToEntity.end() || it->second == entity) continue;
            if (std::find(now.begin(), now.end(), it->second) == now.end()) now.push_back(it->second);
        }
        auto emit = [&](entt::entity other, bool begin) {
            EntityContact c;
            c.Begin = begin;
            c.A = entity;
            c.B = other;
            m_contacts.push_back(c);
        };
        for (entt::entity other : now)
            if (std::find(cc->Touching.begin(), cc->Touching.end(), other) == cc->Touching.end())
                emit(other, true);
        for (entt::entity other : cc->Touching)
            if (std::find(now.begin(), now.end(), other) == now.end()) emit(other, false);
        cc->Touching.swap(now);
    }
}

// Заводит контроллеры для сущностей, у которых есть компонент, но ещё нет
// контроллера, и снимает осиротевшие — ровно как SyncBodies для тел.
void PhysicsScene::SyncCharacters(Scene& scene) {
    if (!m_world || !m_world->SupportsCharacters()) return;

    auto view = scene.Registry().view<CharacterControllerComponent, Transform>();
    for (auto e : view) {
        CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        if (cc.Runtime != kInvalidCharacter) continue;
        // У персонажа СВОЙ мир (игра сама отвечает, где твердь) — физике он не
        // принадлежит, и заводить ему тело в бэкенде значило бы столкнуть два
        // разных представления мира в одном теле.
        if (cc.Solid) continue;
        Transform& tr = view.get<Transform>(e);
        // Персонаж стоит прямо: капсула контроллера всегда вертикальна, а
        // наклон, оставшийся от гизмо, давал бы картинку и дочерней камере
        // крен, которого у самого персонажа нет. Скрипт крутит только рыскание.
        tr.Rotation.x = 0.0f;
        tr.Rotation.z = 0.0f;
        CharacterDesc d;
        d.Radius = cc.Radius;
        d.Height = cc.Height;
        d.StepHeight = cc.StepOffset;
        d.MaxSlopeDeg = cc.SlopeLimit;
        d.SkinWidth = cc.SkinWidth;
        d.Mass = cc.Mass;
        d.Layer = cc.Layer;
        d.CollidesWith = cc.CollidesWith;
        // Позиция контроллера — ПОДОШВЫ; точка объекта может быть не там
        // (начало координат модели — в поясе, в центре, где угодно).
        d.Position = tr.Position + cc.Center;
        cc.Runtime = m_world->CreateCharacter(d);
        if (cc.Runtime != kInvalidCharacter) m_characters.emplace_back(e, cc.Runtime);
    }

    size_t alive = 0;
    for (size_t i = 0; i < m_characters.size(); ++i) {
        const auto& [entity, handle] = m_characters[i];
        const CharacterControllerComponent* cc =
            scene.Registry().valid(entity)
                ? scene.Registry().try_get<CharacterControllerComponent>(entity)
                : nullptr;
        if (cc && cc->Runtime == handle) m_characters[alive++] = m_characters[i];
        else m_world->RemoveCharacter(handle);
    }
    m_characters.resize(alive);
}

// --- ШАГ КОНТРОЛЛЕРОВ ПЕРСОНАЖА ---------------------------------------------
//
// ЗАЧЕМ ОН ЗДЕСЬ, А НЕ В СКРИПТЕ. Контроллеру нужно ровно то, чего у скрипта
// нет: шаг физики и результат столкновения того же шага. Пока тяготение писала
// игра, каждая игра писала его заново — и каждая чуть-чуть иначе: у одной
// падение линейное, у другой прыжок зависит от частоты кадров, у третьей на
// стыке плит дрожание. Это не разнообразие, это одна и та же ошибка, набранная
// заново столько раз, сколько есть игр.
//
// Что делает шаг: копит вертикальную скорость тяготением, прилипает к опоре
// (иначе персонаж отрывается на каждой выпуклости пола и «летит» вниз по
// лестнице), сбрасывает вертикаль при приземлении и об потолок, а на склоне
// круче предела превращает ход в скольжение вниз.
void PhysicsScene::StepCharacters(Scene& scene, float dt) {
    if (dt <= 0.0f) return;
    auto view = scene.Registry().view<CharacterControllerComponent, Transform>();
    for (auto e : view) {
        CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        Transform& tr = view.get<Transform>(e);
        // Игра ведёт этого персонажа сама (старый sage.physics.MoveCharacter) —
        // второе, движковое тяготение удвоило бы её собственное.
        if (!cc.Managed) continue;
        const bool ownWorld = (bool)cc.Solid && cc.Motor;
        if (!ownWorld && cc.Runtime == sage::physics::kInvalidCharacter) continue;

        // ТЕЛО ПЕРСОНАЖА СТОИТ ПРЯМО. Капсула контроллера всегда вертикальна, и
        // наклон X/Z у её объекта — это не поза, а ошибка: так бывает, когда
        // скрипт ставит телу наклон взгляда (камера на самом теле) или гизмо
        // качнуло объект. Тогда вместе со взглядом заваливалась вся модель.
        // Рыскание (Y) — поворот тела, оно остаётся; наклоняется камера —
        // дочерний объект (см. player_fps.lua).
        if (tr.Rotation.x != 0.0f || tr.Rotation.z != 0.0f) {
            static bool told = false;
            if (!told) {
                told = true;
                LOG_WARN("Physics") << "Объект с контроллером персонажа не наклоняется — наклон X/Z снят. "
                                       "Смотреть вверх-вниз должна дочерняя камера, а не тело";
            }
            tr.Rotation.x = 0.0f;
            tr.Rotation.z = 0.0f;
        }

        // --- Вертикаль -------------------------------------------------------
        if (cc.Grounded && cc.VerticalVelocity <= 0.0f) {
            // Небольшая прижимающая скорость, а не ноль: с нулём персонаж на
            // каждом стыке пола отрывается от опоры на кадр, и «стоит на земле»
            // мигает — вместе с ним мигают звук шагов и анимация приземления.
            cc.VerticalVelocity = -2.0f;
        } else {
            cc.VerticalVelocity += cc.Gravity * dt;
            // Предел падения: без него за долгое падение скорость дорастает до
            // величины, на которой шаг перепрыгивает пол целиком.
            const float kTerminal = 55.0f;
            if (cc.VerticalVelocity < -kTerminal) cc.VerticalVelocity = -kTerminal;
        }

        glm::vec3 velocity = cc.HasRequest ? cc.DesiredVelocity : glm::vec3(0.0f);
        velocity.y += cc.VerticalVelocity;

        // --- Склон круче предела: не идём, а съезжаем ------------------------
        if (cc.Grounded) {
            const float up = glm::clamp(cc.GroundNormal.y, -1.0f, 1.0f);
            const float slopeDeg = glm::degrees(std::acos(up));
            if (slopeDeg > cc.SlopeLimit) {
                // Направление вниз по склону — проекция «вниз» на плоскость
                // опоры. Склон, по которому нельзя идти, обязан сносить, иначе
                // на нём можно стоять, просто не двигаясь, — и тогда предел
                // склона не значит ничего.
                glm::vec3 down = glm::vec3(0.0f, -1.0f, 0.0f);
                glm::vec3 slide = down - cc.GroundNormal * glm::dot(down, cc.GroundNormal);
                const float len = glm::length(slide);
                if (len > 1e-4f) {
                    slide /= len;
                    const float speed = std::abs(cc.Gravity) * 0.5f;
                    velocity.x = slide.x * speed;
                    velocity.z = slide.z * speed;
                }
            }
        }

        const glm::vec3 before = ownWorld ? cc.Motor->State().Position : tr.Position + cc.Center;

        if (ownWorld) {
            sage::physics::CharacterDesc d = cc.Motor->Desc();
            d.Radius = cc.Radius;
            d.Height = cc.Height;
            d.StepHeight = cc.StepOffset;
            d.MaxSlopeDeg = cc.SlopeLimit;
            d.SkinWidth = cc.SkinWidth;
            d.Position = cc.Motor->State().Position;
            cc.Motor->Configure(d);
            cc.Motor->Move(cc.Solid, velocity, dt);
            const sage::physics::CharacterState& st = cc.Motor->State();
            tr.Position = st.Position - cc.Center;
            cc.Grounded = st.Grounded;
            cc.GroundNormal = st.GroundNormal;
            cc.Landed = cc.Motor->Landed();
            cc.LeftGround = cc.Motor->LeftGround();
            cc.Blocked = cc.Motor->Blocked();
            cc.StepUp = cc.Motor->StepUp();
            cc.Velocity = (st.Position - before) / dt;
            StopAtCeiling(cc, cc.Velocity.y);
        } else {
            // Маска — каждый шаг: её правят в инспекторе посреди Play.
            m_world->SetCharacterCollidesWith(cc.Runtime, cc.CollidesWith);
            m_world->MoveCharacter(cc.Runtime, velocity, dt);
            // Положение и опора приедут в PullCharacters после шага мира —
            // читать их здесь значило бы видеть состояние ДО столкновения.
            cc.Velocity = velocity;
        }

        // Встали на опору — падение обнуляется (потолок — StopAtCeiling, по
        // фактическому ходу после шага).
        if (cc.Grounded && cc.VerticalVelocity < 0.0f) cc.VerticalVelocity = 0.0f;

        // Запрос действует ОДИН кадр: персонаж, которому забыли сказать «иди»,
        // обязан остановиться сам, а не ехать вечно.
        cc.HasRequest = false;
        cc.DesiredVelocity = glm::vec3(0.0f);
    }
}

// Переносит положение контроллеров в Transform сущностей и обновляет флаг
// опоры. После шага: до него положение ведёт игра (через MoveCharacter), после
// — физика, и путать эти два момента значит терять то одно, то другое.
// ПОТОЛОК ГАСИТ ПРЫЖОК. Персонаж летит вверх, а поднялся меньше половины
// запрошенного — значит, упёрся головой. Скорость вверх тогда обнуляется
// сразу: иначе прыжок в низкий проём «прилипал» к потолку, пока гравитация
// не съест набранную скорость (при 8 м/с — почти секунду под плитой).
void PhysicsScene::StopAtCeiling(CharacterControllerComponent& cc, float actualUp) {
    if (cc.VerticalVelocity > 0.0f && actualUp < cc.VerticalVelocity * 0.5f) cc.VerticalVelocity = 0.0f;
}

void PhysicsScene::PullCharacters(Scene& scene, float dt) {
    if (!m_world || !m_world->SupportsCharacters()) return;
    for (const auto& [entity, handle] : m_characters) {
        if (!scene.Registry().valid(entity)) continue;
        auto* cc = scene.Registry().try_get<CharacterControllerComponent>(entity);
        auto* tr = scene.Registry().try_get<Transform>(entity);
        if (!cc || !tr) continue;
        const sage::physics::CharacterState st = m_world->GetCharacterState(handle);
        if (dt > 0.0f) {
            // Скорость — фактическая, а не запрошенная: игре нужно «как
            // персонаж на самом деле движется» (анимация, звук шагов), а
            // упёршийся в стену не идёт, сколько бы его ни просили.
            cc->Velocity = (st.Position - (tr->Position + cc->Center)) / dt;
            StopAtCeiling(*cc, cc->Velocity.y);
        }
        tr->Position = st.Position - cc->Center;
        cc->Grounded = st.Grounded;
        cc->GroundNormal = st.GroundNormal;
    }
}
