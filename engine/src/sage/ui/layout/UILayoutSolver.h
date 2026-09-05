#pragma once
#include <unordered_map>
#include <vector>

#include "sage/ui/core/UIContext.h"
#include "sage/ui/core/UIDocument.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMaskStack.h"

// ---------------------------------------------------------------------------
// РЕШАТЕЛЬ РАСКЛАДКИ (§82, §83 ТЗ) — отдельный проход, отделённый от рисования.
//
// ПОЧЕМУ ЭТО ГЛАВНОЕ АРХИТЕКТУРНОЕ РАЗДЕЛЕНИЕ (§117). Пока раскладка считается
// внутри рисования, нельзя ни ответить на «где сейчас этот элемент» без
// отрисовки кадра, ни поймать по нему мышь, ни показать его границы в
// редакторе, ни проверить вёрстку тестом без GPU. Ровно этим и болела прежняя
// система: редактор считал прямоугольники СВОЕЙ копией формул и неизбежно
// расходился с тем, что рисовалось.
//
// Здесь один проход превращает документ в плоский массив посчитанных узлов, и
// дальше ВСЕ (рисование, попадание курсором, редактор, отладка, тесты) читают
// этот массив. Разойтись им негде.
//
// ДВА ПРОХОДА. Сначала снизу вверх меряется содержимое (сколько нужно тексту,
// картинке, содержимому контейнера), потом сверху вниз расставляются
// прямоугольники. Иначе «панель по размеру содержимого внутри строки, которая
// сама по размеру содержимого» не считается вовсе.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Посчитанный узел — то, что видят все остальные подсистемы.
struct UIResolvedNode {
    UINodeId Id = kUIInvalidNode;
    UINodeId Parent = kUIInvalidNode;
    int ParentIndex = -1;
    int Depth = 0;

    // Прямоугольник В ЭКРАННЫХ ПИКСЕЛЯХ, без учёта собственного поворота и
    // масштаба (их несёт World). Для подавляющего большинства узлов World —
    // единичная матрица, и Rect — это ровно то, что видно.
    UIRect Rect{};
    // Полное преобразование локальных координат узла в экранные: поворот,
    // масштаб и наклон, накопленные по всей цепочке предков.
    glm::mat3 World{1.0f};
    bool Transformed = false; // World отличается от сдвига — нужен общий путь

    float Opacity = 1.0f;   // уже перемноженная по цепочке (§28)
    UIBlendMode Blend = UIBlendMode::Normal;
    float Scale = 1.0f;     // масштаб холста (логическая единица → пиксель)

    int MaskState = 0;      // номер состояния в UIMaskStack
    UIRect Clip{};          // итоговое окно ножниц
    bool Clipped = false;

    uint64_t SortKey = 0;   // слой → порядок → место в дереве (§26)
    int Layer = 0;

    bool Visible = true;       // с учётом предков
    bool Enabled = true;       // с учётом предков
    bool HitTestable = false;  // есть UIInteraction и он включён
    bool Culled = false;       // целиком за краем экрана или маски (§89)

    // Размер, который узел запросил на этапе измерения. Нужен отладке и
    // редактору: «почему панель шире, чем я задал» — вопрос именно к нему.
    glm::vec2 Measured{0.0f, 0.0f};
};

// Статистика прохода (§110).
struct UILayoutStats {
    int Nodes = 0;
    int Visible = 0;
    int Culled = 0;
    int MeasurePasses = 0;
    int TextLayouts = 0;
    int MaskStates = 0;
    double LayoutMs = 0.0;
};

class UILayoutSolver {
public:
    // Посчитать документ для экрана из контекста. viewport — область в
    // ЛОГИЧЕСКИХ единицах (обычно UIDocument::ViewportFor).
    void Solve(UIDocument& doc, const UIContext& ctx);

    const std::vector<UIResolvedNode>& Nodes() const { return m_nodes; }
    const UIMaskStack& Masks() const { return m_masks; }
    const UILayoutStats& Stats() const { return m_stats; }

    // Номер посчитанного узла по его номеру в документе; -1 — узла нет в
    // раскладке (выключен вместе с предком).
    int IndexOf(UINodeId id) const;
    const UIResolvedNode* Get(UINodeId id) const;

    // Прямоугольник узла в экранных пикселях — то, что спрашивает редактор.
    bool RectOf(UINodeId id, UIRect& out) const;

    float CanvasScale() const { return m_scale; }
    const UIRect& Viewport() const { return m_viewport; }

private:
    // Прямоугольник, назначенный контейнером конкретному ребёнку. Передаётся
    // «в один шаг» между ArrangeChildren и ArrangeNode: иначе пришлось бы
    // тащить необязательный параметр через всю рекурсию ради случая, который
    // касается ровно одного вызова.
    struct PendingRect {
        bool Valid = false;
        UIRect Rect{};
    };
    PendingRect m_pendingRect;

    struct MeasureCache {
        glm::vec2 Size{0.0f, 0.0f};
        bool Valid = false;
    };

    glm::vec2 MeasureNode(UIDocument& doc, UINode& node, glm::vec2 available);
    glm::vec2 MeasureContainer(UIDocument& doc, UINode& node, const UILayout& layout,
                               glm::vec2 available);
    void ArrangeNode(UIDocument& doc, UINode& node, const UIRect& parentRect,
                     int parentIndex, int maskState, float opacity, bool visible,
                     bool enabled, const glm::mat3& parentWorld, int depth,
                     uint32_t layerBase);
    void ArrangeChildren(UIDocument& doc, UINode& node, const UIRect& contentRect,
                         int selfIndex, int maskState, float opacity, bool visible,
                         bool enabled, const glm::mat3& world, int depth);

    UIRect ResolveRect(const UITransform& t, const UIRect& parent, glm::vec2 content) const;

    std::vector<UIResolvedNode> m_nodes;
    std::unordered_map<UINodeId, int> m_index;
    std::unordered_map<UINodeId, MeasureCache> m_measure;
    UIMaskStack m_masks;
    UILayoutStats m_stats;
    const UIContext* m_ctx = nullptr;
    UIRect m_viewport{};
    float m_scale = 1.0f;
    uint32_t m_sortCounter = 0;
};

// --- Открытая математика раскладки ------------------------------------------
//
// Отдельными функциями, потому что ими пользуются и решатель, и редактор
// (перетаскивание пишет обратно в Offset), и тесты.

// Прямоугольник узла внутри родителя при известном размере содержимого.
UIRect UIResolveTransform(const UITransform& t, const UIRect& parent, glm::vec2 content);

// ОБРАТНАЯ задача: какой Offset поставит узел размера size в эту точку.
// Нужна везде, где положение задаётся мышью, а хранится якорем.
glm::vec2 UIOffsetForTopLeft(const UITransform& t, glm::vec2 topLeft, glm::vec2 size,
                             const UIRect& parent);

// Разложить детей контейнера. slots — желаемые размеры на входе, положение и
// итоговый размер на выходе. Возвращает занятую содержимым область.
struct UILayoutSlot {
    glm::vec2 Size{0.0f, 0.0f};
    glm::vec2 Pos{0.0f, 0.0f};
    bool StretchMain = false; // ребёнок тянется вдоль основной оси
    bool StretchCross = false;
};
glm::vec2 UIApplyLayout(const UILayout& layout, const UIRect& container,
                        std::vector<UILayoutSlot>& slots);

} // namespace sage::ui
