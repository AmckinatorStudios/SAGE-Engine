#include "AnimGraphPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../Project.h"
#include "imgui_internal.h"
#include "sage/core/Paths.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
using namespace sage::anim;
namespace ictl = sage::editor::ictl;

// --- Документ ---------------------------------------------------------------

void AnimGraphPanel::Open(EditorHost& host, const std::string& path) {
    m_focusFrames = 2;
    if (m_hasGraph && path == m_path) return;   // уже открыт — не терять несохранённое
    try {
        AnimGraph g = LoadAnimGraph(path);
        m_graph = std::move(g);
        m_hasGraph = true;
        m_path = path;
        m_dirty = false;
        m_undo.clear();
        m_redo.clear();
        m_selected = 0;
        m_inspectNode = false;
        m_values.clear();
        m_rebind = true;
        m_frameRequest = true;
        m_status.clear();
    } catch (const std::exception& e) {
        m_status = std::string(T("Could not open the graph: ")) + e.what();
        host.SetStatusMessage(m_status);
    }
}

bool AnimGraphPanel::Save(EditorHost& host) {
    if (!m_hasGraph) return false;
    if (m_path.empty()) {
        m_browse = Browse::SaveAs;
        FileBrowser::Config c;
        c.Title = T("Save animation graph");
        c.Mode = FileBrowser::PickMode::SaveFile;
        c.DefaultName = "NewAnimGraph.sageanimgraph";
        c.Filters = assetslot::Extensions(assetslot::Kind::AnimGraph);
        c.FilterLabel = T("Animation graph");
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        return false;
    }
    return SaveTo(host, m_path);
}

bool AnimGraphPanel::SaveTo(EditorHost& host, const std::string& path) {
    try {
        SaveAnimGraph(m_graph, path);
        m_path = path;
        m_dirty = false;
        m_status = T("Saved");
        host.SetStatusMessage(std::string(T("Animation graph saved: ")) + sage::PathToUtf8(fs::path(sage::PathFromUtf8(path)).filename()));
        return true;
    } catch (const std::exception& e) {
        m_status = std::string(T("Could not save: ")) + e.what();
        host.SetStatusMessage(m_status);
        return false;
    }
}

void AnimGraphPanel::PushUndo() {
    m_undo.push_back(m_graph);
    if (m_undo.size() > 100) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void AnimGraphPanel::Undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(m_graph);
    m_graph = m_undo.back();
    m_undo.pop_back();
    Changed();
}

void AnimGraphPanel::Redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(m_graph);
    m_graph = m_redo.back();
    m_redo.pop_back();
    Changed();
}

void AnimGraphPanel::Changed(bool structural) {
    m_dirty = true;
    if (structural) m_rebind = true;
    if (!m_graph.Find(m_selected)) {
        m_selected = 0;
        m_inspectNode = false;
    }
}

void AnimGraphPanel::SelectNode(EditorHost& host, int id) {
    m_selected = m_graph.Find(id) ? id : 0;
    m_inspectNode = m_selected != 0;
    m_inspectSelection = host.Selection().Changes();
}

// --- Предпросмотр на выбранном объекте ----------------------------------------

AnimationComponent* AnimGraphPanel::Target(EditorHost& host) const {
    if (m_targetId < 0) return nullptr;
    GameObject o = host.CurrentScene().Get(m_targetId);
    return o.Valid() ? o.Registry()->try_get<AnimationComponent>(o.Entity()) : nullptr;
}

const AnimGraphInstance* AnimGraphPanel::LiveInstance(EditorHost& host) const {
    const AnimationComponent* am = Target(host);
    return am && am->Graph.Bound() ? &am->Graph : nullptr;
}

std::vector<std::string> AnimGraphPanel::ModelClipNames(EditorHost& host) const {
    std::vector<std::string> names;
    const AnimationComponent* am = Target(host);
    if (!am || !am->Model) return names;
    for (const AnimationClip& c : am->Model->Clips()) names.push_back(c.Name);
    return names;
}

