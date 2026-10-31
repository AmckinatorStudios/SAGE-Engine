#include "sage/ai/GraphValidate.h"

#include <set>

#include "sage/ai/World.h"

namespace sage::ai {

namespace {

const char* KindTitle(GraphKind k) {
    switch (k) {
        case GraphKind::Script: return "скрипта объекта (Visual Scripting)";
        case GraphKind::Task: return "задачи (Visual Scripting): управление задачами — в AI Graph";
        case GraphKind::Function: return "графа-функции (только общие узлы)";
        case GraphKind::Behavior: return "AI Graph: действия описываются в задачах";
    }
}

std::string Setting(const GraphNode& n, const char* key) {
    auto it = n.Values.find(key);
    return it == n.Values.end() ? std::string() : it->second.AsString();
}

} // namespace

std::vector<GraphIssue> ValidateGraph(const GraphDoc& doc, const World& world) {
    std::vector<GraphIssue> out;
    auto add = [&](int node, bool error, std::string text) { out.push_back({node, error, std::move(text)}); };
    const GraphEnv env{&world, &world.Types(), &doc};

    std::set<int> entries;
    int starts = 0;
    for (const GraphNode& n : doc.Nodes) {
        const NodeType* t = world.Nodes().Find(n.Type);
        if (!t) {
            add(n.Id, true, "неизвестный узел '" + n.Type + "' — не загружена библиотека, где он объявлен?");
            continue;
        }
        if (!NodeAllowed(t->Domains, doc.Kind))
            add(n.Id, true, "узел «" + t->Title + "» не для " + KindTitle(doc.Kind));
        if (t->Entry) entries.insert(n.Id);
        if (n.Type == "flow.start" || n.Type == "script.on_start") ++starts;

        // Настройки, без которых узел не работает.
        if (n.Type == "task.run" || n.Type == "task.call") {
            const std::string task = Setting(n, "task");
            if (task.empty()) add(n.Id, true, "не выбрана задача");
            else if (!world.FindTask(task)) add(n.Id, true, "нет задачи '" + task + "'");
        }
        if (n.Type == "graph.call") {
            const std::string fn = Setting(n, "function");
            std::shared_ptr<const GraphDoc> g = fn.empty() ? nullptr : world.FindGraph(fn);
            if (fn.empty()) add(n.Id, true, "не выбрана функция");
            else if (!g || g->Kind != GraphKind::Function) add(n.Id, true, "'" + fn + "' — не граф-функция");
        }
        if (n.Type == "call.op") {
            const std::string op = Setting(n, "operation");
            if (op.empty()) add(n.Id, true, "не выбрана операция");
            else if (!world.FindOperation(op)) add(n.Id, false, "операции '" + op + "' сейчас нет в библиотеке");
        }
        if (n.Type == "var.get" || n.Type == "var.set") {
            const std::string name = Setting(n, "name");
            std::string scope = Setting(n, "scope");
            if (scope.empty()) scope = "local";
            const std::vector<GraphVar>* list = scope == "local"    ? &doc.Locals
                                                : scope == "input"  ? &doc.Inputs
                                                : scope == "output" ? &doc.Outputs
                                                                    : nullptr;
            if (name.empty()) add(n.Id, true, "не задано имя переменной");
            else if (list && !doc.FindVar(*list, name))
                add(n.Id, true, "у графа нет переменной '" + name + "' (" + scope + ")");
        }
        if ((n.Type == "event.on" || n.Type == "event.wait" || n.Type == "event.emit") && Setting(n, "event").empty())
            add(n.Id, false, "не задано имя события");
    }

    // Провода: к существующим пинам и совместимых типов.
    for (const GraphLink& l : doc.Links) {
        const GraphNode* a = doc.FindNode(l.From.Node);
        const GraphNode* b = doc.FindNode(l.To.Node);
        const NodeType* ta = a ? world.Nodes().Find(a->Type) : nullptr;
        const NodeType* tb = b ? world.Nodes().Find(b->Type) : nullptr;
        if (!ta || !tb) continue;   // о неизвестном узле уже сказано
        const NodePins pa = ResolvePins(*ta, *a, env);
        const NodePins pb = ResolvePins(*tb, *b, env);
        const PinSpec* out = FindPin(pa.Outputs, l.From.Pin);
        const PinSpec* in = FindPin(pb.Inputs, l.To.Pin);
        if (!out || !in) {
            add(!in ? b->Id : a->Id, true, "провод к несуществующему пину '" + (!in ? l.To.Pin : l.From.Pin) + "'");
            continue;
        }
        std::string why;
        if (!world.Types().CanConnect(out->Type, in->Type, &why)) add(b->Id, true, why);
    }

    // Узел с потоком, к которому поток не ведёт, не выполнится никогда.
    std::set<int> reached;
    for (const GraphLink& l : doc.Links) {
        const GraphNode* b = doc.FindNode(l.To.Node);
        const NodeType* tb = b ? world.Nodes().Find(b->Type) : nullptr;
        if (!tb) continue;
        const NodePins pb = ResolvePins(*tb, *b, env);
        if (const PinSpec* in = FindPin(pb.Inputs, l.To.Pin); in && in->Type == type::Exec) reached.insert(b->Id);
    }
    for (const GraphNode& n : doc.Nodes) {
        const NodeType* t = world.Nodes().Find(n.Type);
        if (!t || t->Entry || entries.count(n.Id)) continue;
        const NodePins p = ResolvePins(*t, n, env);
        if (p.Pure) continue;
        bool hasExecIn = false;
        for (const PinSpec& pin : p.Inputs) hasExecIn |= pin.Type == type::Exec;
        if (hasExecIn && !reached.count(n.Id)) add(n.Id, false, "к узлу не ведёт поток — он не выполнится");
    }

    if (entries.empty())
        add(0, true, doc.Kind == GraphKind::Script ? "нет входа: On Start, On Update или On Event"
                                                   : "нет ни узла Start, ни обработчика событий");
    if (starts > 1) add(0, false, "узлов Start больше одного — выполнится первый");
    return out;
}

} // namespace sage::ai
