#include "AiGraphPanel.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../EditorHost.h"
#include "../PanelRegistry.h"
#include "../Project.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ObjectSlot.h"
#include "../ui/UI.h"
#include "AiGraphDetail.h"
#include "AiGraphExtensions.h"
#include "sage/ai/GraphDebug.h"
#include "sage/ai/GraphRuntime.h"
#include "sage/ai/World.h"
#include "sage/core/Paths.h"
#include "sage/scripting/ScriptEngine.h"

// Окно графа: файл, библиотека, отмена, боковые панели. Холст — в
// AiGraphCanvas.cpp.

namespace fs = std::filesystem;
using sage::ai::GraphDoc;
using sage::ai::GraphKind;
using sage::ai::GraphNode;
using sage::ai::GraphVar;
using sage::vars::Value;
using namespace sage::editor::aigraph;

namespace sage::editor::aigraph {

bool InputStr(const char* id, std::string& text, const char* hint) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", text.c_str());
    const bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, sizeof(buf))
                              : ImGui::InputText(id, buf, sizeof(buf));
    if (changed) text = buf;
    return changed;
}

const char* Tr(const std::string& key) { return key.empty() ? "" : T(key.c_str()); }

std::string ClipUtf8(const std::string& s, size_t max) {
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (chars == max) return s.substr(0, i) + "…";
            ++chars;
        }
    }
    return s;
}

std::string TrPath(const std::string& category) {
    std::string out, part;
    for (size_t i = 0; i <= category.size(); ++i) {
        if (i == category.size() || category[i] == '/') {
            if (!out.empty()) out += "/";
            out += T(part.c_str());
            part.clear();
        } else {
            part += category[i];
        }
    }
    return out;
}

ImU32 TypeColor(const sage::ai::TypeRegistry& types, const std::string& type) {
    if (const sage::ai::TypeInfo* t = types.Find(type)) return t->Color;
    return types.IsStruct(type) ? 0xFFE0E060 : 0xFFB0B0B0;
}

bool ValueField(EditorHost& host, const char* id, const std::string& type, Value& v,
                const sage::ai::TypeRegistry& types) {
    namespace t = sage::ai::type;
    ImGui::PushID(id);
    bool changed = false;
    if (const PinEditorExt* ext = FindPinEditor(type)) {
        changed = ext->Edit(v);
    } else if (type == t::Bool) {
        bool b = v.AsBool();
        if (ImGui::Checkbox("##v", &b)) { v = Value(b); changed = true; }
    } else if (type == t::Int || type == t::Task) {
        int i = v.AsInt();
        if (ImGui::DragInt("##v", &i)) { v = Value(i); changed = true; }
    } else if (type == t::Float) {
        float f = v.AsFloat();
        if (ImGui::DragFloat("##v", &f, 0.01f)) { v = Value(f); changed = true; }
    } else if (type == t::String) {
        std::string s = v.AsString();
        if (InputStr("##v", s)) { v = Value(s); changed = true; }
    } else if (type == t::Vec2) {
        glm::vec2 x = v.AsVec2();
        if (ImGui::DragFloat2("##v", &x.x, 0.01f)) { v = Value(x); changed = true; }
    } else if (type == t::Vec3) {
        glm::vec3 x = v.AsVec3();
        if (ImGui::DragFloat3("##v", &x.x, 0.01f)) { v = Value(x); changed = true; }
    } else if (type == t::Color) {
        glm::vec4 c = v.AsVec4();
        if (ImGui::ColorEdit4("##v", &c.x, ImGuiColorEditFlags_NoInputs)) { v = Value(c); changed = true; }
    } else if (type == t::Rotation) {
        // Углами: кватернион не прочесть и не набрать.
        glm::vec3 e = v.AsVec3();
        if (ImGui::DragFloat3("##v", &e.x, 0.5f)) { v = Value(glm::quat(glm::radians(e))); changed = true; }
    } else if (type == t::Object) {
        objectslot::Options opt;
        opt.EmptyLabel = T("Nothing — pick or drag an object");
        const objectslot::Result r = objectslot::Draw(host, "obj", v.AsEntity().Id, opt);
        if (r.Changed) { v = Value(sage::vars::EntityRef{std::max(0, r.Id)}); changed = true; }
    } else if (types.IsStruct(type) && v.Type() == sage::vars::Kind::Struct) {
        // Структура — полями: тот же редактор для каждого поля по его типу.
        std::vector<sage::vars::Field> fields = v.Fields();
        for (sage::vars::Field& f : fields) {
            ImGui::TextUnformatted(f.first.c_str());
            ImGui::SameLine();
            if (ValueField(host, f.first.c_str(), types.Of(f.second), f.second, types)) {
                v.SetField(f.first, f.second);
                changed = true;
            }
        }
    } else {
        ImGui::TextDisabled("%s", v.AsString().c_str());
    }
    ImGui::PopID();
    return changed;
}

} // namespace sage::editor::aigraph

