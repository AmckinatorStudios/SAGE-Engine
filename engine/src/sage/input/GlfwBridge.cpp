#include "sage/input/GlfwBridge.h"

#include <GLFW/glfw3.h>

#include <cmath>
#include <unordered_map>

#include "sage/core/Window.h"

namespace sage::input {

namespace {

// Насколько должна сдвинуться ось, чтобы это считалось событием. Без порога
// шумящий стик шлёт событие каждый кадр по каждой оси — шесть событий на кадр
// из ниоткуда, и очередь ввода перестаёт быть читаемой в отладке. Порог здесь
// НЕ мёртвая зона: он гасит только повторы об одном и том же положении, а
// решение «считать ли это движением» принимает действие (см. ActionSettings).
constexpr float kAxisEpsilon = 0.0001f;

const std::unordered_map<int, Key>& GlfwKeyTable() {
    static const std::unordered_map<int, Key> table = {
        {GLFW_KEY_A, Key::A}, {GLFW_KEY_B, Key::B}, {GLFW_KEY_C, Key::C}, {GLFW_KEY_D, Key::D},
        {GLFW_KEY_E, Key::E}, {GLFW_KEY_F, Key::F}, {GLFW_KEY_G, Key::G}, {GLFW_KEY_H, Key::H},
        {GLFW_KEY_I, Key::I}, {GLFW_KEY_J, Key::J}, {GLFW_KEY_K, Key::K}, {GLFW_KEY_L, Key::L},
        {GLFW_KEY_M, Key::M}, {GLFW_KEY_N, Key::N}, {GLFW_KEY_O, Key::O}, {GLFW_KEY_P, Key::P},
        {GLFW_KEY_Q, Key::Q}, {GLFW_KEY_R, Key::R}, {GLFW_KEY_S, Key::S}, {GLFW_KEY_T, Key::T},
        {GLFW_KEY_U, Key::U}, {GLFW_KEY_V, Key::V}, {GLFW_KEY_W, Key::W}, {GLFW_KEY_X, Key::X_},
        {GLFW_KEY_Y, Key::Y}, {GLFW_KEY_Z, Key::Z},
        {GLFW_KEY_0, Key::Num0}, {GLFW_KEY_1, Key::Num1}, {GLFW_KEY_2, Key::Num2},
        {GLFW_KEY_3, Key::Num3}, {GLFW_KEY_4, Key::Num4}, {GLFW_KEY_5, Key::Num5},
        {GLFW_KEY_6, Key::Num6}, {GLFW_KEY_7, Key::Num7}, {GLFW_KEY_8, Key::Num8},
        {GLFW_KEY_9, Key::Num9},
        {GLFW_KEY_F1, Key::F1}, {GLFW_KEY_F2, Key::F2}, {GLFW_KEY_F3, Key::F3},
        {GLFW_KEY_F4, Key::F4}, {GLFW_KEY_F5, Key::F5}, {GLFW_KEY_F6, Key::F6},
        {GLFW_KEY_F7, Key::F7}, {GLFW_KEY_F8, Key::F8}, {GLFW_KEY_F9, Key::F9},
        {GLFW_KEY_F10, Key::F10}, {GLFW_KEY_F11, Key::F11}, {GLFW_KEY_F12, Key::F12},
        {GLFW_KEY_F13, Key::F13}, {GLFW_KEY_F14, Key::F14}, {GLFW_KEY_F15, Key::F15},
        {GLFW_KEY_F16, Key::F16},
        {GLFW_KEY_SPACE, Key::Space}, {GLFW_KEY_TAB, Key::Tab}, {GLFW_KEY_ENTER, Key::Enter},
        {GLFW_KEY_ESCAPE, Key::Escape}, {GLFW_KEY_BACKSPACE, Key::Backspace},
        {GLFW_KEY_INSERT, Key::Insert}, {GLFW_KEY_DELETE, Key::Delete},
        {GLFW_KEY_UP, Key::Up}, {GLFW_KEY_DOWN, Key::Down},
        {GLFW_KEY_LEFT, Key::Left}, {GLFW_KEY_RIGHT, Key::Right},
        {GLFW_KEY_PAGE_UP, Key::PageUp}, {GLFW_KEY_PAGE_DOWN, Key::PageDown},
        {GLFW_KEY_HOME, Key::Home}, {GLFW_KEY_END, Key::End},
        {GLFW_KEY_CAPS_LOCK, Key::CapsLock}, {GLFW_KEY_SCROLL_LOCK, Key::ScrollLock},
        {GLFW_KEY_NUM_LOCK, Key::NumLock}, {GLFW_KEY_PRINT_SCREEN, Key::PrintScreen},
        {GLFW_KEY_PAUSE, Key::Pause}, {GLFW_KEY_MENU, Key::Menu},
        {GLFW_KEY_LEFT_SHIFT, Key::LeftShift}, {GLFW_KEY_RIGHT_SHIFT, Key::RightShift},
        {GLFW_KEY_LEFT_CONTROL, Key::LeftControl}, {GLFW_KEY_RIGHT_CONTROL, Key::RightControl},
        {GLFW_KEY_LEFT_ALT, Key::LeftAlt}, {GLFW_KEY_RIGHT_ALT, Key::RightAlt},
        {GLFW_KEY_LEFT_SUPER, Key::LeftSuper}, {GLFW_KEY_RIGHT_SUPER, Key::RightSuper},
        {GLFW_KEY_MINUS, Key::Minus}, {GLFW_KEY_EQUAL, Key::Equal},
        {GLFW_KEY_LEFT_BRACKET, Key::LeftBracket}, {GLFW_KEY_RIGHT_BRACKET, Key::RightBracket},
        {GLFW_KEY_BACKSLASH, Key::Backslash}, {GLFW_KEY_SEMICOLON, Key::Semicolon},
        {GLFW_KEY_APOSTROPHE, Key::Apostrophe}, {GLFW_KEY_GRAVE_ACCENT, Key::GraveAccent},
        {GLFW_KEY_COMMA, Key::Comma}, {GLFW_KEY_PERIOD, Key::Period}, {GLFW_KEY_SLASH, Key::Slash},
        {GLFW_KEY_WORLD_1, Key::World1}, {GLFW_KEY_WORLD_2, Key::World2},
        {GLFW_KEY_KP_0, Key::Kp0}, {GLFW_KEY_KP_1, Key::Kp1}, {GLFW_KEY_KP_2, Key::Kp2},
        {GLFW_KEY_KP_3, Key::Kp3}, {GLFW_KEY_KP_4, Key::Kp4}, {GLFW_KEY_KP_5, Key::Kp5},
        {GLFW_KEY_KP_6, Key::Kp6}, {GLFW_KEY_KP_7, Key::Kp7}, {GLFW_KEY_KP_8, Key::Kp8},
        {GLFW_KEY_KP_9, Key::Kp9}, {GLFW_KEY_KP_DECIMAL, Key::KpDecimal},
        {GLFW_KEY_KP_DIVIDE, Key::KpDivide}, {GLFW_KEY_KP_MULTIPLY, Key::KpMultiply},
        {GLFW_KEY_KP_SUBTRACT, Key::KpSubtract}, {GLFW_KEY_KP_ADD, Key::KpAdd},
        {GLFW_KEY_KP_ENTER, Key::KpEnter}, {GLFW_KEY_KP_EQUAL, Key::KpEqual},
    };
    return table;
}

} // namespace

Key GlfwBridge::FromGlfwKey(int glfwKey) {
    const auto& table = GlfwKeyTable();
    auto it = table.find(glfwKey);
    return it == table.end() ? Key::Unknown : it->second;
}

MouseButton GlfwBridge::FromGlfwMouseButton(int glfwButton, bool* ok) {
    if (ok) *ok = true;
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT:   return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:  return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
        case GLFW_MOUSE_BUTTON_4:      return MouseButton::Extra1;
        case GLFW_MOUSE_BUTTON_5:      return MouseButton::Extra2;
        default: break;
    }
    if (ok) *ok = false;
    return MouseButton::Left;
}

