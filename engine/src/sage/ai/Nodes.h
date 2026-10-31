#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sage/ai/Graph.h"
#include "sage/ai/Task.h"

// ---------------------------------------------------------------------------
// УЗЛЫ SAGE AI GRAPH: что узел умеет и какие у него пины.
//
// ТИП УЗЛА — ЗАПИСЬ В РЕЕСТРЕ, а не класс в редакторе. Встроенные узлы
// (поток, задачи, данные, математика) регистрирует ядро, узлы мира движка —
// слой скриптов, свои — проект из Lua (ai.node). Редактор ни одного узла не
// знает по имени: палитра, пины и окно свойств строятся по реестру. Поэтому
// новая функция движка или компонента попадает в граф без новой строки в
// редакторе.
//
// ПИНЫ БЫВАЮТ ПЕРЕМЕННЫМИ. У «Call Task» входы — входы вызываемой задачи, у
// «Make Struct» — поля типа, у «Sequence» — столько выходов, сколько задано.
// Поэтому кроме постоянных пинов у типа есть функция Pins: по настройкам
// узла и окружению (библиотека задач, типы) она достраивает список.
//
// ЧИСТЫЙ УЗЕЛ (Pure) — без потока выполнения: его выходы вычисляются, когда
// их спросил вход другого узла, и каждый раз заново. Так «расстояние до цели»
// в условии цикла всегда свежее, а не то, что было при входе в цикл.
// ---------------------------------------------------------------------------
namespace sage::ai {

class World;
class TypeRegistry;
class GraphExec;

struct PinSpec {
    std::string Name;               // ключ в файле и в проводах
    std::string Type = "any";       // тип (Types.h); "exec" — поток выполнения
    vars::Value Default;
    std::string Title;              // подпись (пусто — Name)
    std::string Tip;
};

// Настройка узла: показывается в окне свойств, на холсте пином не бывает.
// Kind подсказывает редактору, каким полем её править.
struct SettingSpec {
    std::string Name;
    std::string Kind = "string";   // string, int, float, bool, choice, task, operation, event, var, type, node
    vars::Value Default;
    std::string Title;
    std::vector<std::string> Choices;   // для choice
    std::string Tip;
};

struct NodePins {
    std::vector<PinSpec> Inputs, Outputs;
    bool Pure = false;
};

// Чем кончился вызов узла.
enum class Flow {
    Next,    // идти по выходу Pin (пусто или несоединён — ветка кончилась)
    Push,    // пройти ветку из выхода Pin и ВЕРНУТЬСЯ в этот узел (Sequence, Loop)
    Wait,    // ждать (ожидание назначено через GraphExec), потом позвать снова
    End,     // ветка выполнения завершена целиком
    Return,  // завершить весь граф (Return)
    Break,   // выйти из ближайшего цикла
};

struct NodeResult {
    Flow Kind = Flow::Next;
    std::string Pin;
    static NodeResult Go(std::string pin) { return {Flow::Next, std::move(pin)}; }
    static NodeResult Push(std::string pin) { return {Flow::Push, std::move(pin)}; }
    static NodeResult Wait() { return {Flow::Wait, {}}; }
    static NodeResult End() { return {Flow::End, {}}; }
    static NodeResult Return() { return {Flow::Return, {}}; }
    static NodeResult Break() { return {Flow::Break, {}}; }
};

// Что знает функция Pins: окружение графа.
struct GraphEnv {
    const World* W = nullptr;
    const TypeRegistry* Types = nullptr;
    const GraphDoc* Doc = nullptr;
};

struct NodeType {
    std::string Id;           // "flow.sequence"
    std::string Title;        // "Sequence"
    std::string Category;     // "Flow" — раздел палитры; "/" — подразделы
    std::string Description;
    std::string Origin = "C++";
    std::vector<PinSpec> Inputs, Outputs;
    std::vector<SettingSpec> Settings;
    bool Pure = false;
    bool Entry = false;       // точка входа (Start, On Event): у неё нет входа потока
    // Для какого редактора узел (NodeDomain в Graph.h): общий — обоим.
    unsigned Domains = kDomainAll;
    uint32_t Color = 0;       // 0 — цвет раздела
    std::vector<std::string> Keywords;   // для поиска в палитре

    // Достроить пины по узлу (см. «пины бывают переменными»). Получает уже
    // заполненные постоянные пины.
    std::function<void(const GraphNode&, const GraphEnv&, NodePins&)> Pins;
    // Выполнение (для чистого — вычисление выходов).
    std::function<NodeResult(GraphExec&)> Run;
    // Своё правило «можно ли подключить к этому пину провод типа other».
    // Возвращает false и причину — редактор не даст провести провод.
    std::function<bool(const GraphNode&, const std::string& pin, const std::string& other,
                       std::string& why)>
        CanConnect;
};

class NodeRegistry {
public:
    void Register(NodeType type);
    const NodeType* Find(const std::string& id) const;
    std::vector<const NodeType*> All() const;
    size_t Size() const { return m_types.size(); }
    uint64_t Version() const { return m_version; }

private:
    std::map<std::string, NodeType> m_types;
    uint64_t m_version = 1;
};

// Итоговые пины узла: постоянные + достроенные.
NodePins ResolvePins(const NodeType& type, const GraphNode& node, const GraphEnv& env);
const PinSpec* FindPin(const std::vector<PinSpec>& pins, const std::string& name);

// Встроенные узлы ядра (поток, задачи, события, переменные, данные).
void RegisterBuiltinNodes(NodeRegistry& nodes);

// Состояние узла в ОДНОМ выполнении графа: выходы последнего срабатывания,
// счётчик (Sequence, Loop), запущенная задача. У каждого выполнения своё —
// граф задачи «MoveTo» у десяти стражей не делит счётчиков.
struct NodeState {
    std::map<std::string, vars::Value> Outputs;
    int Counter = 0;
    int Limit = 0;
    bool Broken = false;
    double Until = 0.0;
    RunPtr Task;
    vars::Value Data;
    std::vector<int> Fibers;
    // Decide: активный вариант, его ветка, когда перепроверять условия,
    // пауза перед повтором каждого варианта, метка последнего события.
    int Active = -1;
    int ActiveFiber = 0;
    double NextEval = 0.0;
    std::vector<double> Cooldown;
    uint64_t EventMark = 0;
};

} // namespace sage::ai