void AnimGraphPanel::ReleasePreview(EditorHost& host) {
    if (AnimationComponent* am = Target(host)) {
        // Свой граф объекта — снова из его файла (или никакого): подменённый
        // из памяти граф в сцене остаться не должен.
        am->GraphResolvedFrom.clear();
        if (am->GraphPath.empty()) am->GraphAsset = nullptr;
    }
    m_targetId = -1;
    m_rebind = true;
}

void AnimGraphPanel::SyncPreview(EditorHost& host) {
    // Объект предпросмотра — выбранный, если у него есть анимация. Выбрали
    // другой — прежнему возвращается его граф.
    int want = -1;
    GameObject sel = host.CurrentScene().Get(host.Selection().Primary());
    if (m_hasGraph && sel.Valid() && sel.Registry()->all_of<AnimationComponent>(sel.Entity())) want = sel.Id();
    if (want != m_targetId) {
        ReleasePreview(host);
        m_targetId = want;
    }
    AnimationComponent* am = Target(host);
    if (!am) return;
    if (m_rebind || !am->GraphAsset) {
        am->GraphAsset = std::make_shared<const AnimGraph>(m_graph);
        m_rebind = false;
    }
    // Значения параметров — те же Params.Floats, что пишут скрипты в игре.
    for (const AnimGraphParam& p : m_graph.Params) {
        auto it = m_values.find(p.Name);
        am->Params.Floats[p.Name] = it != m_values.end() ? it->second : p.Default;
    }
}

// --- Окно -------------------------------------------------------------------

void AnimGraphPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!*open) {
        if (m_targetId >= 0) ReleasePreview(host);
        return;
    }
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    std::string title = T("Animation Graph");
    if (m_hasGraph && !m_path.empty()) title += " — " + sage::PathToUtf8(fs::path(sage::PathFromUtf8(m_path)).stem());
    if (m_dirty) title += " *";
    title += "###" + windowId;
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return;
    }
    if (!ImGui::IsAnyItemActive()) m_editUndo = false;
    SyncPreview(host);

    DrawToolbar(host);
    if (!m_hasGraph) {
        DrawEmpty(host);
    } else {
        // Слева — параметры графа (ими и управляют предпросмотром), справа —
        // холст. Свойства узла — в Inspector.
        const float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.22f, 200.0f, 280.0f);
        ImGui::BeginChild("##ag_params", ImVec2(side, 0), ImGuiChildFlags_Borders);
        {
            ictl::UndoScope undo([this] {
                if (m_editUndo) return;
                PushUndo();
                m_editUndo = true;
            });
            DrawParams(host);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ag_canvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvas(host);
        ImGui::EndChild();

        // Горячие клавиши окна — только когда фокус в нём и не в поле ввода.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(host);
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
        }
    }
    DrawCanvasMenu(host);
    DrawBrowser(host);

    // Файл графа, брошенный в окно, — открыть.
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (ImGui::BeginDragDropTargetCustom(win->Rect(), win->ID)) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(assetslot::kPayload)) {
                std::string dropped((const char*)p->Data, (size_t)p->DataSize);
                if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
                if (assetslot::KindOf(sage::PathFromUtf8(dropped)) == assetslot::Kind::AnimGraph) Open(host, dropped);
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}

