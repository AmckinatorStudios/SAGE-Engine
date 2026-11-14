#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/core/SystemAccess.h"

class Scene;

// ---------------------------------------------------------------------------
// Планировщик систем кадра.
//
// ЗАЧЕМ. Порядок «скрипты -> физика -> анимация -> частицы -> звук» до этого
// был записан РУКАМИ в каждом потребителе: рантайм, редактор, игры. Копии
// разошлись — и не гипотетически: в одной игре скрипты вызывались ПОСЛЕ шага
// физики, то есть заданная скриптом скорость применялась на кадр позже, чем в
// рантайме. Такую разницу невозможно заметить в редакторе и мучительно ловить
// в игре, потому что «иногда управление немного залипает» — это не сообщение
// об ошибке.
//
// Ровно та же болезнь уже была у проходов рендера, и там её вылечил
// ScenePasses: последовательность записана ОДИН раз. Здесь то же лекарство для
// логики кадра.
//
// СТАДИИ ФИКСИРОВАНЫ, порядок внутри стадии задаётся числом. Свободный список
// «систем с приоритетами» выглядит гибче, но на деле означает, что порядок
// зависит от того, кто в каком порядке зарегистрировался, — а именно этого мы
// и избегаем. Стадия отвечает на вопрос «до или после физики», и ответ на него
// не должен зависеть от порядка вызовов Add.
//
// Использование:
//     SystemScheduler sched;
//     RegisterCoreSystems(sched, {.Scripts = &scripts, .Physics = &physics});
//     sched.Add(Stage::Update, "мойИИ", [](Scene& s, float dt) { ... });
//     ...
//     sched.Run(scene, dt);   // каждый кадр
//
// ПАРАЛЛЕЛЬНО ВНУТРИ СТАДИИ. Система, объявившая доступ (SystemAccess.h),
// становится узлом графа стадии: она ждёт только тех более ранних систем, с
// которыми конфликтует по данным (или которых назвала в RunAfter), и идёт на
// воркерах вместе с независимыми. Система без объявления эксклюзивна и идёт в
// главном потоке — то есть старый код ведёт себя ровно как раньше. Граница
// стадий остаётся барьером: всё из предыдущей стадии закончено до начала
// следующей.
//
//     sched.Add(Stage::PostPhysics, "boids", UpdateBoids,
//               SystemAccess::Parallel().Read<Transform>().Write<Boid>());
// ---------------------------------------------------------------------------
namespace sage {

// Стадии кадра в порядке выполнения. Между ними — гарантия: всё из предыдущей
// стадии выполнено полностью.
enum class Stage {
    // Ввод, таймеры, всё, что готовит данные для игровой логики.
    PreUpdate,
    // Игровая логика: скрипты, ИИ, правила. Здесь задают скорости и намерения.
    Update,
    // Шаг симуляции. После него позиции тел — уже новые.
    Physics,
    // То, что обязано видеть РЕЗУЛЬТАТ физики: анимация (её IK ставит ноги на
    // ту землю, которую посчитала физика), камера следования, привязки.
    PostPhysics,
    // Частицы, звук — потребители готового состояния мира.
    Effects,
    // Последнее перед отрисовкой: интерфейс, отладочная визуализация.
    LateUpdate,
};

const char* StageName(Stage stage);

class SystemScheduler {
public:
    // Система кадра. Сцена и шаг времени — всё, что ей полагается: система,
    // которой нужно что-то ещё, захватывает это замыканием, и движку незачем
    // знать, что именно.
    using SystemFn = std::function<void(Scene& scene, float dt)>;

    // order — порядок ВНУТРИ стадии (меньше идёт раньше). Имена уникальны:
    // повторная регистрация под тем же именем ЗАМЕНЯЕТ систему, а не добавляет
    // вторую. Иначе перезапуск Play-режима, который переподключает скрипты,
    // копил бы их дубликаты кадр за кадром.
    void Add(Stage stage, std::string name, SystemFn fn, int order = 0);
    // С объявлением доступа — кандидат в параллельное выполнение.
    void Add(Stage stage, std::string name, SystemFn fn, SystemAccess access, int order = 0);
    bool Remove(const std::string& name);
    void Clear();

    // Выполняет все системы по стадиям и порядку. Исключение из системы не
    // роняет кадр: остальные обязаны отработать, а сообщение уходит в лог —
    // упавший скрипт не должен уносить с собой физику и рендер.
    void Run(Scene& scene, float dt);

    int Count() const { return (int)m_systems.size(); }
    // Имена систем в порядке выполнения — для отладки и тестов.
    std::vector<std::string> Order() const;

    // Сколько миллисекунд заняла каждая система в последнем кадре. Профайлер
    // движка знает только про «Обновление» целиком; здесь видно, КАКАЯ система
    // съела кадр, а это и есть первый вопрос при просадке.
    struct Timing {
        std::string Name;
        Stage InStage = Stage::Update;
        float Milliseconds = 0.0f;
        bool OnWorker = false;   // выполнялась на воркере (не в главном потоке)
    };
    const std::vector<Timing>& LastTimings() const { return m_timings; }
    void SetProfilingEnabled(bool on) { m_profiling = on; }

    // Параллельное выполнение независимых систем (по умолчанию включено; без
    // систем, объявивших доступ, ничего не меняет). Выключенное — строго
    // последовательно в порядке Order(), как до графа.
    void SetParallel(bool on) { m_parallel = on; }
    bool Parallel() const { return m_parallel; }

    // Граф кадра — для профилировщика и тестов: узел, где он выполняется и
    // от кого ждёт (индексы в том же списке; рёбра только внутри стадии).
    struct GraphNode {
        std::string Name;
        Stage InStage = Stage::Update;
        bool MainThread = true;
        bool Exclusive = true;
        std::string Access;            // объявление словами
        std::vector<int> DependsOn;
    };
    std::vector<GraphNode> Graph();
    // Ошибка построения графа (цикл в RunAfter, неизвестное имя) — "" если нет.
    const std::string& GraphError() const { return m_graphError; }

private:
    struct Entry {
        Stage InStage = Stage::Update;
        int Order = 0;
        std::string Name;
        SystemFn Fn;
        SystemAccess Access;
    };
    void Sort();
    void BuildGraph();
    void RunSequential(size_t begin, size_t end, Scene& scene, float dt);
    void RunStageGraph(size_t begin, size_t end, Scene& scene, float dt);
    void RunOne(const Entry& e, Scene& scene, float dt, bool profileScope);

    std::vector<Entry> m_systems;
    std::vector<std::vector<int>> m_deps;   // для каждой системы — от кого ждёт
    std::vector<std::vector<int>> m_succ;   // и кто ждёт её
    std::vector<int> m_topo;                // порядок внутри стадий (индексы)
    std::vector<Timing> m_timings;
    std::string m_graphError;
    bool m_profiling = true;
    bool m_dirty = false;
    bool m_parallel = true;
};

} // namespace sage
