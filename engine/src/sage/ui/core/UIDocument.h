#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/ui/core/UINode.h"

// ---------------------------------------------------------------------------
// ДОКУМЕНТ ИНТЕРФЕЙСА — то, что редактирует редактор и загружает игра.
//
// ОДНА МОДЕЛЬ НА ДВОИХ (§66). Прежде у редактора была своя картина интерфейса, а
// у игры своя, и совпадали они настолько, насколько кто-то успевал их сводить.
// Здесь такого разрыва нет по устройству: UIDocument — единственное описание
// интерфейса, редактор правит ЕГО, рантайм считает и рисует ЕГО ЖЕ теми же
// подсистемами. Всё, что человек видит в холсте редактора, игрок увидит в
// точности так же, потому что это буквально один и тот же расчёт.
//
// ЧТО ТАКОЕ ДОКУМЕНТ. Хранилище узлов (владеет ими), иерархия, набор холстов,
// стили и тема, ресурсы и версия формата. Документ НЕ содержит ни рисования, ни
// ввода, ни раскладки: он данные, а не поведение. Раскладку считает
// UILayoutSolver, рисует UIDrawBuilder, ввод раздаёт UIInputRouter — каждый над
// одним и тем же документом и ни один не хранит своей копии дерева.
//
// ГРЯЗНЫЕ ФЛАГИ (§87). Документ помечает, что именно изменилось, а не
// «изменилось хоть что-то»: смена цвета текста не обязана пересчитывать
// раскладку всей сцены. Помечает — сам документ, а не тот, кто правит: иначе
// про пометку рано или поздно забудут.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Что именно устарело. Битовая маска: одна правка часто портит несколько
// стадий (смена текста — и раскладку, и рисование).
enum UIDirtyFlags : uint32_t {
    UIDirty_None      = 0,
    UIDirty_Layout    = 1u << 0, // размеры/положения
    UIDirty_Transform = 1u << 1, // только матрицы, размеры прежние
    UIDirty_Visual    = 1u << 2, // оформление: цвет, картинка, рамка
    UIDirty_Text      = 1u << 3, // разбивка текста на строки
    UIDirty_Mask      = 1u << 4,
    UIDirty_Effect    = 1u << 5,
    UIDirty_Hierarchy = 1u << 6, // состав дерева и порядок
    UIDirty_Style     = 1u << 7, // стиль/тема — пересчитать всё оформление
    UIDirty_All       = 0xFFFFFFFFu,
};

// Как холст переводит логические координаты в пиксели экрана (§78–81).
struct UICanvasSettings {
    enum class Space {
        Screen,  // поверх кадра, координаты экрана
        Overlay, // то же, но всегда поверх всего остального интерфейса
        World,   // холст в мире; матрицу даёт игра, UI даёт только математику
    };
    // Как опорное разрешение соотносится с фактическим (§79, §80).
    enum class ScaleMode {
        Pixels,      // 1 логическая единица = 1 пиксель экрана
        ScaleWithSize, // подгонять под опорное разрешение
        Physical,    // держать физический размер по DPI
    };
    enum class AspectMode {
        Expand, // тянуться за обеими сторонами (смешанный масштаб)
        Fit,    // вписаться целиком, поля по краям
        Crop,   // заполнить, лишнее уходит за край
    };

    Space Kind = Space::Screen;
    ScaleMode Scale = ScaleMode::ScaleWithSize;
    AspectMode Aspect = AspectMode::Expand;
    glm::vec2 Reference{1920.0f, 1080.0f};
    // 0 — тянуться за шириной, 1 — за высотой, 0.5 — среднее. Для интерфейса,
    // который должен помещаться и в узкое, и в широкое окно, среднее честнее.
    float MatchWidthOrHeight = 0.5f;
    float UserScale = 1.0f; // множитель «размер интерфейса» из настроек игры (§107)
    float DpiScale = 1.0f;  // множитель плотности экрана (§78)
    int SortOrder = 0;      // порядок между холстами
    // Безопасная область (§77): вырезы, скруглённые углы, панели системы.
    UIEdges SafeArea{0.0f, 0.0f, 0.0f, 0.0f};
    bool RespectSafeArea = false;
};

class UIDocument {
public:
    UIDocument();
    UIDocument(const UIDocument&) = delete;
    UIDocument& operator=(const UIDocument&) = delete;
    UIDocument(UIDocument&&) noexcept = default;
    UIDocument& operator=(UIDocument&&) noexcept = default;