void AnimGraphPanel::DrawToolbar(EditorHost& host) {
    if (EditorIcons::Button("plus", T("New"), T("A new graph with only the Output node"))) {
        m_graph = NewAnimGraph();
        m_hasGraph = true;
        m_path.clear();
        m_dirty = true;
        m_undo.clear();
        m_redo.clear();
        m_selected = 0;
        m_inspectNode = false;
        m_values.clear();
        m_rebind = true;
        m_frameRequest = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_hasGraph);
    if (EditorIcons::Button("save", T("Save"), T("Ctrl+S"))) Save(host);
    ImGui::SameLine();
    if (EditorIcons::Button("save", T("Save as..."))) {
        const std::string keep = m_path;
        m_path.clear();
        Save(host);
        m_path = keep;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("undo", T("Undo (Ctrl+Z)"))) Undo();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("redo", T("Redo (Ctrl+Y)"))) Redo();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("fit", T("Show all nodes"))) m_frameRequest = true;
    ImGui::EndDisabled();

    // Где играет граф: выбранный объект во вьюпорте. Пауза и скорость — поля
    // его компонента Animation, а не настройки окна: одни и те же здесь, в
    // инспекторе и в игре.
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 3.0f);
    AnimationComponent* am = m_hasGraph ? Target(host) : nullptr;
    if (!m_hasGraph) {
        // Без графа предпросматривать нечего.
    } else if (!am) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T("Select an object with a skeleton: the graph plays on it in the Viewport"));
    } else {
        if (EditorIcons::IconOnlyButton(am->Playing ? "pause" : "play", am->Playing ? T("Pause") : T("Play"))) {
            host.PushUndoSnapshot();
            am->Playing = !am->Playing;
        }
        ImGui::SameLine();
        if (EditorIcons::IconOnlyButton("refresh", T("Restart from the beginning"))) am->Graph.Restart();
        ImGui::SameLine();
        // Скорость — короткий список, а не полоса во всю ширину.
        static const float kSpeeds[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f};
        char cur[16];
        std::snprintf(cur, sizeof(cur), "%gx", am->Speed);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.25x").x + ImGui::GetFrameHeight() * 1.6f);
        if (ictl::BeginDropdown("##ag_speed", cur, ictl::DropLook::Compact)) {
            for (float sp : kSpeeds) {
                char item[16];
                std::snprintf(item, sizeof(item), "%gx", sp);
                if (ImGui::Selectable(item, std::abs(am->Speed - sp) < 1e-3f)) {
                    host.PushUndoSnapshot();
                    am->Speed = sp;
                }
            }
            ictl::EndDropdown();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Playback speed of the object"));
        ImGui::SameLine();
        GameObject o = host.CurrentScene().Get(m_targetId);
        ImGui::AlignTextToFramePadding();
        EditorIcons::Inline("model");
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextUnformatted(o.Valid() ? o.Name().c_str() : "");
        if (am->Ready && !am->Model) {
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("This model has no skeleton — nothing to animate"));
        }
        // Что не так с привязкой — прямо здесь, а не только в логе.
        if (am->Graph.Bound()) {
            for (const std::string& p : am->Graph.Problems()) {
                ImGui::SameLine();
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", p.c_str());
            }
            if (am->Graph.MissingBones() > 0) {
                ImGui::SameLine();
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
                                   T("%d clip channels found no bone in this skeleton"), am->Graph.MissingBones());
            }
        }
    }
    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", m_status.c_str());
    }
}

void AnimGraphPanel::DrawEmpty(EditorHost& host) {
    (void)host;
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("No graph is open."));
    ImGui::TextDisabled("%s", T("Create one with New, in Assets (Create > Animation graph)"));
    ImGui::TextDisabled("%s", T("or open a .sageanimgraph file by double-clicking it."));
}

// --- Диалоги ----------------------------------------------------------------

void AnimGraphPanel::DrawBrowser(EditorHost& host) {
    if (!m_browser.Draw()) return;
    const fs::path picked = m_browser.Result();
    switch (m_browse) {
        case Browse::SaveAs: {
            fs::path file = picked;
            if (file.extension() != ".sageanimgraph") file += ".sageanimgraph";
            SaveTo(host, sage::PathToUtf8(file));
            break;
        }
        case Browse::ClipFile:
            if (AnimGraphNode* n = m_graph.Find(m_browseNode)) {
                PushUndo();
                n->ClipPath = host.CurrentProject().AssetRef(picked);
                n->ClipName.clear();
                Changed();
            }
            break;
        case Browse::None: break;
    }
    m_browse = Browse::None;
}
