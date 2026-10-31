#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// ДОКУМЕНТ SAGE AI GRAPH — то, что лежит в файле .aigraph и что правит
// редактор. Здесь только данные: что за узлы, где стоят, чем соединены.
// Как граф ВЫПОЛНЯЕТСЯ — в GraphRuntime.h; какие бывают узлы — в Nodes.h.
//
// ТРИ ВИДА ГРАФА, одна система:
//   behavior — главный граф поведения: принимает решения и запускает задачи;
//   task     — граф задачи: последовательность действий одной задачи;
//   function — переиспользуемый блок, который зовут из других графов.
// Все три регистрируются задачами в библиотеке мира ИИ и зовутся по имени —
// из графа, из Lua и из C++ одинаково. Вид — это намерение автора (и
// раздел в палитре), а не разные правила выполнения.
//
// ПРОВОДА. У узла пины двух родов: поток выполнения (exec) и данные. Выход
// потока ведёт в ОДИН вход (куда идти дальше — однозначно), вход потока
// принимает сколько угодно. У данных наоборот: вход берёт значение из ОДНОГО
// выхода, выход раздаёт во сколько угодно входов.
//
// БОЛЬШИЕ ГРАФЫ. Группы (рамки с заголовком) и комментарии к узлам — не
// украшение: без них граф из сотни узлов читает только автор, и то неделю.
// ---------------------------------------------------------------------------
namespace sage::ai {

enum class GraphKind { Behavior, Task, Function };
const char* GraphKindId(GraphKind kind);
bool ParseGraphKind(const std::string& id, GraphKind& out);

struct GraphPinRef {
    int Node = 0;
    std::string Pin;
    bool operator==(const GraphPinRef& o) const { return Node == o.Node && Pin == o.Pin; }
};

struct GraphLink {
    GraphPinRef From;   // выход
    GraphPinRef To;     // вход
};

struct GraphNode {
    int Id = 0;
    std::string Type;   // id типа узла: "flow.sequence", "task.run", …
    float X = 0.0f, Y = 0.0f;
    // Значения несоединённых входов и настройки узла — одной таблицей: и то,
    // и другое «что вписано в узел руками».
    std::map<std::string, vars::Value> Values;
    std::string Comment;
};

struct GraphGroup {
    int Id = 0;
    std::string Title;
    float X = 0.0f, Y = 0.0f, W = 300.0f, H = 200.0f;
    uint32_t Color = 0x40806040;
};

// Переменная графа: вход, выход или локальная.
struct GraphVar {
    std::string Name;
    std::string Type = "float";
    vars::Value Default;
    std::string Description;
};

struct GraphDoc {
    int Format = 1;
    GraphKind Kind = GraphKind::Task;
    std::string Name;
    std::string Description;
    std::string Category;   // раздел в палитре для function-графов
    std::vector<GraphVar> Inputs, Outputs, Locals;
    std::vector<GraphNode> Nodes;
    std::vector<GraphLink> Links;
    std::vector<GraphGroup> Groups;
    int NextId = 1;

    GraphNode* FindNode(int id);
    const GraphNode* FindNode(int id) const;
    GraphNode& AddNode(const std::string& type, float x, float y);
    void RemoveNode(int id);
    GraphGroup& AddGroup(const std::string& title, float x, float y, float w, float h);
    void RemoveGroup(int id);

    // Провод из выхода в вход. exec — род пина (решает, какое правило
    // единственности применить): у потока единственен ВЫХОД, у данных — ВХОД.
    void Link(const GraphPinRef& from, const GraphPinRef& to, bool exec);
    void Unlink(const GraphPinRef& pin);   // все провода этого пина
    const GraphLink* LinkInto(int node, const std::string& pin) const;
    const GraphLink* LinkFrom(int node, const std::string& pin) const;   // первый
    std::vector<const GraphLink*> LinksFrom(int node, const std::string& pin) const;

    const GraphVar* FindVar(const std::vector<GraphVar>& list, const std::string& name) const;
};

// Чтение и запись .aigraph (JSON). Неизвестные поля пропускаются: граф из
// будущей версии открывается в нынешней с тем, что она понимает.
bool LoadGraph(const std::string& text, GraphDoc& out, std::string& error);
std::string SaveGraph(const GraphDoc& doc);

} // namespace sage::ai