uint8_t GlfwBridge::FromGlfwMods(int glfwMods) {
    uint8_t mods = ModNone;
    if (glfwMods & GLFW_MOD_SHIFT) mods |= ModShift;
    if (glfwMods & GLFW_MOD_CONTROL) mods |= ModCtrl;
    if (glfwMods & GLFW_MOD_ALT) mods |= ModAlt;
    if (glfwMods & GLFW_MOD_SUPER) mods |= ModSuper;
    return mods;
}

void GlfwBridge::Attach(Window& window, InputSystem& input) {
    m_window = &window;
    m_input = &input;
    input.SetCursorControl(this);

    window.AddKeyCallback([this](int key, int action, int mods) {
        const Key translated = FromGlfwKey(key);
        if (translated == Key::Unknown) return; // клавиши, которых нет в словаре движка
        const uint8_t m = FromGlfwMods(mods);
        if (action == GLFW_PRESS) m_input->Push(InputEvent::KeyDown(translated, m));
        else if (action == GLFW_RELEASE) m_input->Push(InputEvent::KeyUp(translated, m));
        else if (action == GLFW_REPEAT) m_input->Push(InputEvent::KeyAgain(translated, m));
    });

    window.SetCharCallback([this](unsigned int codepoint) {
        m_input->Push(InputEvent::Text(codepoint));
    });

    window.AddMouseButtonCallback([this](int button, int action, int mods) {
        bool ok = false;
        const MouseButton translated = FromGlfwMouseButton(button, &ok);
        if (!ok) return;
        const uint8_t m = FromGlfwMods(mods);
        const glm::vec2 pos = m_lastCursor;
        if (action == GLFW_PRESS) m_input->Push(InputEvent::MouseDown(translated, pos, m));
        else if (action == GLFW_RELEASE) m_input->Push(InputEvent::MouseUp(translated, pos, m));
    });

    window.SetCursorPosCallback([this](double x, double y) {
        const glm::vec2 pos((float)x, (float)y);
        // Первое событие задаёт только точку отсчёта: иначе смещением
        // окажется расстояние от нуля до курсора, и обзор рванёт в сторону в
        // первый же кадр после захвата мыши.
        glm::vec2 delta(0.0f);
        if (!m_firstCursorEvent) {
            delta.x = pos.x - m_lastCursor.x;
            delta.y = m_lastCursor.y - pos.y; // Y вверх: конвенция движка
        }
        m_firstCursorEvent = false;
        m_lastCursor = pos;
        m_input->Push(InputEvent::MouseMove(pos, delta));
    });

    window.SetScrollCallback([this](double, double yoffset) {
        m_input->Push(InputEvent::Wheeled((float)yoffset));
    });

    window.SetCursorEnterCallback([this](bool entered) {
        InputEvent e;
        e.Type = entered ? InputEventType::MouseEnter : InputEventType::MouseLeave;
        e.Position = m_lastCursor;
        m_input->Push(e);
        // Курсор вернулся в окно с другого места — отсчёт смещения надо
        // начинать заново, иначе первый же кадр даст скачок обзора на всю
        // ширину экрана.
        if (entered) m_firstCursorEvent = true;
    });
}

