#include "AnimGraphPanel.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../Project.h"
#include "imgui_internal.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Paths.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;
using namespace sage::anim;

AnimGraphPanel::AnimGraphPanel() = default;
AnimGraphPanel::~AnimGraphPanel() = default;

void AnimGraphPanel::Shutdown() {
    // При живом контексте GL: сцена держит модель (буферы), превью — кадры.
    m_scene.reset();
    m_entity = entt::null;
    m_sceneModel.clear();
    m_rebind = true;
    m_preview.Shutdown();
}

const AnimGraphInstance* AnimGraphPanel::PreviewInstance() const {
    if (!m_scene || m_entity == entt::null) return nullptr;
    const auto* am = m_scene->Registry().try_get<AnimationComponent>(m_entity);
    return am && am->Graph.Bound() ? &am->Graph : nullptr;
}

// --- Документ ---------------------------------------------------------------

void AnimGraphPanel::Open(EditorHost& host, const std::string& path, const std::string& modelHint) {
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
        m_values.clear();
        m_rebind = true;
        m_frameRequest = true;
        m_status.clear();
        if (m_graph.PreviewModel.empty() && !modelHint.empty()) m_graph.PreviewModel = modelHint;
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
    if (!m_graph.Find(m_selected)) m_selected = 0;
}

// --- Окно -------------------------------------------------------------------