GraphEditorMode GraphEditorMode::Ai() {
    GraphEditorMode m;
    m.WindowId = "AiGraph";
    m.Title = "AI Graph";
    m.Folder = "ai";
    m.Extension = ".aigraph";
    // AI Graph только решает: какую задачу и когда. Сами задачи — в Visual
    // Scripting (граф Task) или на Lua.
    m.Kinds = {GraphKind::Behavior};
    m.DefaultKind = GraphKind::Behavior;
    m.AiTools = true;
    return m;
}

GraphEditorMode GraphEditorMode::Visual() {
    GraphEditorMode m;
    m.WindowId = "VisualScript";
    m.Title = "Visual Script";
    m.Folder = "visual";
    m.Extension = ".vgraph";
    m.Kinds = {GraphKind::Script, GraphKind::Task, GraphKind::Function};
    m.DefaultKind = GraphKind::Script;
    return m;
}

AiGraphPanel::AiGraphPanel(GraphEditorMode mode) : m_mode(std::move(mode)) {}
AiGraphPanel::~AiGraphPanel() = default;

sage::ai::World* AiGraphPanel::Library() const { return m_lib ? &m_lib->AI() : nullptr; }

std::string AiGraphPanel::AiDir(EditorHost& host) const {
    if (!host.CurrentProject().Loaded()) return {};
    return sage::PathToUtf8(host.CurrentProject().AssetsDir() / sage::PathFromUtf8(m_mode.Folder));
}

void AiGraphPanel::ReloadLibrary(EditorHost& host) {
    // Свой движок скриптов без сцены: библиотека проекта грузится в него так
    // же, как в Play, и палитра видит ровно то, что увидит игра.
    m_lib = std::make_unique<ScriptEngine>();
    m_libError.clear();
    if (!host.CurrentProject().Loaded()) return;
    m_libProject = sage::PathToUtf8(host.CurrentProject().Dir());
    m_lib->SetAiRoot(m_libProject);
    // Обе папки графов: графы-функции — общая библиотека двух редакторов, и
    // палитра каждого видит функции, сделанные в другом.
    m_lib->LoadAiLibrary("assets/ai");
    m_lib->LoadAiLibrary("assets/visual");
    m_issuesDirty = true;
    // Открытый граф — тоже в библиотеку: его подпись нужна, чтобы его же
    // Return показал выходы, а соседние графы — его входы.
    if (m_hasGraph && !m_doc.Name.empty()) {
        std::string err;
        m_lib->AI().RegisterGraph(m_doc, err);
    }
}

void AiGraphPanel::MarkChanged() {
    m_dirty = true;
    m_issuesDirty = true;
    if (m_lib && !m_doc.Name.empty()) {
        std::string err;
        m_lib->AI().RegisterGraph(m_doc, err);
    }
}

void AiGraphPanel::PushUndo() {
    m_undo.push_back(sage::ai::SaveGraph(m_doc));
    if (m_undo.size() > 100) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void AiGraphPanel::RenameVar(const std::string& scope, const std::string& from, const std::string& to) {
    for (GraphNode& n : m_doc.Nodes) {
        if (n.Type != "var.get" && n.Type != "var.set") continue;
        auto sc = n.Values.find("scope");
        const std::string s = sc == n.Values.end() ? "local" : sc->second.AsString();
        auto nm = n.Values.find("name");
        if (s == scope && nm != n.Values.end() && nm->second.AsString() == from) nm->second = Value(to);
    }
    // Входы и выходы — ещё и пины Return и вызовов этого графа.
    if (scope == "output")
        for (GraphNode& n : m_doc.Nodes)
            if (n.Type == "flow.return") {
                for (sage::ai::GraphLink& l : m_doc.Links)
                    if (l.To.Node == n.Id && l.To.Pin == from) l.To.Pin = to;
                auto v = n.Values.find(from);
                if (v != n.Values.end()) {
                    n.Values[to] = v->second;
                    n.Values.erase(from);
                }
            }
}

void AiGraphPanel::EditBegin() {
    if (m_editUndo) return;
    PushUndo();
    m_editUndo = true;
}

void AiGraphPanel::Undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(sage::ai::SaveGraph(m_doc));
    std::string err;
    sage::ai::LoadGraph(m_undo.back(), m_doc, err);
    m_undo.pop_back();
    m_selected.clear();
    MarkChanged();
}

