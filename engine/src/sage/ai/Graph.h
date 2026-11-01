#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// ДОКУМЕНТ ВИЗУАЛЬНОГО ГРАФА — то, что лежит в файле .aigraph / .sgraph и что
// правит редактор. Здесь только данные: что за узлы, где стоят, чем соединены.
// Как граф ВЫПОЛНЯЕТСЯ — в GraphRuntime.h; какие бывают узлы — в Nodes.h.
//
// ДВА ИНСТРУМЕНТА, у каждого свой файл, свой редактор, свой набор узлов и
// свои правила выполнения:
//   AI Graph (.aigraph)     — поведение ИИ: задачи, условия, контекст и
//                             управление поведением (вид behavior);
//   Script Graph (.sgraph)  — игровая логика объектов: события, поток,
//                             переменные, игровые действия (вид script) и
//                             переиспользуемые функции (вид function).
// Вид записан в файле, но решает его РАСШИРЕНИЕ (CheckGraphFile): файл
// .aigraph с видом script — ошибка загрузки, а не «граф, который сам
// догадается, кто он».
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

// ВИДЫ ГРАФОВ:
//   Behavior — AI Graph: поведение ИИ (выбор задачи, её выполнение, результат).
//   Script   — Script Graph: игровая логика объекта (On Start, On Update, …).
//   Function — функция Script Graph: переиспользуемый блок игровой логики.
//   Task     — УСТАРЕВШИЙ граф задачи (.vgraph прежних версий). Новые не
//              создаются; старые файлы загружаются и работают, как работали,
//              а задачи теперь собираются в AI Graph или пишутся на SAGE Lua.
enum class GraphKind { Behavior, Task, Function, Script };
const char* GraphKindId(GraphKind kind);
bool ParseGraphKind(const std::string& id, GraphKind& out);

// НАЗНАЧЕНИЕ УЗЛА — у каждого графа СВОЙ перечень узлов:
//   AI     — только AI Graph: задачи поведения, условия ИИ, контекст ИИ,
//            управление поведением (Task Start, Succeed, Sequence задач,
//            Decide, Move To Target, …);
//   Logic  — игровая логика Script Graph: поток (Branch, Sequence, циклы),
//            переменные, математика, игровые действия, функции;
//   Object — события объекта (On Start, On Update, столкновения) — только в
//            Script Graph объекта, не в функции.
// Общего «на всякий случай» набора нет: узел Script Graph не появляется в
// AI Graph только потому, что он уже существует, и наоборот. Оба бита несёт
// лишь Reroute — точка излома провода, не логика.
enum NodeDomain : unsigned {
    kDomainAI = 1u,
    kDomainLogic = 2u,
    kDomainObject = 4u,
    kDomainScript = kDomainLogic | kDomainObject,   // узел Script Graph (visual.node)
    kDomainAll = kDomainAI | kDomainLogic | kDomainObject,
};
// Можно ли узлу с таким назначением стоять в графе этого вида.
bool NodeAllowed(unsigned nodeDomains, GraphKind kind);
// Назначение графа этого вида.
unsigned GraphDomain(GraphKind kind);

// ФАЙЛ И ЕГО ВИД. Расширение однозначно задаёт вид графа и редактор:
//   .aigraph — только behavior (AI Graph);
//   .sgraph  — script или function (Script Graph);
//   .vgraph  — устаревший файл Visual Script: script, function или task.
// Несовпадение (ручная правка файла, переименование) — ошибка с причиной,
// а не попытка угадать назначение по содержимому.
bool CheckGraphFile(const std::string& path, GraphKind kind, std::string& error);
// Расширение нового файла этого вида (task — устаревший, новых не бывает).
const char* GraphFileExt(GraphKind kind);
// Файл графа по расширению (любой из трёх).
bool IsGraphFile(const std::string& path);

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
    // AI Graph (и устаревший граф задачи): какие каналы персонажа занимает и
    // можно ли его прервать (TaskSignature::Channels / Interruptible).
    std::vector<std::string> Channels;
    bool Interruptible = true;
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
