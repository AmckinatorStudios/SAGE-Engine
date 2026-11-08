#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

// ---------------------------------------------------------------------------
// ФОНОВЫЕ ЗАДАЧИ СКРИПТОВ: тяжёлая чистая работа игры — на других ядрах.
//
// ЗАЧЕМ. Скрипты игры живут в одном состоянии Lua на главном потоке, и всё,
// что они считают, кадр ждёт: генерация участка мира, меш, поиск пути. На
// машине с 12 потоками работает один — игра тормозит «даже на мощном ПК».
// Само состояние Lua разделить между потоками нельзя (UB), поэтому задача
// исполняется в СВОЁМ состоянии воркера, а данные ходят копией.
//
// КАК УСТРОЕНО.
//   • Воркеров — по числу ядер минус два (главный поток и звук), не меньше
//     одного; у каждого своё состояние Lua, созданное при старте потока и
//     живущее до остановки — модули загружаются в него один раз.
//   • Модули задач — Lua-файлы игры: их текст читает ГЛАВНЫЙ поток (vfs:
//     пакет игры или диск) при Preload и раздаёт воркерам. Воркер не трогает
//     ни диск, ни пакет, ни сцену — только свой Lua и чистые функции движка
//     (шум, хэш, математика, строки, таблицы, log).
//   • Аргументы и результаты — КОПИИ (TaskValue): числа, строки, булевы,
//     таблицы (массивы чисел — плотным вектором, без накладных расходов на
//     элемент). Ссылок на объекты сцены и функций в копии нет — их и нельзя
//     отдавать в чужой поток.
//   • Порядок: результаты забираются по номеру задачи (Result), когда готовы;
//     главный поток не ждёт — опрашивает раз в кадр.
//
// Вызов из Lua — модуль tasks (ScriptApi_Tasks.cpp):
//     tasks.preload("gen")                          -- и все его require
//     local id = tasks.run("gen", "build_chunk", seed, cx, cz)
//     ... в следующих кадрах:
//     local ok, a, b = tasks.result(id)   -- nil — ещё считается
// ---------------------------------------------------------------------------
namespace sage::scripting {

struct TaskTable;

// Значение, которое можно передать между состояниями Lua (копией).
struct TaskValue {
    enum class Type : uint8_t { Nil, Bool, Int, Num, Str, Table };
    Type Kind = Type::Nil;
    bool B = false;
    int64_t I = 0;
    double N = 0.0;
    std::string S;
    std::shared_ptr<TaskTable> T;
};

struct TaskTable {
    // Плотная часть 1..n: если все элементы — целые (или все числа), они
    // хранятся вектором чисел, а не значений: участок мира — десятки тысяч
    // элементов, и общий путь стоил бы десятки мегабайт копий.
    std::vector<int64_t> Ints;
    std::vector<double> Nums;
    std::vector<TaskValue> Array;
    std::vector<std::pair<TaskValue, TaskValue>> Hash;
};

// Lua -> копия. Функции, userdata и потоки не переносятся (ошибка с путём к
// полю), глубина — не больше 64 (защита от циклов).
bool ToTaskValue(lua_State* L, int index, TaskValue& out, std::string& error);
// Копия -> Lua (кладёт на вершину стека).
void PushTaskValue(lua_State* L, const TaskValue& v);

enum class TaskState { Unknown, Pending, Done, Failed };

class ScriptTasks {
public:
    ScriptTasks();
    ~ScriptTasks();

    ScriptTasks(const ScriptTasks&) = delete;
    ScriptTasks& operator=(const ScriptTasks&) = delete;

    // Текст модуля для воркеров (имя — как у require). Повторный вызов с тем
    // же именем заменяет текст для новых воркеров и тех, кто ещё не загрузил.
    void Preload(const std::string& name, const std::string& chunkName, const std::string& source);
    bool HasModule(const std::string& name) const;

    // Поставить задачу: функция fn модуля module с аргументами args.
    // Возвращает номер (> 0).
    int64_t Submit(const std::string& module, const std::string& fn, std::vector<TaskValue> args);

    // Состояние задачи; для Done — результаты, для Failed — текст ошибки.
    // Забранная (Done/Failed) задача удаляется.
    TaskState Take(int64_t id, std::vector<TaskValue>& results, std::string& error);

    int Workers() const;
    int Pending() const;          // поставлено и ещё не забрано
    double BusyMs() const;        // суммарное время работы воркеров (мс, растёт)
    void SetWorkerCount(int n);   // 0 — авто; до первой задачи

    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sage::scripting