void AiGraphPanel::Redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(sage::ai::SaveGraph(m_doc));
    std::string err;
    sage::ai::LoadGraph(m_redo.back(), m_doc, err);
    m_redo.pop_back();
    m_selected.clear();
    MarkChanged();
}

void AiGraphPanel::NewGraph(EditorHost& host, GraphKind kind) {
    if (!m_lib) ReloadLibrary(host);
    m_doc = GraphDoc();
    m_doc.Kind = kind;
    m_doc.Name = kind == GraphKind::Behavior   ? "NewBehavior"
                 : kind == GraphKind::Function ? "NewFunction"
                 : kind == GraphKind::Script   ? "NewScript"
                                               : "NewTask";
    if (kind == GraphKind::Script) {
        m_doc.AddNode("script.on_start", 0.0f, 0.0f);
        m_doc.AddNode("script.on_update", 0.0f, 140.0f);
    } else {
        m_doc.AddNode("flow.start", 0.0f, 0.0f);
    }
    m_issuesDirty = true;
    m_path.clear();
    m_hasGraph = true;
    m_dirty = true;
    m_undo.clear();
    m_redo.clear();
    m_selected.clear();
    m_pan = ImVec2(80.0f, 120.0f);
    m_zoom = 1.0f;
    m_status = T("New graph — save it to give it a file");
}

void AiGraphPanel::Open(EditorHost& host, const std::string& path) {
    // Несохранённое не теряется молча: открыть другой граф — значит сначала
    // сохранить этот (у безымянного — спросить имя и не открывать).
    if (m_hasGraph && m_dirty && path != m_path) {
        if (m_path.empty()) {
            Save(host);
            m_status = T("Save the new graph first — it has no file yet");
            return;
        }
        Save(host);
    }
    if (!m_lib || m_libProject != sage::PathToUtf8(host.CurrentProject().Dir())) ReloadLibrary(host);
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    GraphDoc doc;
    std::string err;
    if (!f || !sage::ai::LoadGraph(ss.str(), doc, err)) {
        m_status = std::string(T("Could not open the graph: ")) + (err.empty() ? path : err);
        return;
    }
    if (doc.Name.empty()) doc.Name = sage::PathToUtf8(sage::PathFromUtf8(path).stem());
    // Граф чужого вида — в свой редактор: задачу правят в Visual Script, а
    // решения — в AI Graph, где бы ни лежал файл (старый проект с задачами в
    // assets/ai открывается без правки руками).
    if (std::find(m_mode.Kinds.begin(), m_mode.Kinds.end(), doc.Kind) == m_mode.Kinds.end()) {
        if (doc.Kind == GraphKind::Behavior) host.OpenAiGraph(path);
        else host.OpenVisualScript(path);
        return;
    }
    m_doc = std::move(doc);
    m_path = path;
    m_hasGraph = true;
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    m_selected.clear();
    m_frameRequest = true;
    m_issuesDirty = true;
    m_status.clear();
    std::string e2;
    m_lib->AI().RegisterGraph(m_doc, e2);
    RequestFocus();
}

bool AiGraphPanel::Save(EditorHost& host) {
    if (m_path.empty()) {
        m_saveAsRequest = true;
        std::snprintf(m_saveName, sizeof(m_saveName), "%s", m_doc.Name.c_str());
        return false;
    }
    std::error_code ec;
    fs::create_directories(sage::PathFromUtf8(m_path).parent_path(), ec);
    std::ofstream f(sage::PathFromUtf8(m_path), std::ios::binary | std::ios::trunc);
    f << sage::ai::SaveGraph(m_doc);
    if (!f) {
        m_status = std::string(T("Could not save: ")) + m_path;
        return false;
    }
    m_dirty = false;
    m_status = std::string(T("Saved: ")) + sage::PathToUtf8(sage::PathFromUtf8(m_path).filename());
    // Сохранённый граф — часть библиотеки: соседние графы видят его подпись.
    ReloadLibrary(host);
    return true;
}

