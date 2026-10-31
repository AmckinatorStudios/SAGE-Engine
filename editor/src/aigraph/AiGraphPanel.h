#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

#include "sage/ai/Graph.h"
#include "sage/ai/Nodes.h"

class EditorHost;
class ScriptEngine;
namespace sage::ai { class World; }

// ---------------------------------------------------------------------------
// РЕДАКТОР SAGE AI GRAPH — узлы, провода, переменные графа.
//
// ЧТО НА ЭКРАНЕ:
//   • СВЕРХУ — файл графа: создать, открыть, сохранить, отменить; вид графа
//     (поведение, задача, функция); инструменты плагинов; кнопки отладчика,
//     когда идёт Play.
//   • СЛЕВА — сам граф: имя, описание, входы, выходы и локальные переменные.
//   • В ЦЕНТРЕ — холст: узлы, провода, группы. Правая кнопка — палитра узлов
//     с поиском; провод, отпущенный в пустоту, открывает палитру, где видны
//     только узлы, которые к нему подходят.
//   • СПРАВА — выбранный узел: что он делает, его настройки и значения
//     несоединённых входов, комментарий, точка останова.
//
// ОТКУДА РЕДАКТОР ЗНАЕТ УЗЛЫ. Ни одного — по имени. Палитра, пины и окно
// свойств строятся по реестру мира ИИ. Мир для редактирования — свой
// движок скриптов БЕЗ сцены, в который загружена библиотека ИИ проекта
// (assets/ai/*.lua и *.aigraph): так в палитре видны и узлы, объявленные на
// Lua, и Lua-задачи с их входами, и другие графы проекта.
//
// ПРОВОДА ПРОВЕРЯЮТСЯ ДО ТОГО, КАК ИХ ПРОВЕЛИ. Несовместимый провод не
// рисуется вовсе, а у курсора — причина (тип не подходит, правило проекта,
// правило узла). Граф, собранный редактором, не падает в игре на «строка в
// вектор».
//
// ОТЛАДКА. Пока идёт Play, холст показывает выполнение выбранного
// экземпляра: шагавшие узлы светятся и остывают, узлы, на которых стоят
// ветки, обведены, у пинов — последние значения. Точка останова — F9 или
// красная метка в заголовке узла.
// ---------------------------------------------------------------------------
// РЕЖИМ РЕДАКТОРА — то, чем SAGE AI Graph отличается от Visual Scripting.
// Холст, палитра, проверка, переменные, отладка на холсте — общие; разные —
// виды графов, папка и расширение файлов и, главное, набор узлов (назначение
// узла, sage::ai::NodeDomain): задачи ИИ в Visual Scripting не предлагаются, а
// On Update объекта — в SAGE AI Graph.
struct GraphEditorMode {
    std::string WindowId;     // имя окна и панели в реестре
    std::string Title;        // заголовок (переводится T())
    std::string Folder;       // папка в assets
    std::string Extension;    // расширение файлов
    std::vector<sage::ai::GraphKind> Kinds;
    sage::ai::GraphKind DefaultKind = sage::ai::GraphKind::Task;
    bool AiTools = false;     // инструменты ИИ: кнопка «Отладчик ИИ», задачи
    static GraphEditorMode Ai();
    static GraphEditorMode Visual();
};

class AiGraphPanel {
public:
    explicit AiGraphPanel(GraphEditorMode mode = GraphEditorMode::Ai());
    ~AiGraphPanel();
    const GraphEditorMode& Mode() const { return m_mode; }

    void Draw(EditorHost& host, bool* open);
    // Открыть файл графа (путь на диске).
    void Open(EditorHost& host, const std::string& path);
    void RequestFocus(int frames = 3) { m_focusFrames = frames; }
    // Перечитать библиотеку ИИ проекта (узлы и задачи из Lua, другие графы).
    void ReloadLibrary(EditorHost& host);
    sage::ai::World* Library() const;

    bool Dirty() const { return m_dirty; }
    const std::string& Path() const { return m_path; }
    const sage::ai::GraphDoc& Doc() const { return m_doc; }
    // Для самопроверки и инструментов: выделить узел и показать его.
    void FocusNode(int nodeId);
    // Открыть граф проекта по имени задачи (assets/ai и assets/visual). Граф
    // вида, которого этот редактор не ведёт, открывается в своём редакторе.
    void OpenTaskGraph(EditorHost& host, const std::string& task);
    // Отладчик: какой экземпляр показывать на холсте (номер выполнения).
    void SetDebugRun(int run) { m_debugRun = run; }

private:
    // --- Файл -------------------------------------------------------------
    void NewGraph(EditorHost& host, sage::ai::GraphKind kind);
    bool Save(EditorHost& host);
    bool SaveAs(EditorHost& host, const std::string& name);
    std::string AiDir(EditorHost& host) const;
    // Найти файл графа проекта по имени (в обеих папках графов).
    std::string FindGraphFile(EditorHost& host, const std::string& name, sage::ai::GraphKind* kind) const;
    void Revalidate();
    void DrawBottom(EditorHost& host);
    // Переименование переменной графа: узлы, которые её читают и пишут, — за ней.
    void RenameVar(const std::string& scope, const std::string& from, const std::string& to);
    void PushUndo();
    // Снимок отмены на начало правки поля, а не на каждое изменение.
    void EditBegin();
    void Undo();
    void Redo();
    void MarkChanged();

