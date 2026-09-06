#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/input/UIEvent.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"

// ---------------------------------------------------------------------------
// SAGE UI — ОБЪЕКТНЫЙ СЛОЙ.
//
// ЧТО ЭТО И ПОЧЕМУ ИМЕННО СЛОЙ, А НЕ НОВАЯ СИСТЕМА.
//
// Ниже уже лежит полноценное ядро интерфейса (sage/ui/core, layout, render,
// input, style): дерево узлов с компонентами, решатель раскладки в два прохода,
// маски, эффекты, команды рисования с батчингом, маршрутизатор ввода с фазами
// capture/target/bubble, фокус, навигация, темы, сериализация и реестры. Всё
// это проверено кадровыми тестами и работает в игре.
//
// Чего у ядра НЕТ — удобного объектного лица. Собрать меню там значит:
//
//     UINode& n = *doc.Create("Button", parent);
//     n.Ensure<UIFill>().Color = ...;
//     n.Ensure<UIInteraction>().Command = "menu.play";
//
// Это правильно для сериализации и для редактора, но неправильно для человека,
// который пишет игру или панель редактора. Ему нужно:
//
//     auto* play = ui.Create<Button>("Играть");
//     play->OnClick([] { StartGame(); });
//
// Поэтому UIElement — не обёртка и не адаптер: это ЛИЦО ядра. Элемент не
// хранит ни позиции, ни размера, ни цвета — он хранит НОМЕР УЗЛА, а всё
// состояние живёт в документе. Из этого следует главное: объектный интерфейс и
// файл .uidoc — одно и то же дерево, а не две копии, которые однажды разойдутся.
//
// СЛЕДСТВИЯ, РАДИ КОТОРЫХ ТАК СДЕЛАНО:
//   • интерфейс, собранный кодом, сохраняется в файл без единой строчки кода;
//   • интерфейс, нарисованный в редакторе, доступен коду как объекты;
//   • раскладка, маски, эффекты и батчинг достаются даром — они уже есть;
//   • новый виджет — это класс-наследник, а не правка ядра (§15, §45 ТЗ).
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

class UIContext;

// ---------------------------------------------------------------------------
// UIElement — базовый объект интерфейса.
//
// Живёт ровно столько, сколько живёт его узел: удаление элемента удаляет узел
// вместе с поддеревом, удаление узла удаляет элемент. Владеет ими контекст —
// не вызывающий: интерфейс из полусотни элементов, за временем жизни которых
// следит человек, — это полсотни возможностей уронить игру.
// ---------------------------------------------------------------------------
class UIElement {
public:
    // Публичный, потому что базовый элемент — полноценный тип: прозрачный узел
    // с раскладкой и без оформления. Прятать его конструктор значит заставлять
    // заводить пустого наследника ради каждой группировки.
    UIElement() = default;
    virtual ~UIElement();

    UIElement(const UIElement&) = delete;
    UIElement& operator=(const UIElement&) = delete;

    // --- Жизненный цикл -----------------------------------------------------
    //
    // Update зовётся каждый кадр ДО раскладки — у элемента есть право поменять
    // содержимое (список дорисовал строки, поле ввода приняло символ), и это
    // должно попасть в ту же раскладку, а не в следующую.
    virtual void Update(float dt) { (void)dt; }
    // Узел создан и привязан к контексту: здесь наследник собирает своё
    // содержимое. Не в конструкторе — в нём узла ещё нет.
    virtual void OnAttach() {}
    // Имя типа для реестра, сериализации и инспектора.
    virtual const char* TypeName() const { return "Element"; }

    // --- Дерево -------------------------------------------------------------
    UIElement* Parent() const;
    std::vector<UIElement*> Children() const;
    // Взять чужой элемент себе. Возвращает его же — чтобы можно было писать
    // panel->Add(ui.Create<Button>("Играть")).
    UIElement* Add(UIElement* child, int index = -1);
    void Remove(UIElement* child);   // ребёнок УДАЛЯЕТСЯ вместе с поддеревом
    void RemoveFromParent();         // удаляет сам себя