bool AiGraphPanel::SaveAs(EditorHost& host, const std::string& name) {
    const std::string dir = AiDir(host);
    if (dir.empty() || name.empty()) return false;
    std::string clean;
    for (char c : name)
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-' || (unsigned char)c >= 0x80) clean += c;
    if (clean.empty()) return false;
    const std::string path = sage::PathToUtf8(sage::PathFromUtf8(dir) / (clean + m_mode.Extension));
    // Чужой файл с тем же именем не перезаписывается: два графа с одним
    // именем — ещё и две задачи с одним именем, и одна молча заменит другую.
    std::error_code ec;
    if (path != m_path && fs::exists(sage::PathFromUtf8(path), ec)) {
        m_status = std::string(T("A graph with this name already exists: ")) + clean;
        return false;
    }
    if (Library() && Library()->FindGraph(clean) && clean != m_doc.Name) {
        m_status = std::string(T("A graph with this name already exists: ")) + clean;
        return false;
    }
    m_doc.Name = clean;
    m_path = path;
    return Save(host);
}

void AiGraphPanel::FocusNode(int nodeId) {
    const GraphNode* n = m_doc.FindNode(nodeId);
    if (!n) return;
    m_selected = {nodeId};
    m_pan = ImVec2(m_canvasSize.x * 0.5f / m_zoom - n->X - 90.0f, m_canvasSize.y * 0.5f / m_zoom - n->Y - 40.0f);
    RequestFocus();
}

std::string AiGraphPanel::FindGraphFile(EditorHost& host, const std::string& name, GraphKind* kind) const {
    if (!host.CurrentProject().Loaded()) return {};
    for (const char* folder : {"ai", "visual"}) {
        const fs::path dir = host.CurrentProject().AssetsDir() / folder;
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) break;
            const fs::path ext = it->path().extension();
            if (ext != ".aigraph" && ext != ".vgraph") continue;
            std::ifstream f(it->path(), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            GraphDoc d;
            std::string err;
            if (!sage::ai::LoadGraph(ss.str(), d, err)) continue;
            const std::string n = d.Name.empty() ? sage::PathToUtf8(it->path().stem()) : d.Name;
            if (n != name) continue;
            if (kind) *kind = d.Kind;
            return sage::PathToUtf8(it->path());
        }
    }
    return {};
}

void AiGraphPanel::OpenTaskGraph(EditorHost& host, const std::string& task) {
    GraphKind kind = GraphKind::Task;
    const std::string path = FindGraphFile(host, task, &kind);
    if (path.empty()) {
        m_status = std::string(T("Task is not a graph of this project: ")) + task;
        return;
    }
    // Граф чужого вида — в свой редактор: граф объекта Visual Scripting не
    // правят палитрой ИИ, и наоборот.
    const bool mine = std::find(m_mode.Kinds.begin(), m_mode.Kinds.end(), kind) != m_mode.Kinds.end();
    if (!mine) {
        if (kind == GraphKind::Behavior) host.OpenAiGraph(path);
        else host.OpenVisualScript(path);
        return;
    }
    Open(host, path);
}