    // --- Части окна --------------------------------------------------------
    void DrawToolbar(EditorHost& host);
    void DrawGraphSide(EditorHost& host);
    void DrawVarList(EditorHost& host, const char* title, std::vector<sage::ai::GraphVar>& vars, const char* id);
    void DrawNodeSide(EditorHost& host);
    void DrawCanvas(EditorHost& host);
    void DrawPalette(EditorHost& host);
    void DrawPaletteList(sage::ai::World& lib);
    void DrawNodeMenu(EditorHost& host);
    void DrawOpenPopup(EditorHost& host);
    void DrawSaveAsPopup(EditorHost& host);
    void DrawEmpty(EditorHost& host);

    // --- Холст ---------------------------------------------------------------
    struct PinHit {
        int Node = 0;
        std::string Pin;
        bool Output = false;
        std::string Type;
        ImVec2 Pos;
    };
    struct NodeLayout {
        ImVec2 Min, Max;           // на экране
        float HeaderH = 0.0f;
        std::vector<PinHit> Pins;
    };
    sage::ai::NodePins PinsOf(const sage::ai::GraphNode& n) const;
    const sage::ai::NodeType* TypeOf(const sage::ai::GraphNode& n) const;
    // Граф, который узел зовёт (Run Task, Call Function), если это граф проекта.
    std::string LinkedGraph(const sage::ai::GraphNode& n) const;
    ImVec2 ToScreen(float x, float y) const;
    ImVec2 ToGraph(ImVec2 screen) const;
    void Layout(const sage::ai::GraphNode& n, NodeLayout& out) const;
    bool CanLink(const PinHit& a, const PinHit& b, std::string* why) const;
    void Link(const PinHit& a, const PinHit& b);
    int AddNodeAt(const std::string& type, ImVec2 graphPos);
    void DeleteSelection();
    void CopySelection();
    void Paste(ImVec2 graphPos);
    void Duplicate();
    void FrameAll(ImVec2 size);

    sage::ai::GraphDoc m_doc;
    std::string m_path;
    bool m_hasGraph = false;
    bool m_dirty = false;
    std::vector<std::string> m_undo, m_redo;
    std::string m_status;
    int m_focusFrames = 0;

    // Библиотека для редактирования: свой движок скриптов без сцены.
    std::unique_ptr<ScriptEngine> m_lib;
    std::string m_libProject;
    std::string m_libError;

    // Вид холста.
    ImVec2 m_origin{0, 0};       // левый верхний угол холста на экране
    ImVec2 m_canvasSize{0, 0};
    ImVec2 m_pan{0, 0};
    float m_zoom = 1.0f;
    bool m_frameRequest = false;

    // Выделение и действия мышью.
    std::set<int> m_selected;
    int m_selectedGroup = 0;
    bool m_draggingNodes = false;
    bool m_boxSelecting = false;
    ImVec2 m_boxStart{0, 0};
    bool m_linking = false;
    PinHit m_linkFrom;
    int m_draggingGroup = 0;
    int m_resizingGroup = 0;
    std::vector<int> m_groupNodes;
    bool m_pushedForDrag = false;

    // Палитра.
    bool m_paletteRequest = false;
    ImVec2 m_paletteAt{0, 0};
    char m_search[128] = {};
    bool m_paletteFromLink = false;
    PinHit m_paletteLink;
    int m_menuNode = 0;
    bool m_menuRequest = false;
    bool m_openRequest = false;
    bool m_saveAsRequest = false;
    char m_saveName[96] = {};

    std::string m_clipboard;
    bool m_editUndo = false;

    GraphEditorMode m_mode;
    // Проверка графа (sage/ai/GraphValidate.h) — пересчёт при изменении.
    bool m_issuesDirty = true;
    std::vector<std::pair<int, std::pair<bool, std::string>>> m_issues;   // узел, (ошибка?, текст)
    int m_bottomTab = 0;   // 0 — проблемы, 1 — вывод выполнения
    bool m_bottomOpen = true;
    int m_debugRun = 0;
};