    // --- Геометрия ----------------------------------------------------------
    //
    // Задаётся в ЛОГИЧЕСКИХ единицах документа; на экране она умножается на
    // масштаб холста. Bounds, наоборот, отдаёт экранные пиксели — то, что
    // реально нарисовано.
    UIElement* SetPosition(glm::vec2 position);
    UIElement* SetSize(glm::vec2 size);
    UIElement* SetWidth(float w);
    UIElement* SetHeight(float h);
    // Якоря долями родителя: {0,0} — левый верх, {1,1} — правый низ. Равные
    // min и max — точка, разные — растяжение вдоль стороны.
    UIElement* SetAnchor(glm::vec2 min, glm::vec2 max);
    UIElement* SetAnchor(UIAnchor preset);
    UIElement* SetPivot(glm::vec2 pivot);
    UIElement* SetMargin(const UIEdges& margin);
    UIElement* SetStretch(bool horizontal, bool vertical);
    // Размер по содержимому (§«auto size»): контейнер обнимает детей, надпись —
    // текст.
    UIElement* FitContent(bool width = true, bool height = true);

    glm::vec2 Position() const;
    glm::vec2 Size() const;
    // Прямоугольник в ЭКРАННЫХ пикселях по последней посчитанной раскладке.
    // Пока раскладки не было — нули: врать про «где элемент» нельзя, по этому
    // числу ставят курсор и всплывающие окна.
    UIRect Bounds() const;

    // --- Раскладка детей ----------------------------------------------------
    UILayout& Layout();                 // заводит компонент раскладки
    UIElement* Horizontal(float gap = 0.0f);
    UIElement* Vertical(float gap = 0.0f);
    UIElement* Grid(int columns, glm::vec2 gap = {0.0f, 0.0f});
    // Наложение: дети занимают одну и ту же область (§6 «Stack»).
    UIElement* Stack();
    UIElement* Padding(const UIEdges& padding);

    // --- Состояние ----------------------------------------------------------
    UIElement* SetVisible(bool visible);
    bool IsVisible() const;
    UIElement* SetEnabled(bool enabled);
    bool IsEnabled() const;
    UIElement* SetOpacity(float opacity);
    UIElement* SetName(std::string name);
    const std::string& Name() const;

    // --- Стиль --------------------------------------------------------------
    //
    // Именем, а не набором цветов: «сделать все кнопки другими» должно быть
    // правкой в одном месте, а не обходом дерева.
    UIElement* SetStyle(const std::string& style);

    // --- Обрезка содержимого (§9) -------------------------------------------
    UIElement* ClipChildren(bool clip, float radius = 0.0f);

    // --- События ------------------------------------------------------------
    //
    // Подписка живёт вместе с элементом: удалили элемент — обработчики сняты.
    // Иначе колбэк переживает свой объект, и это падение через кадр.
    int On(UIEventType type, UIEventHandler handler);
    void Off(int subscription);
    // Частые случаи — коротко. Без аргумента: чаще всего он не нужен, а
    // лямбда без параметра читается лучше.
    UIElement* OnClick(std::function<void()> fn);
    UIElement* OnClickEvent(UIEventHandler fn);
    UIElement* OnHover(std::function<void(bool)> fn);
    UIElement* OnValueChanged(std::function<void(float)> fn);

    // --- Доступ к ядру ------------------------------------------------------
    //
    // Открыт намеренно. Слой не обязан пересказывать все компоненты ядра
    // своими методами: любая попытка это сделать заканчивается тем, что слой
    // отстаёт от ядра, и половина возможностей недостижима.
    UINodeId NodeId() const { return m_id; }
    UINode* Node() const;
    UIContext& Ctx() const { return *m_ctx; }
    UIDocument& Doc() const;

    template <class T> T& Ensure() { return Node()->Ensure<T>(); }
    template <class T> T* Get() const { UINode* n = Node(); return n ? n->Get<T>() : nullptr; }
    template <class T> bool Has() const { return Get<T>() != nullptr; }

protected:
    UITransform& Transform();
    // Пометить документ грязным: слой обязан это делать за наследников, иначе
    // «поменял и не видно» становится обычным делом.
    void Dirty(uint32_t flags);

private:
    friend class UIContext;

    UIContext* m_ctx = nullptr;
    UINodeId m_id = kUIInvalidNode;
    std::vector<int> m_subs;
};

} // namespace sage::ui::sui