void AnimGraphPanel::Draw(EditorHost& host, bool* open) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(1150, 760), ImGuiCond_FirstUseEver);
    std::string title = T("Animation Graph");
    if (m_hasGraph)
        title += " — " + (m_path.empty() ? std::string(T("untitled"))
                                         : sage::PathToUtf8(fs::path(sage::PathFromUtf8(m_path)).stem()));
    if (m_dirty) title += " *";
    title += "###AnimGraphWindow";
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return;
    }
    if (!ImGui::IsAnyItemActive()) m_editUndo = false;

    DrawToolbar(host);
    if (!m_hasGraph) {
        DrawEmpty(host);
    } else {
        // Справа — инспектор узла и параметры; слева сверху — предпросмотр,
        // снизу — холст. Предпросмотр большой: смешивание оценивают глазами.
        const float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.26f, 240.0f, 340.0f);
        const float mainW = std::max(240.0f, ImGui::GetContentRegionAvail().x - side - ImGui::GetStyle().ItemSpacing.x);
        const float totalH = ImGui::GetContentRegionAvail().y;
        const float previewH = std::max(160.0f, totalH * 0.48f);

        ImGui::BeginGroup();
        ImGui::BeginChild("##ag_preview", ImVec2(mainW, previewH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPreview(host, ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        ImGui::BeginChild("##ag_canvas", ImVec2(mainW, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvas(host);
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginChild("##ag_side", ImVec2(0, 0), ImGuiChildFlags_Borders);
        DrawSide(host);
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

// --- Предпросмотр -----------------------------------------------------------

std::vector<std::string> AnimGraphPanel::ModelClipNames() const {
    std::vector<std::string> names;
    if (!m_scene || m_entity == entt::null) return names;
    const auto* am = m_scene->Registry().try_get<AnimationComponent>(m_entity);
    if (!am || !am->Model) return names;
    for (const AnimationClip& c : am->Model->Clips()) names.push_back(c.Name);
    return names;
}

void AnimGraphPanel::SyncPreview(EditorHost& host, float dt) {
    (void)host;
    if (!m_scene) {
        m_scene = std::make_unique<Scene>("animgraph-preview");
        m_entity = m_scene->CreateEmptyObject("Preview").Entity();
        m_scene->Registry().emplace<MeshRendererComponent>(m_entity);
        m_scene->Registry().emplace<AnimationComponent>(m_entity);
        m_sceneModel.clear();
    }
    auto& reg = m_scene->Registry();
    auto& mr = reg.get<MeshRendererComponent>(m_entity);
    auto& am = reg.get<AnimationComponent>(m_entity);
    if (m_sceneModel != m_graph.PreviewModel) {
        // Модель сменилась: Mesh показывает на новую, система анимации сама
        // перезагрузит скелет и перепривяжет граф.
        m_sceneModel = m_graph.PreviewModel;
        mr.Ref = MeshRef();
        if (!m_sceneModel.empty()) {
            mr.Ref.type = MeshRef::Type::Model;
            mr.Ref.path = m_sceneModel;
        }
        m_rebind = true;
    }
    if (m_rebind) {
        am.GraphAsset = std::make_shared<const AnimGraph>(m_graph);
        m_rebind = false;
    }
    am.Playing = m_playing;
    am.Speed = m_playSpeed;
    // Значения параметров — те же Params.Floats, что пишут скрипты в игре.
    for (const AnimGraphParam& p : m_graph.Params) {
        auto it = m_values.find(p.Name);
        am.Params.Floats[p.Name] = it != m_values.end() ? it->second : p.Default;
    }
    UpdateAnimators(*m_scene, dt);
}

void AnimGraphPanel::DrawPreview(EditorHost& host, const ImVec2& size) {
    SyncPreview(host, ImGui::GetIO().DeltaTime);
    const auto& am = m_scene->Registry().get<AnimationComponent>(m_entity);

    // Строка над картинкой: модель предпросмотра и проигрывание.
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline("model");
    ImGui::SameLine();
    const float slotW = std::min(320.0f, size.x * 0.45f);
    ImGui::SetNextItemWidth(slotW);
    assetslot::SetCompact(true);
    const assetslot::Result r = assetslot::Draw(host, "ag_model", assetslot::Kind::Model, m_graph.PreviewModel,
                                                nullptr, T("Preview model with a skeleton"));
    assetslot::SetCompact(false);
    if (r.Changed) {
        PushUndo();
        m_graph.PreviewModel = r.Path;
        Changed(false);
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = T("Choose a model for the preview");
        c.Filters = assetslot::Extensions(assetslot::Kind::Model);
        c.FilterLabel = T("Model");
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_browse = Browse::PreviewModel;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(m_playing ? "pause" : "play", m_playing ? T("Pause") : T("Play"))) m_playing = !m_playing;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("refresh", T("Restart from the beginning"))) {
        auto& comp = m_scene->Registry().get<AnimationComponent>(m_entity);
        comp.Graph.Restart();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, std::min(160.0f, ImGui::GetContentRegionAvail().x)));
    {
        // Скорость предпросмотра — свойство окна, не графа: в историю не пишется.
        sage::editor::ictl::UndoScope noUndo([] {});
        sage::editor::ictl::SliderNumber(host, T("Speed"), &m_playSpeed, 0.0f, 2.0f, "%.2fx");
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (m_graph.PreviewModel.empty()) {
        ImGui::TextDisabled("%s", T("Choose a model with a skeleton to see the graph play."));
        return;
    }
    if (am.Ready && !am.Model) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                           T("This model has no skeleton — nothing to animate"));
        return;
    }
    if (!am.Model) return;

    // Рамка по костям в позе покоя: модель целиком в кадре на любом масштабе.
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const glm::mat4& m : am.Anim.GlobalMatrices()) {
        lo = glm::min(lo, glm::vec3(m[3]));
        hi = glm::max(hi, glm::vec3(m[3]));
    }
    if (lo.x > hi.x) lo = hi = glm::vec3(0.0f);
    const glm::vec3 center = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.6f, 0.25f);

    const float side = std::max(64.0f, std::min(avail.x, avail.y));
    const int px = (int)std::min(1024.0f, side * ImGui::GetIO().DisplayFramebufferScale.y);
    const uint64_t tex = m_preview.RenderLiveScene(*m_scene, px, center, radius, "animgraph");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - side) * 0.5f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
    // Орбита — левой кнопкой, приближение — колесом, как у обложек.
    ImGui::SetCursorScreenPos(at);
    ImGui::InvisibleButton("##ag_orbit", ImVec2(side, side));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        m_preview.Orbit(ImGui::GetIO().MouseDelta.x * 0.5f, ImGui::GetIO().MouseDelta.y * 0.5f);
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) m_preview.Zoom(ImGui::GetIO().MouseWheel);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_preview.ResetView();

    // Что не так с привязкой — прямо на картинке, а не только в логе.
    if (am.Graph.Bound() && (!am.Graph.Problems().empty() || am.Graph.MissingBones() > 0)) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + 8.0f, at.y + 8.0f));
        ImGui::PushTextWrapPos(at.x + side - 8.0f);
        for (const std::string& p : am.Graph.Problems())
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", p.c_str());
        if (am.Graph.MissingBones() > 0)
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), T("%d clip channels found no bone in this skeleton"),
                               am.Graph.MissingBones());
        ImGui::PopTextWrapPos();
    }
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
        case Browse::PreviewModel:
            PushUndo();
            m_graph.PreviewModel = host.CurrentProject().AssetRef(picked);
            Changed(false);
            break;
        case Browse::None: break;
    }
    m_browse = Browse::None;
}
