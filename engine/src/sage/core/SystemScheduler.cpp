#include "sage/core/SystemScheduler.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "sage/core/EngineContext.h"
#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/jobs/JobSystem.h"

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
    Add(stage, std::move(name), std::move(fn), SystemAccess{}, order);
}

void SystemScheduler::Add(Stage stage, std::string name, SystemFn fn, SystemAccess access, int order) {
    if (!fn) return;
    // Повторная регистрация ЗАМЕНЯЕТ: перезапуск Play переподключает скрипты и
    // физику, и без замены планировщик копил бы дубликаты каждый заход.
    for (Entry& e : m_systems) {
        if (e.Name == name) {
            e.InStage = stage;
            e.Order = order;
            e.Fn = std::move(fn);
            e.Access = std::move(access);
            m_dirty = true;
            return;
        }
    }
    m_systems.push_back({stage, order, std::move(name), std::move(fn), std::move(access)});
    m_dirty = true;
}

bool SystemScheduler::Remove(const std::string& name) {
    const auto it = std::find_if(m_systems.begin(), m_systems.end(),
                                 [&](const Entry& e) { return e.Name == name; });
    if (it == m_systems.end()) return false;
    m_systems.erase(it);
    m_dirty = true;  // граф держит индексы — перестроить
    return true;
}

void SystemScheduler::Clear() {
    m_systems.clear();
    m_timings.clear();
    m_dirty = true;
}

void SystemScheduler::Sort() {
    // Устойчивая сортировка: системы одной стадии с одинаковым порядком идут
    // так, как их зарегистрировали. Неустойчивая давала бы кадр, который
    // зависит от реализации сортировки — то есть невоспроизводимый.
    std::stable_sort(m_systems.begin(), m_systems.end(), [](const Entry& a, const Entry& b) {
        if (a.InStage != b.InStage) return (int)a.InStage < (int)b.InStage;
        return a.Order < b.Order;
    });
    BuildGraph();
    m_dirty = false;
}

void SystemScheduler::RunOne(const Entry& e, Scene& scene, float dt, bool profileScope) {
    // Объявление доступа — потоку на время системы: помощники ECS сверяются
    // с ним (см. SystemAccess.h, «Проверка в работе»).
    ScopedSystemAccess access(&e.Access, e.Name.c_str());
    try {
        if (profileScope) {
            // Каждая система — участок профилировщика, а не только строка в
            // LastTimings. Иначе профилировщик показывал бы «Обновление 9 мс»
            // одним куском рядом с расписанными по проходам «Тени 0.4» и
            // «Bloom 1.1» — и на вопрос «что съело кадр» ответа не было бы
            // ровно в той половине, где живёт игровая логика. (Профилировщик
            // кадра — только главного потока; воркерные системы видны в
            // LastTimings и на шкале панели «Потоки».)
            sage::profile::Scope scope(e.Name.c_str());
            e.Fn(scene, dt);
        } else {
            e.Fn(scene, dt);
        }
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
}

void SystemScheduler::RunSequential(size_t begin, size_t end, Scene& scene, float dt) {
    for (size_t t = begin; t < end; ++t) {
        const Entry& e = m_systems[(size_t)m_topo[t]];
        const auto start = m_profiling ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        RunOne(e, scene, dt, true);
        if (m_profiling) {
            const float ms = std::chrono::duration<float, std::milli>(
                                 std::chrono::steady_clock::now() - start).count();
            m_timings.push_back({e.Name, e.InStage, ms, false});
        }
    }
}

void SystemScheduler::Run(Scene& scene, float dt) {
    if (m_dirty) Sort();
    if (m_profiling) m_timings.clear();
    SAGE_PROFILE("Системы");

    // Стадии — по очереди (барьер между ними); внутри стадии — граф, если
    // есть кому идти на воркеры, иначе ровно прежний последовательный проход.
    sage::JobSystem* js = nullptr;
    if (m_parallel)
        if (EngineContext* ctx = EngineContext::CurrentOrNull()) js = &ctx->Jobs();
    const bool canParallel = js && js->Enabled() && js->WorkerCount() > 0;
    size_t b = 0;
    while (b < m_systems.size()) {
        size_t e = b;
        bool anyWorker = false;
        while (e < m_systems.size() && m_systems[e].InStage == m_systems[b].InStage) {
            anyWorker |= !m_systems[e].Access.MainThread;
            ++e;
        }
        if (canParallel && anyWorker) RunStageGraph(b, e, scene, dt);
        else RunSequential(b, e, scene, dt);
        b = e;
    }
}

std::vector<std::string> SystemScheduler::Order() const {
    // Граф построен — порядок с учётом RunAfter (он может переставить системы
    // внутри стадии); иначе — по стадиям и Order.
    if (!m_dirty && m_topo.size() == m_systems.size()) {
        std::vector<std::string> names;
        names.reserve(m_topo.size());
        for (int i : m_topo) names.push_back(m_systems[(size_t)i].Name);
        return names;
    }
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

} // namespace sage