// ---------------------------------------------------------------------------
// Окно.
// ---------------------------------------------------------------------------
void AiGraphPanel::Draw(EditorHost& host, bool* open) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(1100, 650), ImGuiCond_FirstUseEver);
    const std::string title = std::string(T(m_mode.Title.c_str())) +
                              (m_hasGraph ? " — " + m_doc.Name : std::string()) + (m_dirty ? " *" : "") + "###" +
                              m_mode.WindowId;
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return;
    }
    if (!ImGui::IsAnyItemActive()) m_editUndo = false;
    if (!m_lib || (host.CurrentProject().Loaded() && m_libProject != sage::PathToUtf8(host.CurrentProject().Dir())))
        ReloadLibrary(host);
    DrawToolbar(host);
    if (!m_hasGraph) {
        DrawEmpty(host);
    } else {
        const float side = 260.0f;
        ImGui::BeginChild("##aig_graph", ImVec2(side, 0), ImGuiChildFlags_Borders);
        DrawGraphSide(host);
        ImGui::EndChild();
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        // Центр: холст и под ним — проблемы графа и вывод выполнения.
        const float centerW = std::max(200.0f, avail - side - 8.0f);
        ImGui::BeginGroup();
        const float bottomH = m_bottomOpen ? 150.0f : ImGui::GetFrameHeightWithSpacing() + 8.0f;
        ImGui::BeginChild("##aig_canvas", ImVec2(centerW, std::max(120.0f, ImGui::GetContentRegionAvail().y - bottomH)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvas(host);
        ImGui::EndChild();
        ImGui::BeginChild("##aig_bottom", ImVec2(centerW, 0), ImGuiChildFlags_Borders);
        DrawBottom(host);
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginChild("##aig_node", ImVec2(0, 0), ImGuiChildFlags_Borders);
        DrawNodeSide(host);
        ImGui::EndChild();
    }
    DrawPalette(host);
    DrawNodeMenu(host);
    DrawOpenPopup(host);
    DrawSaveAsPopup(host);
    ImGui::End();
}

void AiGraphPanel::DrawEmpty(EditorHost& host) {
    ImGui::Spacing();
    if (m_mode.AiTools)
        ImGui::TextWrapped("%s", T("AI Graph decides which task a character runs and when to switch: conditions, "
                                   "priorities (Decide), starting and cancelling tasks, events. The tasks themselves "
                                   "are made in Visual Script or Lua. AI graphs live in assets/ai of the project."));
    else
        ImGui::TextWrapped("%s", T("Visual Script is general visual programming: tasks for AI (how a task is done), "
                                   "game logic of objects (On Start, On Update) and functions shared with AI Graph. "
                                   "Graphs live in assets/visual of the project."));
    ImGui::Spacing();
    if (!host.CurrentProject().Loaded()) {
        ImGui::TextDisabled("%s", T("Open a project to work with graphs."));
        return;
    }
    for (GraphKind k : m_mode.Kinds) {
        const char* label = k == GraphKind::Behavior   ? T("New behaviour graph")
                            : k == GraphKind::Task     ? T("New task graph")
                            : k == GraphKind::Script   ? T("New visual script")
                                                       : T("New function graph");
        if (EditorIcons::Button("plus", label)) NewGraph(host, k);
        ImGui::SameLine();
    }
    if (EditorIcons::Button("open", T("Open…"))) m_openRequest = true;
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
}

void AiGraphPanel::DrawToolbar(EditorHost& host) {
    const bool project = host.CurrentProject().Loaded();
    ImGui::BeginDisabled(!project);
    if (EditorIcons::IconOnlyButton("plus", m_mode.AiTools ? T("New behaviour graph") : T("New visual script")))
        NewGraph(host, m_mode.DefaultKind);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("open", T("Open a graph of the project"))) m_openRequest = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_hasGraph);
    if (EditorIcons::IconOnlyButton("save", T("Save (Ctrl+S)"))) Save(host);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("undo", T("Undo (Ctrl+Z)"))) Undo();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("redo", T("Redo (Ctrl+Y)"))) Redo();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("fit", T("Show the whole graph (F)"))) m_frameRequest = true;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("rect", T("Add a group around the selection"))) {
        PushUndo();
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int id : m_selected)
            if (const GraphNode* n = m_doc.FindNode(id)) {
                x0 = std::min(x0, n->X);
                y0 = std::min(y0, n->Y);
                x1 = std::max(x1, n->X + 200.0f);
                y1 = std::max(y1, n->Y + 120.0f);
            }
        const ImVec2 c = ToGraph(ImVec2(m_origin.x + m_canvasSize.x * 0.4f, m_origin.y + m_canvasSize.y * 0.4f));
        if (x0 > x1) { x0 = c.x; y0 = c.y; x1 = c.x + 300.0f; y1 = c.y + 200.0f; }
        m_doc.AddGroup(T("Group"), x0 - 30.0f, y0 - 50.0f, x1 - x0 + 60.0f, y1 - y0 + 80.0f);
        MarkChanged();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("refresh", T("Reload the AI library of the project (Lua nodes, tasks, graphs)")))
        ReloadLibrary(host);
    // Инструменты ИИ — только у SAGE AI Graph: задачи и их состояния к
    // Visual Scripting отношения не имеют.
    if (m_mode.AiTools) {
        ImGui::SameLine();
        if (EditorIcons::IconOnlyButton("debug", T("AI Debugger: running tasks and their states")))
            host.Panels().SetVisible("AiDebugger", true);
    }
    ImGui::EndDisabled();

    // Инструменты плагинов (editor.graph_tool).
    const std::string editorKey = m_mode.AiTools ? "ai" : "visual";
    for (const ToolExt& tool : Tools()) {
        if (!tool.Editor.empty() && tool.Editor != editorKey) continue;
        ImGui::SameLine();
        if (ImGui::Button(tool.Title.c_str()) && m_hasGraph) {
            ToolContext ctx{m_path, m_doc.Name, editorKey, std::vector<int>(m_selected.begin(), m_selected.end())};
            tool.Run(ctx);
        }
    }

    // Отладка: экземпляр этого графа в идущей игре.
    sage::ai::GraphDebugger& dbg = sage::ai::GraphDebugger::Get();
    if (m_hasGraph && !dbg.Runs().empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        std::string current = T("Not shown");
        for (const auto& [id, r] : dbg.Runs())
            if (id == m_debugRun) current = "#" + std::to_string(id) + " " + r.Status;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##aig_run", current.c_str())) {
            if (ImGui::Selectable(T("Not shown"), m_debugRun == 0)) m_debugRun = 0;
            for (const auto& [id, r] : dbg.Runs()) {
                if (r.Graph != m_doc.Name) continue;
                const std::string label = "#" + std::to_string(id) + " " + r.Task + " — " + r.Status;
                if (ImGui::Selectable(label.c_str(), m_debugRun == id)) m_debugRun = id;
            }
            ImGui::EndCombo();
        }
        if (m_debugRun && dbg.IsPaused(m_debugRun)) {
            ImGui::SameLine();
            if (EditorIcons::IconOnlyButton("play", T("Continue (F5)"))) dbg.Continue(m_debugRun);
            ImGui::SameLine();
            if (EditorIcons::IconOnlyButton("step", T("Step one node (F10)"))) dbg.Step(m_debugRun);
        }
    }
    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_status.c_str());
    }
}

