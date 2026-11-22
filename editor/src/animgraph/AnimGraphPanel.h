#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

#include "FileBrowser.h"
#include "sage/anim/AnimGraph.h"

class EditorHost;
struct AnimationComponent;

// ---------------------------------------------------------------------------
// Окно SAGE Animation Graph — вкладка пространства Animation: холст узлов и
// параметры графа (имя, умолчание, диапазон). Граф играет на КОПИИ цели в
// сцене предпросмотра пространства (animws/AnimStage.h) той же системой
// анимации, что в игре, прямо из памяти — правка видна до сохранения, а объект
// уровня не получает ни графа, ни значений параметров.
//
// Значения параметров для предпросмотра и свойства выбранного узла — в
// Animation Inspector, раздел Animation Graph.
// ---------------------------------------------------------------------------
class AnimGraphPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
    // Открыть файл графа (путь на диске).
    void Open(EditorHost& host, const std::string& path);
    bool Save(EditorHost& host);
    // Добавить узел (меню холста; в самопроверке — правка документа без мыши).
    void AddNodeAt(EditorHost& host, sage::anim::AnimNodeKind kind, const glm::vec2& at);
    // Свойства выбранного узла — в Inspector. false — узел не выбран (или
    // после него выбрали объект): инспектор рисует объект.
    bool DrawInspector(EditorHost& host);
    // Вернуть объекту предпросмотра его собственный граф (уход из
    // пространства, закрытие окна, смена выделения).
    void ReleasePreview(EditorHost& host);

    bool HasGraph() const { return m_hasGraph; }
    // Несохранённые правки открытого документа (общая защита от потери при
    // открытии другого — EditorLayer::GuardAnimDocument).
    bool Dirty() const { return m_hasGraph && m_dirty; }
    // Отбросить правки: документ заново из файла. Рабочая копия всегда привязана
    // к файлу, поэтому безымянной не бывает.
    void DiscardChanges(EditorHost& host);
    const sage::anim::AnimGraph& Graph() const { return m_graph; }
    const std::string& Path() const { return m_path; }
    // Окно открыто с графом — граф предпросмотра ведёт оно.
    bool PreviewActive() const { return m_open && m_hasGraph; }
    int SelectedNode() const { return m_selected; }
    void SelectNode(EditorHost& host, int id);
    // Состояние графа на объекте предпросмотра (nullptr — объекта нет).
    const sage::anim::AnimGraphInstance* LiveInstance(EditorHost& host) const;

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
    void DrawCanvas(EditorHost& host);
    void DrawCanvasMenu(EditorHost& host);
    void DrawNodeInspector(EditorHost& host);
    void DrawParams(EditorHost& host);
    void DrawBrowser(EditorHost& host);

    // --- Предпросмотр на выбранном объекте --------------------------------
    AnimationComponent* Target(EditorHost& host) const;
    void SyncPreview(EditorHost& host);
    std::vector<std::string> ModelClipNames(EditorHost& host) const;

    // --- Холст ------------------------------------------------------------
    ImVec2 ToScreen(const glm::vec2& p) const;
    glm::vec2 ToCanvas(const ImVec2& s) const;
    ImVec2 NodeSize(const sage::anim::AnimGraphNode& n) const;
    ImVec2 InputPinPos(const sage::anim::AnimGraphNode& n, int pin) const;
    ImVec2 OutputPinPos(const sage::anim::AnimGraphNode& n) const;
    std::string NodeTitle(const sage::anim::AnimGraphNode& n) const;
    std::string NodeDetail(const sage::anim::AnimGraphNode& n) const;
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

    // Узел в Inspector: пока выделение объектов то же, что в момент выбора
    // узла. Выбрали объект — инспектор снова его.
    bool m_inspectNode = false;
    unsigned m_inspectSelection = 0;   // EditorSelection::Changes в момент выбора узла

    // Холст
    glm::vec2 m_pan{40.0f, 40.0f};
    float m_zoom = 1.0f;
    ImVec2 m_origin{0, 0};
    ImVec2 m_canvasSize{0, 0};
    ImVec2 m_lastCanvasSize{0, 0};
    int m_dragNode = 0;
    bool m_dragMoved = false;
    int m_linkFrom = 0;        // тянем провод от выхода узла
    int m_linkToNode = 0;      // ... или от входа (провод снимается со входа)
    int m_linkToPin = -1;
    bool m_menuRequest = false;
    glm::vec2 m_menuAt{0.0f};
    bool m_frameRequest = false;

    // Предпросмотр: граф отдаётся сцене Animation общим неизменяемым экземпляром.
    std::shared_ptr<const sage::anim::AnimGraph> m_shared;
    bool m_rebind = true;       // граф изменился — отдать новый
    bool m_open = false;

    // Диалоги
    FileBrowser m_browser;
    enum class Browse { None, SaveAs, ClipFile };
    Browse m_browse = Browse::None;
    int m_browseNode = 0;
};
