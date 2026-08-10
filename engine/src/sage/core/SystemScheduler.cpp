#include "sage/core/SystemScheduler.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "sage/anim/AnimationSystem.h"
#include "sage/audio/AudioEngine.h"
#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/render/ParticleECS.h"
#include "sage/scripting/ScriptEngine.h"

namespace sage {

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::PreUpdate:   return "PreUpdate";
        case Stage::Update:      return "Update";
        case Stage::Physics:     return "Physics";
        case Stage::PostPhysics: return "PostPhysics";
        case Stage::Effects:     return "Effects";
        case Stage::LateUpdate:  return "LateUpdate";
    }
    return "?";
}

void SystemScheduler::Add(Stage stage, std::string name, SystemFn fn, int order) {
    if (!fn) return;
    // Повторная регистрация ЗАМЕНЯЕТ: перезапуск Play переподключает скрипты и
    // физику, и без замены планировщик копил бы дубликаты каждый заход.
    for (Entry& e : m_systems) {
        if (e.Name == name) {
            e.InStage = stage;
            e.Order = order;
            e.Fn = std::move(fn);
            m_dirty = true;
            return;
        }
    }
    m_systems.push_back({stage, order, std::move(name), std::move(fn)});
    m_dirty = true;
}

bool SystemScheduler::Remove(const std::string& name) {
    const auto it = std::find_if(m_systems.begin(), m_systems.end(),
                                 [&](const Entry& e) { return e.Name == name; });
    if (it == m_systems.end()) return false;
    m_systems.erase(it);
    return true;
}

void SystemScheduler::Clear() {
    m_systems.clear();
    m_timings.clear();
}

void SystemScheduler::Sort() {
    // Устойчивая сортировка: системы одной стадии с одинаковым порядком идут
    // так, как их зарегистрировали. Неустойчивая давала бы кадр, который
    // зависит от реализации сортировки — то есть невоспроизводимый.
    std::stable_sort(m_systems.begin(), m_systems.end(), [](const Entry& a, const Entry& b) {
        if (a.InStage != b.InStage) return (int)a.InStage < (int)b.InStage;
        return a.Order < b.Order;
    });
    m_dirty = false;
}

void SystemScheduler::Run(Scene& scene, float dt) {
    if (m_dirty) Sort();
    if (m_profiling) m_timings.clear();
    SAGE_PROFILE("Системы");

    for (const Entry& e : m_systems) {
        const auto start = m_profiling ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        // Каждая система — участок профилировщика, а не только строка в
        // LastTimings. Иначе профилировщик показывал бы «Обновление 9 мс» одним
        // куском рядом с расписанными по проходам «Тени 0.4» и «Bloom 1.1» — и
        // на вопрос «что съело кадр» ответа не было бы ровно в той половине, где
        // живёт игровая логика.
        sage::profile::Scope scope(e.Name.c_str());
        try {
            e.Fn(scene, dt);
        } catch (const std::exception& ex) {
            // Упавшая система не уносит кадр: остальные обязаны отработать.
            // Скрипт с ошибкой не должен останавливать физику и рендер — иначе
            // одна опечатка в Lua превращается в чёрный экран вместо строки в
            // консоли.
            LOG_ERROR("Systems") << "Система '" << e.Name << "' (" << StageName(e.InStage)
                                 << ") выбросила исключение: " << ex.what();
        } catch (...) {
            LOG_ERROR("Systems") << "Система '" << e.Name << "' выбросила исключение";
        }
        if (m_profiling) {
            const auto end = std::chrono::steady_clock::now();
            const float ms =
                std::chrono::duration<float, std::milli>(end - start).count();
            m_timings.push_back({e.Name, e.InStage, ms});
        }
    }
}

std::vector<std::string> SystemScheduler::Order() const {
    std::vector<Entry> sorted = m_systems;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) {
        if (a.InStage != b.InStage) return (int)a.InStage < (int)b.InStage;
        return a.Order < b.Order;
    });
    std::vector<std::string> names;
    names.reserve(sorted.size());
    for (const Entry& e : sorted) names.push_back(e.Name);
    return names;
}

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

    if (systems.Physics) {
        PhysicsScene* physics = systems.Physics;
        scheduler.Add(Stage::Physics, "physics", [physics](Scene& scene, float dt) {
            physics->Step(scene, dt);
        });
    }

    // Анимация — ПОСЛЕ физики. Её второй проход (IK) ставит ноги на землю, а
    // где земля, известно только после шага симуляции; до него IK работал бы
    // по позициям прошлого кадра.
    if (systems.Animation) {
        scheduler.Add(Stage::PostPhysics, "animation", [](Scene& scene, float dt) {
            sage::anim::UpdateAnimators(scene, dt);
        });
    }

    // Частицы — после всего, что двигает мир: эмиттер стоит там, где его
    // сущность оказалась В ИТОГЕ, а не там, где была до физики.
    if (systems.Particles) {
        ParticleSystem* particles = systems.Particles;
        scheduler.Add(Stage::Effects, "particles", [particles](Scene& scene, float dt) {
            sage::fx::UpdateEmitters(scene, *particles, dt);
        });
    }

    // Звук последним: он читает позиции источников и слушателя, и читать их
    // надо окончательные — иначе панорама отстаёт от картинки на кадр.
    if (systems.Audio) {
        AudioEngine* audio = systems.Audio;
        scheduler.Add(Stage::Effects, "audio", [audio](Scene&, float) { audio->Update(); }, 10);
    }
}

} // namespace sage
