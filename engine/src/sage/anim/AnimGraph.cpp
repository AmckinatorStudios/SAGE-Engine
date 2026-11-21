#include "sage/anim/AnimGraph.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/FormatVersion.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace sage::anim {

using json = nlohmann::json;

namespace {
constexpr int kGraphVersion = 1;
const sage::format::Spec kSpec{"sage_animgraph_version", kGraphVersion, "граф анимации", {}};

std::atomic<uint64_t> g_revision{0};

AnimNodeKind KindFromName(const std::string& name) {
    if (name == "output") return AnimNodeKind::Output;
    if (name == "clip") return AnimNodeKind::Clip;
    if (name == "blend") return AnimNodeKind::Blend;
    if (name == "float") return AnimNodeKind::Float;
    throw std::runtime_error("неизвестный вид узла: " + name);
}
} // namespace

// --- Описание узлов ---------------------------------------------------------

int AnimGraph::InputCount(AnimNodeKind kind) {
    switch (kind) {
        case AnimNodeKind::Output: return 1;
        case AnimNodeKind::Blend: return 3;
        default: return 0;
    }
}

AnimPinType AnimGraph::InputType(AnimNodeKind kind, int pin) {
    switch (kind) {
        case AnimNodeKind::Output: return pin == 0 ? AnimPinType::Pose : AnimPinType::None;
        case AnimNodeKind::Blend:
            if (pin == kBlendPinA || pin == kBlendPinB) return AnimPinType::Pose;
            return pin == kBlendPinWeight ? AnimPinType::Float : AnimPinType::None;
        default: return AnimPinType::None;
    }
}

AnimPinType AnimGraph::OutputType(AnimNodeKind kind) {
    switch (kind) {
        case AnimNodeKind::Clip:
        case AnimNodeKind::Blend: return AnimPinType::Pose;
        case AnimNodeKind::Float: return AnimPinType::Float;
        default: return AnimPinType::None;
    }
}

const char* AnimGraph::KindName(AnimNodeKind kind) {
    switch (kind) {
        case AnimNodeKind::Output: return "output";
        case AnimNodeKind::Clip: return "clip";
        case AnimNodeKind::Blend: return "blend";
        case AnimNodeKind::Float: return "float";
    }
    return "clip";
}

// --- Правка -----------------------------------------------------------------

AnimGraphNode* AnimGraph::Find(int id) {
    for (AnimGraphNode& n : Nodes)
        if (n.Id == id) return &n;
    return nullptr;
}

const AnimGraphNode* AnimGraph::Find(int id) const {
    for (const AnimGraphNode& n : Nodes)
        if (n.Id == id) return &n;
    return nullptr;
}

int AnimGraph::Add(AnimNodeKind kind, glm::vec2 pos) {
    if (kind == AnimNodeKind::Output && OutputNode() != 0) return 0;
    AnimGraphNode n;
    n.Id = NextId++;
    n.Kind = kind;
    n.Pos = pos;
    Nodes.push_back(n);
    return n.Id;
}

bool AnimGraph::Remove(int id) {
    const AnimGraphNode* n = Find(id);
    if (!n || n->Kind == AnimNodeKind::Output) return false;
    Links.erase(std::remove_if(Links.begin(), Links.end(),
                               [id](const AnimGraphLink& l) { return l.From == id || l.To == id; }),
                Links.end());
    Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(), [id](const AnimGraphNode& x) { return x.Id == id; }),
                Nodes.end());
    return true;
}

bool AnimGraph::CanLink(int from, int to, int pin, std::string* why) const {
    auto fail = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    const AnimGraphNode* a = Find(from);
    const AnimGraphNode* b = Find(to);
    if (!a || !b) return fail("Узла нет");
    if (from == to) return fail("Узел не подключается сам к себе");
    if (pin < 0 || pin >= InputCount(b->Kind)) return fail("У узла нет такого входа");
    const AnimPinType out = OutputType(a->Kind);
    if (out == AnimPinType::None || out != InputType(b->Kind, pin))
        return fail(out == AnimPinType::Float ? "Число подключается только ко входу веса"
                                              : "Поза подключается только ко входу позы");
    // Петля: если `to` уже питает `from`, провод замкнёт круг, и позу нельзя
    // будет вычислить. Обход вверх по входам `from`.
    std::vector<int> stack{from};
    std::vector<int> seen;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        if (cur == to) return fail("Провод замкнёт петлю");
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        for (const AnimGraphLink& l : Links)
            if (l.To == cur) stack.push_back(l.From);
    }
    return true;
}

bool AnimGraph::Connect(int from, int to, int pin) {
    if (!CanLink(from, to, pin)) return false;
    Disconnect(to, pin);
    Links.push_back({from, to, pin});
    return true;
}

void AnimGraph::Disconnect(int to, int pin) {
    Links.erase(std::remove_if(Links.begin(), Links.end(),
                               [to, pin](const AnimGraphLink& l) { return l.To == to && l.Pin == pin; }),
                Links.end());
}

const AnimGraphLink* AnimGraph::LinkInto(int to, int pin) const {
    for (const AnimGraphLink& l : Links)
        if (l.To == to && l.Pin == pin) return &l;
    return nullptr;
}

int AnimGraph::OutputNode() const {
    for (const AnimGraphNode& n : Nodes)
        if (n.Kind == AnimNodeKind::Output) return n.Id;
    return 0;
}

AnimGraphParam* AnimGraph::FindParam(const std::string& name) {
    for (AnimGraphParam& p : Params)
        if (p.Name == name) return &p;
    return nullptr;
}

