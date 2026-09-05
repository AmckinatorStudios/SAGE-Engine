#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// ВИДЖЕТЫ (§99, §100 ТЗ) — надстройка над ядром, а не его часть.
//
// ГРАНИЦА. Ядро (узел, дерево, раскладка, оформление, маски, эффекты,
// рисование, ввод) НЕ ЗНАЕТ о существовании кнопок и ползунков. Виджет — это
// сборка узлов ядра плюс, если нужно, маленький компонент поведения. Убери
// отсюда всё — ядро продолжит работать; убери ядро — не останется ничего.
//
// НИКАКОЙ ИГРОВОЙ ЛОГИКИ (§103). Кнопка сообщает «меня нажали» и меняет свой
// вид. Что произойдёт дальше — не её дело и вообще не дело интерфейса.
// Ползунок меняет своё число; что это за число — громкость, сложность или
// доля здоровья — интерфейс не знает.
// ---------------------------------------------------------------------------
namespace sage::ui {

// --- Компоненты поведения ---------------------------------------------------

// Числовое значение 0..1 с направлением роста: шкала, полоса загрузки,
// индикатор. Отдельно от ползунка: показывать значение и позволять его менять —
// разные вещи, и смешивать их в одном компоненте значит дать «перетаскиваемую
// полосу здоровья».
struct UIProgress : UIComponentOf<UIProgress> {
    static const UIComponentType& StaticType();

    enum class Direction { LeftToRight, RightToLeft, BottomToTop, TopToBottom, Radial };

    float Value = 1.0f;
    float Min = 0.0f, Max = 1.0f;
    Direction Grow = Direction::LeftToRight;
    UIColor FillColor{0.36f, 0.75f, 0.42f, 1.0f};
    UIColor TrackColor{0.14f, 0.15f, 0.20f, 1.0f};
    UICorners Radius{4.0f};
    UIEdges Padding{0.0f, 0.0f, 0.0f, 0.0f};
    // Плавное движение к цели, единиц в секунду (0 — мгновенно). Полоса,
    // прыгающая рывком, читается хуже, чем едущая за четверть секунды.
    float Smoothing = 0.0f;
    float Displayed = -1.0f; // рантайм

    float Normalized() const;
};

// Изменяемое число: ползунок, переключатель, шаговый выбор.
struct UIRangeValue : UIComponentOf<UIRangeValue> {
    static const UIComponentType& StaticType();

    float Min = 0.0f, Max = 1.0f, Value = 0.5f;
    float Step = 0.0f;   // 0 — плавно
    bool Vertical = false;
    bool Toggle = false; // галка — тот же диапазон 0..1 с шагом 1
    UIColor TrackColor{0.16f, 0.18f, 0.24f, 1.0f};
    UIColor AccentColor{0.95f, 0.76f, 0.20f, 1.0f};
    float HandleSize = 18.0f;
    UICorners Radius{4.0f};

    float Normalized() const;
    void SetNormalized(float t);
};

// Поле ввода текста.
struct UITextField : UIComponentOf<UITextField> {
    static const UIComponentType& StaticType();

    std::string Value;
    std::string PlaceholderKey;
    int MaxLength = 0;
    bool Password = false;
    bool ReadOnly = false;
    bool Multiline = false;
    bool SelectAllOnFocus = true;

    // Рантайм: каретка и выделение в БАЙТАХ строки.
    int Caret = 0;
    int SelectionAnchor = 0;
    float ScrollX = 0.0f;
    float Blink = 0.0f;
};

// Прокручиваемая область (§90). Знает только про размер содержимого, окно и
// смещение — и НИЧЕГО про то, что внутри (§90: ни инвентаря, ни чата, ни
// списка заданий).
struct UIScrollView : UIComponentOf<UIScrollView> {
    static const UIComponentType& StaticType();

    glm::vec2 Offset{0.0f, 0.0f};
    glm::vec2 ContentSize{0.0f, 0.0f}; // рантайм: считает раскладка
    bool Horizontal = false;
    bool Vertical = true;
    float Speed = 40.0f;
    // Инерция и «резинка» — то, без чего прокрутка ощущается сломанной.
    float Damping = 12.0f;
    bool Elastic = true;
    glm::vec2 Velocity{0.0f, 0.0f}; // рантайм
    // Основа виртуализации (§91): сколько элементов видно и с какого начинать.
    // Сам виртуальный список — задача поверх, но окно ему считает прокрутка.
    int FirstVisible = 0;
    int VisibleCount = 0;
};

// Переключаемая группа: вкладки, радио-кнопки, панель выбора. Хранит только
// НОМЕР выбранного — что именно выбрано, знает тот, кто заполнил список.
struct UISelection : UIComponentOf<UISelection> {
    static const UIComponentType& StaticType();
    int Index = 0;
    bool AllowNone = false;
    std::string Group;
};

// --- Сборка виджетов --------------------------------------------------------
//
// Функции, а не классы: виджет — это не тип данных, а РЕЦЕПТ. Готовое поддерево
// потом правится как любое другое, потому что оно и есть обычное поддерево.
UINodeId UIMakePanel(UIDocument& doc, UINodeId parent, const std::string& name = "Panel");
UINodeId UIMakeLabel(UIDocument& doc, UINodeId parent, const std::string& text);
UINodeId UIMakeImage(UIDocument& doc, UINodeId parent, const std::string& path);
UINodeId UIMakeButton(UIDocument& doc, UINodeId parent, const std::string& text,
                      const std::string& command = {});
UINodeId UIMakeCheckbox(UIDocument& doc, UINodeId parent, const std::string& text);
UINodeId UIMakeSlider(UIDocument& doc, UINodeId parent);
UINodeId UIMakeProgress(UIDocument& doc, UINodeId parent);
UINodeId UIMakeInputField(UIDocument& doc, UINodeId parent, const std::string& placeholder);
UINodeId UIMakeScrollView(UIDocument& doc, UINodeId parent);
UINodeId UIMakeList(UIDocument& doc, UINodeId parent);
UINodeId UIMakeTabs(UIDocument& doc, UINodeId parent, const std::vector<std::string>& titles);
UINodeId UIMakeDropdown(UIDocument& doc, UINodeId parent,
                        const std::vector<std::string>& options);
UINodeId UIMakeTooltip(UIDocument& doc, UINodeId parent, const std::string& text);

// Шаг поведения виджетов за кадр: плавность полос, каретка поля ввода,
// инерция прокрутки. Отдельно от ввода и от рисования — это ни то, ни другое.
void UIUpdateWidgets(UIDocument& doc, float dt);

void RegisterBuiltinUIWidgetComponents();

} // namespace sage::ui
