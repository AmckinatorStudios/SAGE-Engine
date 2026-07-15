#pragma once
#include "../scene/Scene.h"
#include "../core/InputMap.h"
#include <sol/sol.hpp>
#include <vector>
#include <string>

// Система скриптинга на Lua. Позволяет управлять поведением GameObject'ов
// и логикой игры БЕЗ перекомпиляции движка — правишь .lua файл, перезапускаешь
// игру. Часть ЯДРА движка (не зависит от вокселей и конкретной игры).
//
// Помимо базового Transform/Color скрипты получают доступ к:
//   - Vec3 с арифметикой (+, -, *, длина, нормализация) — для игровой математики
//   - Sql/Spawn/Find/Destroy объектов сцены — можно порождать/убирать сущности из Lua
//   - именованным действиям ввода движка (см. core/InputMap.h) — IsActionDown и т.п.
//   - Schedule/Repeat — отложенные и повторяющиеся вызовы без хранения таймера вручную
//   - StartCoroutine + wait(seconds) — последовательности во времени (катсцены,
//     волны спавна, задержки) пишутся как обычный линейный Lua-код
//
// Использование в игре:
//   ScriptEngine scripts;
//   scripts.BindScene(scene);      // опционально — даёт Spawn/Find/Destroy
//   scripts.BindInput(inputMap);   // опционально — даёт IsActionDown и т.п.
//   scripts.AttachScript(myObject, "assets/scripts/spin.lua");
//   ...
//   scripts.UpdateAll(deltaTime); // вызывать каждый кадр — тикает и скрипты, и таймеры/корутины
//
// Пример скрипта (assets/scripts/spin.lua):
//   function OnUpdate(entity, dt)
//       entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 30.0
//   end
class ScriptEngine {
public:
    ScriptEngine();

    // Даёт скриптам доступ к сцене: SpawnObject/FindObject/DestroyObject
    // из Lua будут работать с этой сценой. Без BindScene эти функции
    // бросают ошибку при вызове из Lua — понятную, не сегфолт.
    void BindScene(Scene& scene) { m_scene = &scene; }

    // Даёт скриптам доступ к именованным действиям ввода движка:
    // IsActionDown("Jump"), WasActionPressed("BreakOrHook") и т.п.
    void BindInput(InputMap& input) { m_input = &input; }

    // Загружает .lua файл и привязывает его к объекту. Скрипт должен
    // определить глобальную функцию OnUpdate(entity, dt) — она будет
    // вызываться каждый кадр из UpdateAll(). Необязательная OnStart(entity)
    // вызывается один раз сразу при загрузке.
    void AttachScript(GameObject& object, const std::string& scriptPath);

    // Выполняет .lua файл как самостоятельный скрипт уровня игры, не
    // привязанный ни к одному объекту — например, "правила уровня",
    // спавнер волн врагов, скрипт-дирижёр катсцены. Если файл определяет
    // OnUpdate(dt) — она будет вызываться каждый кадр так же, как для
    // объектных скриптов, только без параметра entity.
    void RunScript(const std::string& scriptPath);

    // Вызывает OnUpdate для всех привязанных объектных и уровневых
    // скриптов, а также тикает отложенные вызовы (Schedule/Repeat) и
    // резюмирует активные корутины (StartCoroutine/wait).
    void UpdateAll(float deltaTime);

    // Доступ к состоянию Lua — на случай если игре нужно зарегистрировать
    // свою собственную API-функцию/тип в дополнение к базовой
    sol::state& Lua() { return m_lua; }

private:
    struct ScriptInstance {
        GameObject* Object; // nullptr для уровневых скриптов (RunScript)
        sol::environment Env;
        sol::protected_function UpdateFn; // может быть невалидной, если OnUpdate не определён
        std::string Path; // для сообщений об ошибках
    };

    struct ScheduledCall {
        int Id = 0;
        float TimeLeft = 0.0f;
        float Interval = 0.0f; // > 0 для Repeat, 0 для одноразового Schedule
        bool Repeating = false;
        bool Cancelled = false;
        sol::protected_function Fn;
    };

    struct CoroutineInstance {
        sol::coroutine Co;
        float WaitTime = 0.0f; // сколько ещё ждать перед следующим resume
    };

    void RegisterEngineApi();
    void UpdateTimers(float dt);
    void UpdateCoroutines(float dt);

    sol::state m_lua;
    std::vector<ScriptInstance> m_instances;
    std::vector<ScheduledCall> m_scheduled;
    std::vector<CoroutineInstance> m_coroutines;

    Scene* m_scene = nullptr;
    InputMap* m_input = nullptr;

    int m_nextTimerId = 1;
};
