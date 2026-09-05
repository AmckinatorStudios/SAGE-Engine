#pragma once
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "../AssetPreview.h"
#include "../FileBrowser.h"
#include "sage/render/Framebuffer.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/render/UIEngineResources.h"

class EditorHost;
class UIRenderer;

// ---------------------------------------------------------------------------
// РЕДАКТОР ДОКУМЕНТА ИНТЕРФЕЙСА (§68–76 ТЗ).
//
// ГЛАВНОЕ: У РЕДАКТОРА НЕТ СВОЕЙ МОДЕЛИ ИНТЕРФЕЙСА (§66, §119). Он правит
// ровно тот sage::ui::UIDocument, который загрузит игра, и показывает его
// ровно теми же подсистемами: та же раскладка (UILayoutSolver), та же
// подготовка кадра (UIDrawBuilder), тот же рисующий (IUIDrawBackend). Ни одной
// «редакторской» формулы положения здесь нет — рамки выделения берутся из
// посчитанной раскладки, а не считаются заново.
//
// Из этого следует то, ради чего всё и делалось: расхождение между тем, что
// видно в редакторе, и тем, что увидит игрок, невозможно ПО УСТРОЙСТВУ, а не
// потому, что кто-то аккуратно синхронизировал два расчёта.
//
// ЧТО ЗДЕСЬ ЕСТЬ:
//   • дерево — создать, выбрать, перетащить, переставить, спрятать, запереть;
//   • холст — настоящий кадр интерфейса в разрешении игры, с перемещением и
//     изменением размера мышью, привязками, выравниванием и стрелками;
//   • инспектор — ПО РЕЕСТРУ компонентов: показываются только те свойства,
//     которые у объекта есть (§72), и ни одного списка полей в самом редакторе;
//   • слои, маски, эффекты, стили и префабы — как обычные компоненты;
//   • отладочные слои и профайлер — то, что отвечает на «почему так» (§109).
//
// ЧЕГО ЗДЕСЬ НЕТ: ни одного знания о том, что означают элементы интерфейса в
// игре. Редактор правит документ, а не игру.
// ---------------------------------------------------------------------------
class UIDocumentPanel {
public:
    UIDocumentPanel();
    ~UIDocumentPanel();

    void Draw(EditorHost& host, bool* open);
    void RequestFocus() { m_focusFrames = 3; }

    // Открыть документ по пути (из панели ассетов двойным щелчком).
    bool Open(const std::string& path);

private:
    // --- части окна ---------------------------------------------------------
    void DrawToolbar(EditorHost& host);
    void DrawHierarchy(EditorHost& host, float width);
    void DrawCanvas(EditorHost& host);
    void DrawInspector(EditorHost& host, float width);
    void DrawTreeNode(EditorHost& host, sage::ui::UINodeId id, int depth);
    void DrawCreateMenu(EditorHost& host);
    void DrawComponentEditor(sage::ui::UINode& node, sage::ui::UIComponent& comp);
    void DrawEffectsEditor(sage::ui::UINode& node);
    void DrawAlignTools();
    bool DrawProperty(void* data, const sage::ui::UIProperty& prop, const char* idSuffix);

    // --- работа с документом ------------------------------------------------
    void NewDocument();
    bool Save(const std::string& path);
    void PushUndo();
    void Undo();
    void Redo();
    void Select(sage::ui::UINodeId id, bool additive);
    bool IsSelected(sage::ui::UINodeId id) const;
    void DeleteSelected();
    void DuplicateSelected();
    // Прямоугольник выделения в пикселях кадра — объединение выделенных.
    sage::ui::UIRect SelectionBounds() const;
    // Записать новое положение/размер узла обратно в его Transform. Пиксели
    // кадра переводятся в логические единицы холста здесь и только здесь.
    void ApplyRect(sage::ui::UINodeId id, glm::vec2 topLeft, glm::vec2 size, bool sizeChanged);
    void MoveSelection(glm::vec2 deltaPixels);

    void Recompute(int frameW, int frameH);

    // --- состояние ----------------------------------------------------------
    sage::ui::UIRuntime m_rt;
    sage::ui::UIEngineResources m_res;
    std::unique_ptr<UIRenderer> m_renderer;
    std::unique_ptr<Framebuffer> m_target;

    std::vector<sage::ui::UINodeId> m_selection;
    sage::ui::UINodeId m_primary = sage::ui::kUIInvalidNode;
    sage::ui::UINodeId m_renaming = sage::ui::kUIInvalidNode;
    char m_renameBuf[128] = {0};

    std::string m_path;      // открытый файл; пусто — документ ещё не сохраняли
    bool m_dirty = false;
    int m_focusFrames = 0;

    // Показ холста. Масштаб и сдвиг — свойства ПОКАЗА, а не вёрстки: сам кадр
    // всегда считается в разрешении игры, меняется только то, как крупно его
    // видно.
    float m_zoom = 1.0f;
    ImVec2 m_pan{0.0f, 0.0f};
    bool m_fitOnce = true;
    float m_backdrop = 1.0f;
    bool m_snapToGrid = false;
    float m_gridStep = 8.0f;
    bool m_showGrid = true;

    // Отладочные слои — те же флаги, что у рантайма (§109).
    uint32_t m_debug = 0;

    // Перетаскивание на холсте. Ручки нумеруются по сторонам света, чтобы код
    // изменения размера читался, а не расшифровывался.
    enum class Drag { None, Move, N, S, W, E, NW, NE, SW, SE };
    Drag m_drag = Drag::None;
    ImVec2 m_dragStartMouse{0.0f, 0.0f};
    std::vector<std::pair<sage::ui::UINodeId, sage::ui::UIRect>> m_dragStart;
    sage::ui::UIRect m_dragStartUnion{};
    bool m_dragPushedUndo = false;

    // Отмена правки — снимками документа. Не «список операций»: документ
    // читается и пишется одним и тем же кодом, а операций в редакторе десятки,
    // и своя отмена у каждой рано или поздно разъедется с моделью.
    std::vector<std::string> m_undo;
    std::vector<std::string> m_redo;

    AssetPreview m_preview;
    FileBrowser m_browser;
    std::string* m_browseTarget = nullptr;
    bool m_browseIsSave = false;
};