    // --- Узлы ---------------------------------------------------------------
    //
    // Документ ВЛАДЕЕТ узлами. Наружу отдаются указатели и номера; указатель
    // живёт до удаления узла, номер — всегда (по нему всегда можно спросить,
    // существует ли узел).
    UINode* Create(const std::string& name, UINodeId parent = kUIInvalidNode);
    // Создать по имени виджета из реестра (§60): "button", "slider"...
    UINodeId CreateWidget(std::string_view widgetId, UINodeId parent = kUIInvalidNode);

    UINode* Find(UINodeId id);
    const UINode* Find(UINodeId id) const;
    UINode* FindByGuid(std::string_view guid);
    UINode* FindByName(std::string_view name); // первый по обходу сверху вниз
    // Путь вида "Root/Panel/Title" — так на узлы ссылаются анимации и привязки.
    UINode* FindByPath(std::string_view path);
    std::string PathOf(UINodeId id) const;

    void Destroy(UINodeId id); // вместе с поддеревом
    // Копия поддерева рядом с оригиналом (или в указанного родителя).
    UINodeId Duplicate(UINodeId id, UINodeId newParent = kUIInvalidNode);

    // --- Иерархия -----------------------------------------------------------
    //
    // index < 0 — в конец. Возвращает false, если перенос создал бы цикл (§135):
    // родитель внутри собственного поддерева — не «странная вёрстка», а
    // бесконечный обход при первой же отрисовке.
    bool Reparent(UINodeId id, UINodeId newParent, int index = -1);
    bool Move(UINodeId id, int index); // сменить место среди соседей
    bool IsAncestorOf(UINodeId ancestor, UINodeId node) const;

    const std::vector<UINodeId>& Roots() const { return m_roots; }
    // Все узлы в порядке обхода (родитель раньше детей). Готовый порядок для
    // раскладки и рисования: считать его заново в каждой подсистеме — верный
    // способ получить три разных ответа.
    const std::vector<UINodeId>& Ordered() const;
    size_t NodeCount() const { return m_nodes.size(); }

    // Обход поддерева сверху вниз. Возврат false из посетителя — не заходить в
    // детей этого узла.
    void Traverse(UINodeId root, const std::function<bool(UINode&)>& fn);
    void TraverseConst(UINodeId root, const std::function<bool(const UINode&)>& fn) const;

    // --- Холст --------------------------------------------------------------
    UICanvasSettings& Canvas() { return m_canvas; }
    const UICanvasSettings& Canvas() const { return m_canvas; }

    // Множитель холста при данном размере экрана и итоговая область вёрстки в
    // ЛОГИЧЕСКИХ единицах. Одна функция на всех: и раскладка, и редактор, и
    // ввод обязаны получать одно и то же (§114).
    float ScaleFor(glm::vec2 screenPixels) const;
    UIRect ViewportFor(glm::vec2 screenPixels) const;

    // --- Грязные флаги (§87, §88) ------------------------------------------
    void MarkDirty(uint32_t flags) { m_dirty |= flags; }
    void MarkDirty(UINodeId id, uint32_t flags);
    uint32_t Dirty() const { return m_dirty; }
    bool IsDirty(uint32_t flags) const { return (m_dirty & flags) != 0; }
    void ClearDirty() { m_dirty = UIDirty_None; m_dirtyNodes.clear(); }
    const std::vector<UINodeId>& DirtyNodes() const { return m_dirtyNodes; }

    // --- Прочее -------------------------------------------------------------
    const std::string& Name() const { return m_name; }
    void SetName(std::string name) { m_name = std::move(name); }

    // Версия формата документа (§65). Пишется в файл; читатель старого файла
    // прогоняет миграции до текущей.
    static int FormatVersion();

    void Clear();

    // Уникальный Guid — для новых узлов и для разведения одинаковых при
    // вставке копии.
    std::string MakeGuid();

private:
    void Detach(UINode& node);
    void CollectSubtree(UINodeId id, std::vector<UINodeId>& out) const;
    void RebuildOrder() const;

    std::unordered_map<UINodeId, std::unique_ptr<UINode>> m_nodes;
    std::vector<UINodeId> m_roots;
    UICanvasSettings m_canvas;
    std::string m_name;

    UINodeId m_nextId = 1;
    uint64_t m_guidCounter = 0;
    uint32_t m_dirty = UIDirty_All;
    std::vector<UINodeId> m_dirtyNodes;

    mutable std::vector<UINodeId> m_order;
    mutable bool m_orderValid = false;
};

} // namespace sage::ui