void GlfwBridge::PollGamepads() {
    if (!m_input) return;
    for (int slot = 0; slot < kMaxGamepads; ++slot) EmitPadEvents(slot);
}

void GlfwBridge::EmitPadEvents(int slot) {
    PadSnapshot& prev = m_pads[(size_t)slot];

    GLFWgamepadstate state{};
    // Именно «геймпад», а не «джойстик»: GLFW сам приводит известные ему
    // устройства к стандартной раскладке (A/B/X/Y, два стика, два курка). Без
    // этого пришлось бы держать таблицу «какая кнопка какая» на каждую модель.
    const bool connected = glfwJoystickPresent(GLFW_JOYSTICK_1 + slot) &&
                           glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + slot) &&
                           glfwGetGamepadState(GLFW_JOYSTICK_1 + slot, &state);

    if (connected != prev.Connected) {
        m_input->Push(InputEvent::PadConnected(slot, connected));
        prev.Connected = connected;
        // Имя устройства — для экрана настроек: «PAD_A» надо подписать так, как
        // эта кнопка называется на том геймпаде, который человек держит.
        if (connected) {
            const char* name = glfwGetGamepadName(GLFW_JOYSTICK_1 + slot);
            m_input->MutableState().PadMutable(slot).SetName(name ? name : "");
        }
        if (!connected) {
            prev.Buttons.fill(false);
            prev.Axes.fill(0.0f);
            return;
        }
    }
    if (!connected) return;

    for (size_t i = 0; i < prev.Buttons.size(); ++i) {
        const bool down = state.buttons[i] == GLFW_PRESS;
        if (down == prev.Buttons[i]) continue;
        prev.Buttons[i] = down;
        m_input->Push(down ? InputEvent::PadDown((GamepadButton)i, slot)
                           : InputEvent::PadUp((GamepadButton)i, slot));
    }

    for (size_t i = 0; i < prev.Axes.size(); ++i) {
        float value = state.axes[i];
        const GamepadAxis axis = (GamepadAxis)i;
        if (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger) {
            // Курки GLFW отдаёт как ось −1..1, где −1 — отпущен. Игре нужен
            // «насколько нажат», то есть 0..1: сравнивать курок с нулём при
            // сыром диапазоне значит считать отпущенный курок нажатым
            // наполовину.
            value = (value + 1.0f) * 0.5f;
        } else if (axis == GamepadAxis::LeftY || axis == GamepadAxis::RightY) {
            // Y стика у GLFW направлен ВНИЗ. Переворачиваем здесь, на границе
            // платформы, чтобы «вперёд» во всём движке значило +Y — как у
            // клавиш W/S и у смещения мыши.
            value = -value;
        }
        if (std::fabs(value - prev.Axes[i]) < kAxisEpsilon) continue;
        prev.Axes[i] = value;
        m_input->Push(InputEvent::PadAxisMoved(axis, value, slot));
    }
}

void GlfwBridge::SetCursorCaptured(bool captured) {
    if (!m_window) return;
    m_window->SetCursorCaptured(captured);
    // После захвата/отпускания курсор прыгает (система прячет его в центр),
    // и первое же событие движения дало бы огромное смещение — то самое
    // «камера дёрнулась при входе в режим обзора».
    m_firstCursorEvent = true;
}

bool GlfwBridge::CursorCaptured() const { return m_window && m_window->CursorCaptured(); }

} // namespace sage::input
