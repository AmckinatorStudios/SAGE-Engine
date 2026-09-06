#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sage/ui/sageui/UIWindow.h"

// ---------------------------------------------------------------------------
// ДОКИНГ (§22–27 ТЗ).
//
// ЧТО ЭТО. Раскладка редактора: панели стоят в областях, области делятся
// полосами, несколько панелей в одной области становятся вкладками, панель
// можно перетащить, отцепить в отдельное окно и вернуть обратно, а всё
// получившееся — сохранить и восстановить при следующем запуске.
//
// ГЛАВНОЕ АРХИТЕКТУРНОЕ РЕШЕНИЕ: РАСКЛАДКА — ЭТО ДАННЫЕ, А НЕ ДЕРЕВО ЭЛЕМЕНТОВ.
//
// DockNode — маленькое дерево из «раздели пополам» и «здесь лежат вот эти
// панели», и в нём НЕТ ни одного указателя на элемент интерфейса. Панели
// названы строками-идентификаторами. Элементы строятся ПО этому дереву и
// перестраиваются, когда оно меняется.
//
// Почему не наоборот — не «дерево элементов и есть раскладка». Потому что
// раскладку надо сохранять в файл (§27), сравнивать с сохранённой, сбрасывать к
// стандартной и восстанавливать при следующем запуске. Всё это над деревом
// живых элементов делается через боль: элементы держат состояние, обработчики и
// текстуры, их нельзя просто так пересоздать, а сериализовать их положение —
// значит завести вторую, теневую модель. Здесь модель одна, и она с самого
// начала та, которую пишут в файл.
//
// САМИ ПАНЕЛИ ПРИ ЭТОМ ПЕРЕЕЗЖАЮТ, А НЕ ПЕРЕСОЗДАЮТСЯ. Перетащить панель — это
// перевесить её узел в другое место дерева документа; её содержимое, прокрутка,
// выделение и подписки остаются на месте. Иначе перетаскивание сбрасывало бы
// состояние панели, и это выглядело бы как «редактор забыл, что я делал».
//
// ЧЕГО ЗДЕСЬ НЕТ: знания о редакторе. Ни «инспектор», ни «иерархия», ни
// «вьюпорт» — докингу всё равно, что за панели он раскладывает.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

class DockSpace;

// Куда кладут панель относительно цели.
enum class DockSide { Center, Left, Right, Top, Bottom };

// ---------------------------------------------------------------------------
// Узел раскладки. Либо делит область пополам, либо держит вкладки.
// ---------------------------------------------------------------------------
struct DockNode {
    enum class Kind { Split, Tabs };

    Kind Type = Kind::Tabs;

    // --- Split ---
    bool Vertical = false;  // true — делит по вертикали (верх/низ)
    float Ratio = 0.5f;     // доля первого ребёнка, 0..1
    std::vector<std::unique_ptr<DockNode>> Children; // ровно два у Split

    // --- Tabs ---
    // Идентификаторы, а не указатели: ровно это и пишется в файл раскладки.
    std::vector<std::string> Panels;
    int Active = 0;

    bool IsSplit() const { return Type == Kind::Split; }
    bool Empty() const { return Type == Kind::Tabs && Panels.empty(); }
};

// ---------------------------------------------------------------------------
// Панель, которую можно пристыковать.
//
// Наследуется от обычного элемента: пристыкованная панель и плавающее окно —
// это ОДИН И ТОТ ЖЕ объект в разных местах дерева (§26). Двух реализаций
// панели — «в доке» и «в окне» — не существует.
// ---------------------------------------------------------------------------
class DockPanel : public UIElement {
public:
    DockPanel(std::string id, std::string title);

    const char* TypeName() const override { return "DockPanel"; }
    void OnAttach() override;

    const std::string& Id() const { return m_id; }
    const std::string& Title() const { return m_title; }
    DockPanel* SetTitle(std::string title);

    // Содержимое кладут сюда.
    UIElement* Body() const { return m_body; }

    DockPanel* SetClosable(bool on) { m_closable = on; return this; }
    bool Closable() const { return m_closable; }

private:
    std::string m_id, m_title;
    UIElement* m_body = nullptr;
    bool m_closable = true;
};

