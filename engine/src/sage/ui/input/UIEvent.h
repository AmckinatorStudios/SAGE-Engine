#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// СОБЫТИЯ ИНТЕРФЕЙСА (§49–51 ТЗ).
//
// ГЛАВНОЕ ПРАВИЛО: событие описывает ТО, ЧТО ПРОИЗОШЛО С ИНТЕРФЕЙСОМ, и ничего
// больше. «Нажали узел с командой menu.play» — это событие. «Начать игру» — уже
// не событие интерфейса, а решение игры, и принимается оно снаружи (§102, §103).
// Поэтому здесь нет ни одного имени из игровой предметной области, и появиться
// оно не может: интерфейс о ней не знает.
//
// РАСПРОСТРАНЕНИЕ (§51) — три фазы, как в любой зрелой системе интерфейса:
//   Capture — сверху вниз: предок вправе перехватить событие до потомка
//             (модальное окно, перетаскивание, блокировка панели);
//   Target  — сам узел;
//   Bubble  — снизу вверх: список узнаёт о нажатии на строку, панель — о
//             нажатии на любую кнопку внутри.
// Без фаз каждая такая задача решается «глобальным флагом», и два таких флага
// уже не уживаются.
// ---------------------------------------------------------------------------
namespace sage::ui {

enum class UIEventType {
    PointerEnter,
    PointerExit,
    PointerMove,
    PointerDown,
    PointerUp,
    Click,
    DoubleClick,
    Scroll,
    DragStart,
    Drag,
    DragEnd,
    Drop,
    Focus,
    Blur,
    KeyDown,
    KeyUp,
    TextInput,
    ValueChanged, // значение узла изменено пользователем (ползунок, галка, поле)
    Submit,       // Enter в поле ввода, «принять» в диалоге
    Cancel,
};

enum class UIEventPhase { Capture, Target, Bubble };

struct UIEvent {
    UIEventType Type = UIEventType::PointerMove;
    UIEventPhase Phase = UIEventPhase::Target;

    UINodeId Target = kUIInvalidNode;  // куда событие адресовано
    UINodeId Current = kUIInvalidNode; // на каком узле сейчас обрабатывается

    glm::vec2 Pointer{0.0f, 0.0f};     // экранные пиксели
    glm::vec2 LocalPointer{0.0f, 0.0f};// в координатах узла
    glm::vec2 Delta{0.0f, 0.0f};       // перемещение/прокрутка
    int Button = 0;                    // 0 — левая, 1 — правая, 2 — средняя
    int Clicks = 1;

    int Key = 0;
    uint32_t Codepoint = 0;
    bool Shift = false, Ctrl = false, Alt = false;

    float Value = 0.0f;                // для ValueChanged
    std::string Command;               // команда узла (§102), если задана

    // Событие обработано — дальше не идёт. Явный флаг, а не возврат из
    // обработчика: обработчиков у одного узла может быть несколько.
    bool Handled = false;
};

using UIEventHandler = std::function<void(UIEvent&)>;

// Слушатели документа. Хранятся ОТДЕЛЬНО от узлов, а не полем в узле, ровно по
// одной причине: узлы сериализуются, а функции — нет. Попытка положить
// обработчик в узел заканчивается либо тем, что узел нельзя сохранить, либо
// тем, что обработчик молча теряется при загрузке.
class UIEventBus {
public:
    // Подписка на конкретный узел. Возвращает номер подписки для отписки.
    int On(UINodeId node, UIEventType type, UIEventHandler handler);
    // Подписка на ВСЕ события документа — так игра узнаёт про команды, не
    // подписываясь на каждую кнопку.
    int OnAny(UIEventHandler handler);
    void Off(int subscription);
    void Clear();

    void Dispatch(UIEvent& e) const;

private:
    struct Sub {
        int Id = 0;
        UINodeId Node = kUIInvalidNode;
        UIEventType Type = UIEventType::Click;
        bool Any = false;
        UIEventHandler Handler;
    };
    std::vector<Sub> m_subs;
    int m_nextId = 1;
};

const char* const* UIEventTypeNames();
int UIEventTypeCount();

} // namespace sage::ui