const AnimGraphParam* AnimGraph::FindParam(const std::string& name) const {
    for (const AnimGraphParam& p : Params)
        if (p.Name == name) return &p;
    return nullptr;
}

AnimGraph NewAnimGraph() {
    AnimGraph g;
    g.Add(AnimNodeKind::Output, glm::vec2(360.0f, 120.0f));
    return g;
}

// --- Файл -------------------------------------------------------------------

std::string AnimGraphToJson(const AnimGraph& graph) {
    json root;
    sage::format::Stamp(root, kSpec);
    root["nextId"] = graph.NextId;
    if (!graph.PreviewModel.empty()) root["previewModel"] = graph.PreviewModel;

    json params = json::array();
    for (const AnimGraphParam& p : graph.Params)
        params.push_back({{"name", p.Name}, {"default", p.Default}, {"min", p.Min}, {"max", p.Max}});
    root["params"] = std::move(params);

    json nodes = json::array();
    for (const AnimGraphNode& n : graph.Nodes) {
        json j{{"id", n.Id}, {"kind", AnimGraph::KindName(n.Kind)}, {"pos", {n.Pos.x, n.Pos.y}}};
        // Пишется только то, что у узла этого вида значит что-то: файл читают
        // и люди, и лишнее поле «weight» у клипа заставляло бы гадать.
        switch (n.Kind) {
            case AnimNodeKind::Clip:
                j["clipPath"] = n.ClipPath;
                j["clipName"] = n.ClipName;
                j["loop"] = n.Loop;
                j["speed"] = n.Speed;
                break;
            case AnimNodeKind::Blend: j["weight"] = n.Weight; break;
            case AnimNodeKind::Float: j["param"] = n.Param; break;
            case AnimNodeKind::Output: break;
        }
        nodes.push_back(std::move(j));
    }
    root["nodes"] = std::move(nodes);

    json links = json::array();
    for (const AnimGraphLink& l : graph.Links) links.push_back({{"from", l.From}, {"to", l.To}, {"pin", l.Pin}});
    root["links"] = std::move(links);
    return root.dump(1, '\t');
}

AnimGraph AnimGraphFromJson(const std::string& text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error("не разобран как JSON: " + std::string(e.what()));
    }
    std::string err;
    if (!sage::format::Upgrade(root, kSpec, err)) throw std::runtime_error(err);

    AnimGraph g;
    g.PreviewModel = root.value("previewModel", std::string());
    for (const json& p : root.value("params", json::array())) {
        AnimGraphParam param;
        param.Name = p.value("name", std::string());
        param.Default = p.value("default", 0.0f);
        param.Min = p.value("min", 0.0f);
        param.Max = p.value("max", 1.0f);
        if (!param.Name.empty() && !g.FindParam(param.Name)) g.Params.push_back(param);
    }
    int maxId = 0;
    for (const json& j : root.value("nodes", json::array())) {
        AnimGraphNode n;
        n.Id = j.value("id", 0);
        n.Kind = KindFromName(j.value("kind", std::string()));
        if (n.Id <= 0 || g.Find(n.Id)) throw std::runtime_error("неверный или повторный id узла");
        if (n.Kind == AnimNodeKind::Output && g.OutputNode() != 0) continue;   // второй выход не нужен
        const json pos = j.value("pos", json::array({0.0f, 0.0f}));
        if (pos.is_array() && pos.size() == 2) n.Pos = glm::vec2(pos[0].get<float>(), pos[1].get<float>());
        n.ClipPath = j.value("clipPath", std::string());
        n.ClipName = j.value("clipName", std::string());
        n.Loop = j.value("loop", true);
        n.Speed = j.value("speed", 1.0f);
        n.Weight = j.value("weight", 0.5f);
        n.Param = j.value("param", std::string());
        maxId = std::max(maxId, n.Id);
        g.Nodes.push_back(n);
    }
    g.NextId = std::max(root.value("nextId", 1), maxId + 1);
    if (g.OutputNode() == 0) g.Add(AnimNodeKind::Output, glm::vec2(360.0f, 120.0f));

    // Провода проверяются теми же правилами, что и в редакторе: файл, правленный
    // руками, не должен приносить петлю или позу во вход веса.
    for (const json& l : root.value("links", json::array())) {
        const int from = l.value("from", 0), to = l.value("to", 0), pin = l.value("pin", 0);
        std::string why;
        if (!g.CanLink(from, to, pin, &why) || g.LinkInto(to, pin)) {
            LOG_WARN("Anim") << "Граф анимации: провод " << from << " -> " << to << ":" << pin
                             << " отброшен (" << (why.empty() ? "вход уже занят" : why) << ")";
            continue;
        }
        g.Links.push_back({from, to, pin});
    }
    return g;
}

void SaveAnimGraph(const AnimGraph& graph, const std::string& path) {
    // Путь — UTF-8: на Windows кириллица в std::string без перевода ломает имя.
    std::ofstream out(sage::PathFromUtf8(path), std::ios::binary);
    if (!out) throw std::runtime_error("не удалось открыть для записи: " + path);
    out << AnimGraphToJson(graph);
    if (!out) throw std::runtime_error("не удалось записать: " + path);
    ++g_revision;
}

AnimGraph LoadAnimGraph(const std::string& path) {
    // Через Pack: в собранной игре граф лежит в .sagepak вместе с клипами.
    std::string text;
    if (!sage::assets::vfs::ReadText(path, text)) throw std::runtime_error("не удалось прочитать: " + path);
    return AnimGraphFromJson(text);
}

uint64_t AnimGraphRevision() { return g_revision.load(); }

} // namespace sage::anim
