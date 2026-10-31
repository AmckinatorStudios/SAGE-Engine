#include "sage/ai/Graph.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "sage/vars/ValueJson.h"

using json = nlohmann::json;

namespace sage::ai {

const char* GraphKindId(GraphKind kind) {
    switch (kind) {
        case GraphKind::Behavior: return "behavior";
        case GraphKind::Task: return "task";
        case GraphKind::Function: return "function";
        case GraphKind::Script: return "script";
    }
    return "task";
}

bool NodeAllowed(unsigned nodeDomains, GraphKind kind) {
    switch (kind) {
        case GraphKind::Function: return nodeDomains == kDomainAll;
        case GraphKind::Script: return (nodeDomains & kDomainScript) != 0;
        default: return (nodeDomains & kDomainAI) != 0;
    }
}

unsigned GraphDomain(GraphKind kind) {
    switch (kind) {
        case GraphKind::Function: return kDomainAll;
        case GraphKind::Script: return kDomainScript;
        default: return kDomainAI;
    }
}

bool ParseGraphKind(const std::string& id, GraphKind& out) {
    for (GraphKind k : {GraphKind::Behavior, GraphKind::Task, GraphKind::Function, GraphKind::Script}) {
        if (id == GraphKindId(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

GraphNode* GraphDoc::FindNode(int id) {
    for (GraphNode& n : Nodes)
        if (n.Id == id) return &n;
    return nullptr;
}

const GraphNode* GraphDoc::FindNode(int id) const {
    for (const GraphNode& n : Nodes)
        if (n.Id == id) return &n;
    return nullptr;
}

GraphNode& GraphDoc::AddNode(const std::string& type, float x, float y) {
    GraphNode n;
    n.Id = NextId++;
    n.Type = type;
    n.X = x;
    n.Y = y;
    Nodes.push_back(std::move(n));
    return Nodes.back();
}

void GraphDoc::RemoveNode(int id) {
    Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(), [id](const GraphNode& n) { return n.Id == id; }),
                Nodes.end());
    Links.erase(std::remove_if(Links.begin(), Links.end(),
                               [id](const GraphLink& l) { return l.From.Node == id || l.To.Node == id; }),
                Links.end());
}

GraphGroup& GraphDoc::AddGroup(const std::string& title, float x, float y, float w, float h) {
    GraphGroup g;
    g.Id = NextId++;
    g.Title = title;
    g.X = x;
    g.Y = y;
    g.W = w;
    g.H = h;
    Groups.push_back(std::move(g));
    return Groups.back();
}

void GraphDoc::RemoveGroup(int id) {
    Groups.erase(std::remove_if(Groups.begin(), Groups.end(), [id](const GraphGroup& g) { return g.Id == id; }),
                 Groups.end());
}

void GraphDoc::Link(const GraphPinRef& from, const GraphPinRef& to, bool exec) {
    if (from.Node == to.Node) return;   // петля на себя ничего не значит
    Links.erase(std::remove_if(Links.begin(), Links.end(),
                               [&](const GraphLink& l) {
                                   return exec ? l.From == from : l.To == to;
                               }),
                Links.end());
    Links.push_back({from, to});
}

void GraphDoc::Unlink(const GraphPinRef& pin) {
    Links.erase(std::remove_if(Links.begin(), Links.end(),
                               [&](const GraphLink& l) { return l.From == pin || l.To == pin; }),
                Links.end());
}

const GraphLink* GraphDoc::LinkInto(int node, const std::string& pin) const {
    for (const GraphLink& l : Links)
        if (l.To.Node == node && l.To.Pin == pin) return &l;
    return nullptr;
}

const GraphLink* GraphDoc::LinkFrom(int node, const std::string& pin) const {
    for (const GraphLink& l : Links)
        if (l.From.Node == node && l.From.Pin == pin) return &l;
    return nullptr;
}

std::vector<const GraphLink*> GraphDoc::LinksFrom(int node, const std::string& pin) const {
    std::vector<const GraphLink*> out;
    for (const GraphLink& l : Links)
        if (l.From.Node == node && l.From.Pin == pin) out.push_back(&l);
    return out;
}

const GraphVar* GraphDoc::FindVar(const std::vector<GraphVar>& list, const std::string& name) const {
    for (const GraphVar& v : list)
        if (v.Name == name) return &v;
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
namespace {

json VarsJson(const std::vector<GraphVar>& vars) {
    json out = json::array();
    for (const GraphVar& v : vars) {
        json j;
        j["name"] = v.Name;
        j["type"] = v.Type;
        j["default"] = sage::vars::ValueToJson(v.Default);
        if (!v.Description.empty()) j["description"] = v.Description;
        out.push_back(std::move(j));
    }
    return out;
}

void VarsFrom(const json& in, std::vector<GraphVar>& out) {
    out.clear();
    if (!in.is_array()) return;
    for (const json& j : in) {
        if (!j.is_object() || !j.contains("name")) continue;
        GraphVar v;
        v.Name = j.value("name", std::string());
        v.Type = j.value("type", std::string("float"));
        if (j.contains("default")) v.Default = sage::vars::ValueFromJson(j["default"]);
        v.Description = j.value("description", std::string());
        out.push_back(std::move(v));
    }
}

} // namespace

bool LoadGraph(const std::string& text, GraphDoc& out, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("файл графа не читается как JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "файл графа — не объект JSON";
        return false;
    }
    GraphDoc d;
    d.Format = j.value("format", 1);
    if (!ParseGraphKind(j.value("kind", std::string("task")), d.Kind)) d.Kind = GraphKind::Task;
    d.Name = j.value("name", std::string());
    d.Description = j.value("description", std::string());
    d.Category = j.value("category", std::string());
    VarsFrom(j.value("inputs", json()), d.Inputs);
    VarsFrom(j.value("outputs", json()), d.Outputs);
    VarsFrom(j.value("locals", json()), d.Locals);
    int maxId = 0;
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const json& nj : j["nodes"]) {
            if (!nj.is_object()) continue;
            GraphNode n;
            n.Id = nj.value("id", 0);
            n.Type = nj.value("type", std::string());
            n.X = nj.value("x", 0.0f);
            n.Y = nj.value("y", 0.0f);
            n.Comment = nj.value("comment", std::string());
            if (nj.contains("values") && nj["values"].is_object())
                for (const auto& [k, v] : nj["values"].items()) n.Values[k] = sage::vars::ValueFromJson(v);
            if (n.Id <= 0 || n.Type.empty()) continue;
            maxId = std::max(maxId, n.Id);
            d.Nodes.push_back(std::move(n));
        }
    }
    if (j.contains("links") && j["links"].is_array()) {
        for (const json& lj : j["links"]) {
            if (!lj.is_object()) continue;
            GraphLink l;
            l.From.Node = lj.value("from", 0);
            l.From.Pin = lj.value("from_pin", std::string());
            l.To.Node = lj.value("to", 0);
            l.To.Pin = lj.value("to_pin", std::string());
            // Провод к узлу, которого нет, — след ручной правки файла; без
            // узла он ничего не значит, и тащить его дальше незачем.
            if (!d.FindNode(l.From.Node) || !d.FindNode(l.To.Node)) continue;
            d.Links.push_back(std::move(l));
        }
    }
    if (j.contains("groups") && j["groups"].is_array()) {
        for (const json& gj : j["groups"]) {
            if (!gj.is_object()) continue;
            GraphGroup g;
            g.Id = gj.value("id", 0);
            g.Title = gj.value("title", std::string());
            g.X = gj.value("x", 0.0f);
            g.Y = gj.value("y", 0.0f);
            g.W = gj.value("w", 300.0f);
            g.H = gj.value("h", 200.0f);
            g.Color = gj.value("color", 0x40806040u);
            maxId = std::max(maxId, g.Id);
            d.Groups.push_back(std::move(g));
        }
    }
    d.NextId = std::max(j.value("next_id", 1), maxId + 1);
    out = std::move(d);
    return true;
}

std::string SaveGraph(const GraphDoc& d) {
    json j;
    j["format"] = d.Format;
    j["kind"] = GraphKindId(d.Kind);
    j["name"] = d.Name;
    if (!d.Description.empty()) j["description"] = d.Description;
    if (!d.Category.empty()) j["category"] = d.Category;
    j["inputs"] = VarsJson(d.Inputs);
    j["outputs"] = VarsJson(d.Outputs);
    j["locals"] = VarsJson(d.Locals);
    json nodes = json::array();
    for (const GraphNode& n : d.Nodes) {
        json nj;
        nj["id"] = n.Id;
        nj["type"] = n.Type;
        nj["x"] = n.X;
        nj["y"] = n.Y;
        if (!n.Comment.empty()) nj["comment"] = n.Comment;
        if (!n.Values.empty()) {
            json vals = json::object();
            for (const auto& [k, v] : n.Values) vals[k] = sage::vars::ValueToJson(v);
            nj["values"] = std::move(vals);
        }
        nodes.push_back(std::move(nj));
    }
    j["nodes"] = std::move(nodes);
    json links = json::array();
    for (const GraphLink& l : d.Links)
        links.push_back({{"from", l.From.Node}, {"from_pin", l.From.Pin}, {"to", l.To.Node}, {"to_pin", l.To.Pin}});
    j["links"] = std::move(links);
    json groups = json::array();
    for (const GraphGroup& g : d.Groups)
        groups.push_back({{"id", g.Id}, {"title", g.Title}, {"x", g.X}, {"y", g.Y}, {"w", g.W}, {"h", g.H},
                          {"color", g.Color}});
    j["groups"] = std::move(groups);
    j["next_id"] = d.NextId;
    return j.dump(2);
}

} // namespace sage::ai
