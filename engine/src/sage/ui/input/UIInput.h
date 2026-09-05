#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIContext.h"
#include "sage/ui/input/UIEvent.h"
#include "sage/ui/layout/UILayoutSolver.h"

// ---------------------------------------------------------------------------
// ВВОД (§49–56 ТЗ): состояние устройств → события интерфейса.
//
// ГРАНИЦА, КОТОРУЮ НЕЛЬЗЯ ПЕРЕХОДИТЬ (§118). Здесь мышь и клавиатура
// превращаются в события интерфейса — и на этом всё. Что означает нажатие
// кнопки, решает игра, снаружи, подписавшись на события. Ни одной строки вида
// «если нажали Play — начать игру» в этом модуле быть не может.
//
// ЧТО ЗДЕСЬ ЕСТЬ: наведение и уход, нажатие и отпускание, клик и двойной клик,
// захват указателя (перетаскивание не теряется, если курсор ушёл с кнопки),
// прокрутка, фокус, навигация клавишами и геймпадом, ввод текста.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIDocument;

// Состояние устройств за кадр — то, что даёт игра/редактор.
struct UIInputFrame {
    glm::vec2 Pointer{0.0f, 0.0f};   // экранные пиксели
    bool PointerInside = true;
    bool Buttons[3] = {false, false, false};
    glm::vec2 Scroll{0.0f, 0.0f};

    // Клавиши, нажатые и отпущенные в этом кадре (коды GLFW).
    std::vector<int> KeysDown;
    std::vector<int> KeysUp;
    std::string TextInput;           // введённые символы, UTF-8
    bool Shift = false, Ctrl = false, Alt = false;

    // Навигация (клавиатура/геймпад) — отдельно от клавиш: одна и та же
    // «вправо» приходит и со стрелки, и со стика, и интерфейс не должен знать,
    // с какого именно устройства.
    int NavX = 0, NavY = 0; // -1/0/1, срабатывает по факту нажатия
    bool NavSubmit = false;
    bool NavCancel = false;
    bool NavNext = false;   // Tab
    bool NavPrev = false;   // Shift+Tab
};

// Чем интерфейс «отчитывается» перед игрой: взял ли он ввод себе.
struct UIInputResult {
    bool PointerOverUI = false;
    bool PointerCaptured = false;   // идёт перетаскивание/нажатие в интерфейсе
    bool KeyboardCaptured = false;  // фокус в поле ввода
    UINodeId Hovered = kUIInvalidNode;
    UINodeId Focused = kUIInvalidNode;
    // Команды, сработавшие в этом кадре (§102). Именно СПИСОК: за кадр может
    // сработать и две.
    std::vector<std::string> Commands;
    std::string Cursor; // курсор, который просит интерфейс
};

class UIInputRouter {
public:
    // Один шаг. Зовётся РАНЬШЕ игровой логики кадра: по результату игра решает,
    // доставать ли ей тот же щелчок.
    UIInputResult Update(UIDocument& doc, const UILayoutSolver& layout,
                         const UIContext& ctx, const UIInputFrame& input,
                         UIEventBus& bus);

    // --- Фокус (§55) --------------------------------------------------------
    UINodeId Focused() const { return m_focused; }
    void SetFocus(UIDocument& doc, UINodeId id, UIEventBus* bus = nullptr);
    void ClearFocus(UIDocument& doc, UIEventBus* bus = nullptr);

    // --- Навигация (§56) ----------------------------------------------------
    //
    // Геометрическая: сосед ищется в нужную сторону среди принимающих фокус.
    // Явные NavUp/NavDown/... у узла перекрывают геометрию — иначе сложное меню
    // невозможно провести по задуманному маршруту.
    UINodeId FindNeighbour(UIDocument& doc, const UILayoutSolver& layout,
                           UINodeId from, int dx, int dy) const;
    UINodeId FindTabTarget(UIDocument& doc, const UILayoutSolver& layout,
                           UINodeId from, bool forward) const;

    void Reset();

private:
    void Emit(UIDocument& doc, UIEventBus& bus, UIEvent& e,
              const std::vector<UINodeId>& path) const;

    UINodeId m_hovered = kUIInvalidNode;
    UINodeId m_pressed = kUIInvalidNode;
    UINodeId m_focused = kUIInvalidNode;
    UINodeId m_capture = kUIInvalidNode;
    UINodeId m_dragging = kUIInvalidNode;
    glm::vec2 m_pressPoint{0.0f, 0.0f};
    double m_lastClickTime = 0.0;
    UINodeId m_lastClickNode = kUIInvalidNode;
    int m_clickCount = 0;
    bool m_prevButtons[3] = {false, false, false};
    std::vector<UINodeId> m_path;
};

} // namespace sage::ui
