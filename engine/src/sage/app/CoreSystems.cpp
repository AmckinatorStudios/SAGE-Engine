#include "sage/app/CoreSystems.h"

#include <exception>

#include "sage/anim/AnimationSystem.h"
#include "sage/anim/Tween.h"
#include "sage/audio/AudioEngine.h"
#include "sage/audio/AudioSystem.h"
#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/ecs/DayNightCycle.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/ParticleSystem.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"

namespace sage {

// ---------------------------------------------------------------------------
//  Стандартный набор
// ---------------------------------------------------------------------------
//
// ЕДИНСТВЕННОЕ место, где записан порядок кадра движка. Каждая строка ниже —
// ответ на вопрос «почему именно здесь», и ответы разные:

void RegisterCoreSystems(SystemScheduler& scheduler, const CoreSystems& systems) {
    // Скрипты — ДО физики. Они задают намерение (скорость, импульс, спавн), а
    // физика его исполняет в этом же кадре. Наоборот — и заданная скриптом
    // скорость применяется на кадр позже: управление «чуть залипает», и найти
    // это по симптому почти невозможно.
    if (systems.Scripts) {
        ScriptEngine* scripts = systems.Scripts;
        scheduler.Add(Stage::Update, "scripts", [scripts](Scene&, float dt) {
            // События кадра — перед OnUpdate: реакция на удар обязана попасть в
            // тот же кадр, что и удар. Контакты берутся с прошлого шага физики.
            scripts->DispatchFrameEvents();
            scripts->UpdateFixed(dt);
            scripts->UpdateAll(dt);
        });
    }

    // Скрипты объектов — рядом с уровневыми и по той же причине: намерение
    // задаётся до физики. Порядок внутри кадра записан ЗДЕСЬ целиком:
    // столкновения прошлого шага, постоянный шаг, обычный Update.
    if (systems.Scripting) {
        sage::scripting::ScriptingSystem* scripting = systems.Scripting;
        PhysicsScene* physics = systems.Physics;
        scheduler.Add(Stage::Update, "scripting", [scripting, physics](Scene& scene, float dt) {
            // Удар обязан дойти до скрипта в ТОМ ЖЕ кадре, в котором случился,
            // иначе взрыв отстаёт от столкновения на кадр — и это видно.
            if (physics) scripting->DispatchPhysicsEvents(*physics, scene);
            scripting->FixedUpdate(dt);
            scripting->Update(dt);
        }, 1);
    }

    if (systems.Physics) {
        PhysicsScene* physics = systems.Physics;
        scheduler.Add(Stage::Physics, "physics", [physics](Scene& scene, float dt) {
            physics->Step(scene, dt);
        });
    }

    // LateUpdate — после физики и анимации: камере, следящей за персонажем,
    // нужна его ОКОНЧАТЕЛЬНАЯ позиция этого кадра. Позови её раньше — камера
    // отстаёт на кадр, и картинка «плывёт» именно при беге.
    if (systems.Scripting) {
        sage::scripting::ScriptingSystem* scripting = systems.Scripting;
        scheduler.Add(Stage::PostPhysics, "scripting.late", [scripting](Scene&, float dt) {
            scripting->LateUpdate(dt);
        }, 20);
    }

    // Время суток — после скриптов и физики: скрипт вправе поставить час
    // (`Cycle.Time = 18`) в Update, и солнце этого же кадра обязано встать уже
    // по нему, а не по прошлому.
    if (systems.DayNight) {
        scheduler.Add(Stage::PostPhysics, "daynight", [](Scene& scene, float dt) {
            sage::ecs::UpdateDayNight(scene, dt);
        }, 15);
    }

    // Твины — после скриптов (Tween.to этого кадра даёт движение уже в нём) и
    // до анимации клипов: у одного свойства клип главнее — ключевой кадр
    // записан человеком, а твин обычно запущен «поверх» на мгновение.
    // Регистрируются всегда: зависимостей у них нет, а без системы твины из
    // редактора молча стояли бы на месте.
    scheduler.Add(Stage::PostPhysics, "tweens", [](Scene& scene, float dt) {
        sage::anim::UpdateTweens(scene, dt);
    }, 5);

    // Анимация — ПОСЛЕ физики. Её второй проход (IK) ставит ноги на землю, а
    // где земля, известно только после шага симуляции; до него IK работал бы
    // по позициям прошлого кадра.
    if (systems.Animation) {
        scheduler.Add(Stage::PostPhysics, "animation", [](Scene& scene, float dt) {
            sage::anim::UpdateAnimators(scene, dt);
            // Клипы по свойствам — тем же шагом и той же системой: движение
            // кнопки и движение персонажа отличаются тем, ЧТО они двигают, а
            // не тем, когда в кадре их считать.
            sage::anim::UpdatePropertyAnimators(scene, dt);
        });
    }

    // Частицы — после всего, что двигает мир: эмиттер стоит там, где его
    // сущность оказалась В ИТОГЕ, а не там, где была до физики.
    if (systems.Particles) {
        ParticleSystem* particles = systems.Particles;
        // Столкновения частиц с миром — лучом в физику сцены, если она есть.
        // Без неё частицы сталкиваются только с плоскостью из своих настроек.
        // Без физики запрос снимается: иначе остался бы указатель на физику
        // прошлого запуска.
        particles->SetCollisionQuery({});
        if (PhysicsScene* physics = systems.Physics) {
            particles->SetCollisionQuery([physics](const glm::vec3& from, const glm::vec3& to,
                                                   sage::fx::CollisionHit& hit) {
                const glm::vec3 d = to - from;
                const float len = glm::length(d);
                if (len < 1e-6f) return false;
                const PhysicsScene::EntityHit h = physics->Raycast(from, d / len, len);
                if (!h.Hit) return false;
                hit.Point = h.Point;
                hit.Normal = h.Normal;
                return true;
            });
        }
        scheduler.Add(Stage::Effects, "particles", [particles](Scene& scene, float dt) {
            sage::fx::UpdateEmitters(scene, *particles, dt);
        });
    }

    // Звук последним: он читает позиции источников и слушателя, и читать их
    // надо окончательные — иначе панорама отстаёт от картинки на кадр.
    if (systems.Audio) {
        AudioEngine* audio = systems.Audio;
        scheduler.Add(Stage::Effects, "audio", [audio](Scene& scene, float) {
            // Источники сцены (AudioSourceComponent) — здесь же, а не отдельной
            // системой, и по той же причине: они читают мировые позиции, и
            // читать их надо после того, как всё в кадре доехало на свои места.
            // Отдельная система означала бы ещё одно место, которое обязан
            // зарегистрировать КАЖДЫЙ потребитель движка, — а забывший её
            // получил бы сцену, где звук просто не играет, без единого
            // сообщения о причине.
            sage::audio::Update(scene, *audio);
            audio->Update();
        }, 10);
    }
}

} // namespace sage
