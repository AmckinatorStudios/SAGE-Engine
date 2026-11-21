#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "imgui.h"

#include "AssetPreview.h"
#include "FileBrowser.h"
#include "sage/anim/AnimGraph.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// Окно SAGE Animation Graph: сверху — предпросмотр модели, играющей граф,
// снизу — холст узлов, справа — инспектор выбранного узла и параметры.
//
// Предпросмотр — настоящая мини-сцена с компонентом Animation: граф играет
// ровно та же система (sage/anim/AnimationSystem), что и в игре, а не
// отдельный «проигрыватель для окна», который однажды разошёлся бы с игрой.
// Граф кладётся в компонент прямо из памяти (AnimationComponent::GraphAsset),
// поэтому правка видна сразу, до сохранения.
// ---------------------------------------------------------------------------
class AnimGraphPanel {
public:
    AnimGraphPanel();
    ~AnimGraphPanel();

    void Draw(EditorHost& host, bool* open);
    // Отпустить видеоресурсы предпросмотра — пока жив контекст GL.
    void Shutdown();
    // Открыть файл графа (путь на диске). modelHint — модель для
    // предпросмотра, если в графе её ещё нет (модель объекта из инспектора).
    void Open(EditorHost& host, const std::string& path, const std::string& modelHint = {});
    bool Save(EditorHost& host);

    bool HasGraph() const { return m_hasGraph; }
    const sage::anim::AnimGraph& Graph() const { return m_graph; }
    const std::string& Path() const { return m_path; }
    // Состояние предпросмотра — для самопроверки редактора.
    const sage::anim::AnimGraphInstance* PreviewInstance() const;

private:
    // --- Документ ---------------------------------------------------------
    bool SaveTo(EditorHost& host, const std::string& path);
    void PushUndo();
    void Undo();
    void Redo();
    // structural: изменилось то, что влияет на позу (провода, клипы, веса), —
    // предпросмотр перепривязывает граф. Перемещение узла — нет.
    void Changed(bool structural = true);

    // --- Части окна -------------------------------------------------------
    void DrawToolbar(EditorHost& host);
    void DrawEmpty(EditorHost& host);
    void DrawPreview(EditorHost& host, const ImVec2& size);
    void DrawCanvas(EditorHost& host);
    void DrawCanvasMenu(EditorHost& host);
    void DrawSide(EditorHost& host);
    void DrawNodeInspector(EditorHost& host);
    void DrawParams(EditorHost& host);
    void DrawBrowser(EditorHost& host);

    // --- Предпросмотр -----------------------------------------------------
    void SyncPreview(EditorHost& host, float dt);
    std::vector<std::string> ModelClipNames() const;

    // --- Холст ------------------------------------------------------------
    ImVec2 ToScreen(const glm::vec2& p) const;
    glm::vec2 ToCanvas(const ImVec2& s) const;
    ImVec2 NodeSize(const sage::anim::AnimGraphNode& n) const;
    ImVec2 InputPinPos(const sage::anim::AnimGraphNode& n, int pin) const;
    ImVec2 OutputPinPos(const sage::anim::AnimGraphNode& n) const;
    std::string NodeTitle(const sage::anim::AnimGraphNode& n) const;
    std::string NodeDetail(const sage::anim::AnimGraphNode& n) const;
    void AddNodeAt(sage::anim::AnimNodeKind kind, const glm::vec2& at);
    void DeleteSelected();
    void FrameAll();

    sage::anim::AnimGraph m_graph;
    bool m_hasGraph = false;
    std::string m_path;
    bool m_dirty = false;
    std::vector<sage::anim::AnimGraph> m_undo, m_redo;
    bool m_editUndo = false;   // снимок для текущего перетаскивания поля уже сделан
    int m_selected = 0;
    std::string m_status;
    int m_focusFrames = 0;

    // Холст
    glm::vec2 m_pan{40.0f, 40.0f};
    float m_zoom = 1.0f;
    ImVec2 m_origin{0, 0};
    ImVec2 m_canvasSize{0, 0};
    int m_dragNode = 0;
    bool m_dragMoved = false;
    int m_linkFrom = 0;        // тянем провод от выхода узла
    int m_linkToNode = 0;      // ... или от входа (провод снимается со входа)
    int m_linkToPin = -1;
    bool m_menuRequest = false;
    glm::vec2 m_menuAt{0.0f};
    bool m_frameRequest = false;

    // Предпросмотр
    std::unique_ptr<Scene> m_scene;
    entt::entity m_entity = entt::null;
    AssetPreview m_preview;
    std::string m_sceneModel;   // какая модель стоит в мини-сцене
    bool m_rebind = true;       // граф изменился — отдать новый в компонент
    bool m_playing = true;
    float m_playSpeed = 1.0f;
    std::unordered_map<std::string, float> m_values;   // текущие значения параметров

    // Диалоги
    FileBrowser m_browser;
    enum class Browse { None, SaveAs, ClipFile, PreviewModel };
    Browse m_browse = Browse::None;
    int m_browseNode = 0;
};
