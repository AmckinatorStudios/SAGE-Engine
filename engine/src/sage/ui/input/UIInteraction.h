#pragma once
#include <string>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// ВЗАИМОДЕЙСТВИЕ (§49, §52–57 ТЗ) — компонент «узел отвечает на ввод».
//
// Узел без этого компонента не ловит мышь вовсе и не стоит системе ввода
// ничего: ни проверки попадания, ни состояния, ни памяти (§130). Узел с ним
// получает наведение, нажатие, фокус, навигацию и собственную форму попадания.
//
// СОСТОЯНИЕ ОТДЕЛЕНО ОТ НАСТРОЙКИ. Наведение, нажатие и фокус — это рантайм: их
// нельзя сохранять в файл (иначе документ помнит, что кнопка была под курсором)
// и нельзя путать с авторскими данными. Поэтому они собраны в UIInteractionState
// и в таблице свойств не значатся вовсе.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Визуальные состояния (§57). Именно ВИЗУАЛЬНЫЕ: «выбран» здесь — это подсветка
// строки списка, а не «выбранное оружие игрока».
enum UIStateFlags : uint32_t {
    UIState_Normal   = 0,
    UIState_Hovered  = 1u << 0,
    UIState_Pressed  = 1u << 1,
    UIState_Focused  = 1u << 2,
    UIState_Disabled = 1u << 3,
    UIState_Selected = 1u << 4,
    UIState_Checked  = 1u << 5,
};

const char* const* UIStateNames();
int UIStateCount();

// Форма области попадания (§53).
enum class UIHitShape {
    Rect,
    RoundedRect, // берёт радиусы у заливки/формы узла
    Ellipse,
    ImageAlpha,  // по альфе картинки узла
    Polygon,     // по точкам формы
    None,        // не ловит совсем (но дети — ловят)
};

struct UIInteractionState {
    uint32_t Flags = UIState_Normal;
    // Произошло в ЭТОМ кадре. Живёт один кадр: система ставит, игра читает,
    // следующий шаг гасит.
    bool Clicked = false;
    bool Changed = false;
    float HoverTime = 0.0f;  // сколько курсор над узлом — для подсказок
    float PressTime = 0.0f;
    glm::vec2 PressPoint{0.0f, 0.0f};
    bool Dragging = false;
};

struct UIInteraction : UIComponentOf<UIInteraction> {
    static const UIComponentType& StaticType();

    bool Enabled = true;
    bool BlockRaycast = true;  // false — клики проходят СКВОЗЬ узел
    bool Focusable = false;
    bool Draggable = false;
    bool ScrollTarget = false;
    // Порог прозрачности, ниже которого узел не ловит мышь (§52). Иначе
    // невидимая (alpha 0) панель продолжает перехватывать клики — классическая
    // причина «кнопка не нажимается, а почему — непонятно».
    float AlphaThreshold = 0.01f;

    UIHitShape Hit = UIHitShape::Rect;
    UIEdges HitPadding{0.0f, 0.0f, 0.0f, 0.0f}; // расширить область попадания

    // КОМАНДА (§102) — строка, которую интерфейс сообщает наружу при нажатии.
    // Ядро интерфейса не знает и не должно знать, что она значит.
    std::string Command;

    // Навигация клавиатурой/геймпадом (§55, §56). Пусто — сосед вычисляется
    // геометрически; имя узла — переход к нему.
    std::string NavUp, NavDown, NavLeft, NavRight;
    int TabIndex = 0;
    std::string FocusGroup;

    // Курсор мыши над узлом (имя из набора движка; пусто — не менять).
    std::string Cursor;
    // Ключ подсказки (§60). Ключ, а не текст: подсказки переводятся.
    std::string TooltipKey;

    UIInteractionState Runtime;

    bool Is(uint32_t flag) const { return (Runtime.Flags & flag) != 0; }
};

const char* const* UIHitShapeNames();
int UIHitShapeCount();

} // namespace sage::ui