void AiGraphPanel::DrawVarList(EditorHost& host, const char* title, std::vector<GraphVar>& vars, const char* id) {
    ImGui::PushID(id);
    sage::ai::World* lib = Library();
    if (EditorTheme::SectionHeader(title)) {
        int remove = -1;
        for (size_t i = 0; i < vars.size(); ++i) {
            GraphVar& v = vars[i];
            ImGui::PushID((int)i);
            ImGui::SetNextItemWidth(110.0f);
            std::string name = v.Name;
            // Занятое имя не применяется: две переменные с одним именем —
            // одна молча прячет другую.
            if (InputStr("##name", name) && !name.empty() &&
                std::none_of(vars.begin(), vars.end(), [&](const GraphVar& o) { return &o != &v && o.Name == name; })) {
                EditBegin();
                // Узлы Get/Set Variable идут за переименованием: иначе
                // переименованная переменная молча ломала бы весь граф.
                const std::string scope = std::string(id) == "in" ? "input" : std::string(id) == "out" ? "output" : "local";
                RenameVar(scope, v.Name, name);
                v.Name = name;
                MarkChanged();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-30.0f);
            if (ImGui::BeginCombo("##type", v.Type.c_str())) {
                if (lib)
                    for (const sage::ai::TypeInfo* t : lib->Types().All()) {
                        if (t->Id == sage::ai::type::Exec) continue;
                        if (ImGui::Selectable(t->Id.c_str(), v.Type == t->Id)) {
                            PushUndo();
                            v.Type = t->Id;
                            v.Default = lib->Types().Default(t->Id);
                            MarkChanged();
                        }
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (EditorIcons::IconOnlyButton("trash", T("Remove"))) remove = (int)i;
            if (lib) {
                ImGui::SetNextItemWidth(-1.0f);
                Value def = lib->Types().Convert(v.Default, v.Type);
                // Поле умолчания: у объекта сцены умолчания нет — ссылку дают входом.
                if (v.Type != sage::ai::type::Object && v.Type != sage::ai::type::Task &&
                    ValueField(host, "def", v.Type, def, lib->Types())) {
                    EditBegin();
                    v.Default = def;
                    MarkChanged();
                }
            }
            ImGui::PopID();
        }
        if (remove >= 0) {
            PushUndo();
            vars.erase(vars.begin() + remove);
            MarkChanged();
        }
        if (EditorIcons::Button("plus", T("Add"))) {
            PushUndo();
            GraphVar v;
            v.Name = "value" + std::to_string(vars.size() + 1);
            v.Type = sage::ai::type::Float;
            v.Default = Value(0.0f);
            vars.push_back(v);
            MarkChanged();
        }
    }
    ImGui::PopID();
}
