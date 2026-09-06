#include "sage/input/Devices.h"

namespace sage::input {

// --- Клавиатура -------------------------------------------------------------

void Keyboard::BeginFrame() {
    for (ButtonState& b : m_keys) b.BeginFrame();
    m_text.clear();
    // Маска из событий живёт ровно кадр: она описывает события ЭТОГО кадра, а
    // не состояние клавиатуры — состояние спрашивается у самих клавиш.
    m_modifiers = ModNone;
}

uint8_t Keyboard::Modifiers() const {
    uint8_t mods = m_modifiers;
    if (Down(Key::LeftShift) || Down(Key::RightShift)) mods |= ModShift;
    if (Down(Key::LeftControl) || Down(Key::RightControl)) mods |= ModCtrl;
    if (Down(Key::LeftAlt) || Down(Key::RightAlt)) mods |= ModAlt;
    if (Down(Key::LeftSuper) || Down(Key::RightSuper)) mods |= ModSuper;
    return mods;
}

void Keyboard::Handle(const InputEvent& e, uint64_t frame) {
    switch (e.Type) {
        case InputEventType::KeyPressed: {
            const size_t i = (size_t)e.Keyboard;
            if (i < m_keys.size()) m_keys[i].Press(frame);
            m_modifiers |= e.Modifiers;
            break;
        }
        case InputEventType::KeyReleased: {
            const size_t i = (size_t)e.Keyboard;
            if (i < m_keys.size()) m_keys[i].Release(frame);
            m_modifiers |= e.Modifiers;
            break;
        }
        case InputEventType::KeyRepeat:
            // Автоповтор системы — НЕ новое нажатие: он нужен полю ввода
            // («держу Backspace — стирается»), но действию «прыжок» повтор
            // клавиши означал бы прыжок каждый кадр удержания.
            m_modifiers |= e.Modifiers;
            break;
        case InputEventType::TextInput:
            m_text.push_back(e.Codepoint);
            break;
        default:
            break;
    }
}

void Keyboard::ReleaseAll() {
    for (ButtonState& b : m_keys) b.ForceUp();
    m_text.clear();
    m_modifiers = ModNone;
}

// --- Мышь -------------------------------------------------------------------

void Mouse::BeginFrame() {
    for (ButtonState& b : m_buttons) b.BeginFrame();
    // Дельта и колесо живут ровно кадр: это «сколько сдвинули», а не «где
    // находится». Не обнулив их, игра получила бы вечно вращающийся обзор на
    // кадре, в котором мышь не двигали.
    m_delta = glm::vec2(0.0f);
    m_wheel = 0.0f;
}

void Mouse::Handle(const InputEvent& e, uint64_t frame) {
    switch (e.Type) {
        case InputEventType::MouseButtonPressed: {
            const size_t i = (size_t)e.Button;
            if (i < m_buttons.size()) m_buttons[i].Press(frame);
            m_position = e.Position;
            break;
        }
        case InputEventType::MouseButtonReleased: {
            const size_t i = (size_t)e.Button;
            if (i < m_buttons.size()) m_buttons[i].Release(frame);
            m_position = e.Position;
            break;
        }
        case InputEventType::MouseMoved:
            m_position = e.Position;
            m_delta += e.Delta; // за кадр событий движения приходит много
            break;
        case InputEventType::MouseWheel:
            m_wheel += e.Wheel;
            break;
        case InputEventType::MouseEnter:
            m_inside = true;
            break;
        case InputEventType::MouseLeave:
            m_inside = false;
            break;
        default:
            break;
    }
}

void Mouse::ReleaseAll() {
    for (ButtonState& b : m_buttons) b.ForceUp();
    m_delta = glm::vec2(0.0f);
    m_wheel = 0.0f;
}

// --- Геймпад ----------------------------------------------------------------

void Gamepad::BeginFrame() {
    for (ButtonState& b : m_buttons) b.BeginFrame();
}

void Gamepad::Handle(const InputEvent& e, uint64_t frame) {
    switch (e.Type) {
        case InputEventType::GamepadButtonPressed: {
            const size_t i = (size_t)e.PadButton;
            if (i < m_buttons.size()) m_buttons[i].Press(frame);
            break;
        }
        case InputEventType::GamepadButtonReleased: {
            const size_t i = (size_t)e.PadButton;
            if (i < m_buttons.size()) m_buttons[i].Release(frame);
            break;
        }
        case InputEventType::GamepadAxisChanged: {
            const size_t i = (size_t)e.PadAxis;
            if (i < m_axes.size()) m_axes[i] = e.Value;
            break;
        }
        case InputEventType::GamepadConnected:
            m_connected = true;
            break;
        case InputEventType::GamepadDisconnected:
            // Выдернули провод — всё отпущено. Иначе игрок, вынувший джойстик
            // с зажатым «вперёд», навсегда останется идти вперёд.
            m_connected = false;
            ReleaseAll();
            break;
        default:
            break;
    }
}

void Gamepad::ReleaseAll() {
    for (ButtonState& b : m_buttons) b.ForceUp();
    m_axes.fill(0.0f);
}

// --- Все устройства ---------------------------------------------------------

void Devices::BeginFrame() {
    ++m_frame;
    m_keyboard.BeginFrame();
    m_mouse.BeginFrame();
    for (Gamepad& p : m_gamepads) p.BeginFrame();
}

void Devices::Handle(const InputEvent& e) {
    if (e.IsKeyboard()) {
        m_keyboard.Handle(e, m_frame);
    } else if (e.IsMouse()) {
        m_mouse.Handle(e, m_frame);
    } else if (e.IsGamepad()) {
        if (e.Gamepad >= 0 && e.Gamepad < kMaxGamepads)
            m_gamepads[(size_t)e.Gamepad].Handle(e, m_frame);
    }
}

void Devices::ReleaseAll() {
    m_keyboard.ReleaseAll();
    m_mouse.ReleaseAll();
    for (Gamepad& p : m_gamepads) p.ReleaseAll();
}

int Devices::FirstConnected() const {
    for (int i = 0; i < kMaxGamepads; ++i)
        if (m_gamepads[(size_t)i].Connected()) return i;
    return -1;
}

} // namespace sage::input