// ---------------------------------------------------------------------------
// Область докинга.
// ---------------------------------------------------------------------------
class DockSpace : public UIElement {
public:
    const char* TypeName() const override { return "DockSpace"; }
    void OnAttach() override;
    void Update(float dt) override;

    // Зарегистрировать панель. Она попадает в раскладку: рядом с nearId с
    // указанной стороны, а при пустом nearId — в корень.
    DockPanel* Add(DockPanel* panel, DockSide side = DockSide::Center,
                   const std::string& nearId = {});

    DockPanel* Find(const std::string& id) const;
    // Показать панель и сделать её активной вкладкой. Если она была закрыта —
    // вернуть в раскладку: пункт меню «Окно > Консоль» обязан её возвращать, а
    // не «включать» невидимо.
    void Focus(const std::string& id);
    void Close(const std::string& id);
    bool IsOpen(const std::string& id) const;
    std::vector<std::string> PanelIds() const;

    // --- Плавающие окна (§26) -----------------------------------------------
    //
    // Отцепить панель в отдельное окно и вернуть обратно. Панель при этом одна
    // и та же — переезжает её узел.
    void Float(const std::string& id, glm::vec2 at = {80.0f, 80.0f});
    void Dock(const std::string& id, DockSide side = DockSide::Center,
              const std::string& nearId = {});
    bool IsFloating(const std::string& id) const;

    // --- Раскладка как данные (§27) -----------------------------------------
    std::string SaveLayout() const;
    bool LoadLayout(const std::string& json);
    // Сбросить к состоянию «все панели в одной области».
    void ResetLayout();

    DockNode* Root() const { return m_root.get(); }
    // Пометить раскладку изменённой — дерево элементов пересоберётся к
    // следующему кадру. Публично: раскладку меняет и перетаскивание, и меню.
    void Invalidate() { m_dirty = true; }

    // --- Перетаскивание панелей (§24) ---------------------------------------
    //
    // Ведёт ОБЛАСТЬ, а не вкладка: подсветить место, куда упадёт панель, можно
    // только зная все области сразу. Вкладка лишь сообщает «меня тянут».
    void BeginDrag(const std::string& id);
    void DragTo(glm::vec2 screenPoint);
    void EndDrag(glm::vec2 screenPoint);
    bool Dragging() const { return !m_drag.empty(); }
    // Куда упадёт панель при текущем положении курсора. Пусто — никуда.
    const std::string& DropTargetId() const { return m_dropTarget; }
    DockSide DropSide() const { return m_dropSide; }

    float SplitterSize() const { return m_splitter; }
    DockSpace* SetSplitterSize(float size) { m_splitter = size; m_dirty = true; return this; }

private:
    struct Floating {
        std::string Id;
        Window* Win = nullptr;
    };

    void Rebuild();
    UIElement* BuildNode(DockNode& node, UIElement* parent);
    UIElement* BuildSplit(DockNode& node, UIElement* parent);
    UIElement* BuildTabs(DockNode& node, UIElement* parent);

    DockNode* FindTabsOf(const std::string& id, DockNode* from = nullptr) const;
    DockNode* FindParentOf(const DockNode* child, DockNode* from = nullptr) const;
    void RemoveFromLayout(const std::string& id);
    void Insert(const std::string& id, DockSide side, const std::string& nearId);
    void Prune(DockNode* node);
    // Область под курсором и сторона, к которой ближе всего край. Одна функция
    // на подсветку и на бросок: разойдись они — подсветка обещала бы одно, а
    // бросок делал другое.
    bool HitArea(glm::vec2 screenPoint, std::string& outPanelId, DockSide& outSide) const;
    void UpdateDropHint();

    std::unique_ptr<DockNode> m_root;
    std::vector<DockPanel*> m_panels;   // все зарегистрированные, включая закрытые
    std::vector<Floating> m_floating;
    UIElement* m_host = nullptr;        // сюда строится дерево областей
    UIElement* m_parking = nullptr;     // где ждут закрытые и переезжающие панели
    bool m_dirty = true;
    float m_splitter = 4.0f;

    std::string m_drag;         // какую панель тянут
    std::string m_dropTarget;   // рядом с какой она упадёт
    DockSide m_dropSide = DockSide::Center;
    UIElement* m_hint = nullptr; // подсветка места броска
};

} // namespace sage::ui::sui
