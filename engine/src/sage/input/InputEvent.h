#pragma once
#include <glm/glm.hpp>
#include <cstdint>

#include "sage/input/Keys.h"

// ---------------------------------------------------------------------------
// СОБЫТИЕ ВВОДА — «что именно сделал человек», один факт за раз.
//
// Событие, а не только опрос состояния. Опрос отвечает на вопрос «нажато ли
// сейчас», и этого достаточно ходьбе, но не достаточно ничему короткому:
// нажатие, начавшееся и кончившееся между двумя кадрами, при опросе не
// существует вовсе. На шестидесяти кадрах это редкость, но проседания бывают у
// всех, и теряются ровно те нажатия, которые делают коротко — прыжок, выстрел,
// инвентарь. Со стороны человека это «игра иногда не реагирует».
//
// ОДНА СТРУКТУРА на все виды событий, а не иерархия классов. Причина
// практическая: события ввода складываются в очередь, копируются и проходят
// через несколько слоёв (контексты, интерфейс, игра). Полиморфный тип означал
// бы аллокацию на каждое движение мыши — а их за секунду сотни. Поля, не
// относящиеся к виду события, просто не заполнены; какие относятся, написано
// у каждого вида.
//
// ПОТРЕБЛЕНИЕ (Consumed) — не украшение, а обязательный механизм: щелчок по
// кнопке интерфейса не должен ОДНОВРЕМЕННО стрелять в игре. Тот, кто событие
// обработал, помечает его потреблённым, и слои с меньшим приоритетом его уже
// не видят (см. Context::Priority).
// ---------------------------------------------------------------------------
namespace sage::input {

enum class InputEventType : uint8_t {
    None = 0,
    KeyPressed,             // Key, Modifiers
    KeyReleased,            // Key, Modifiers
    KeyRepeat,              // Key, Modifiers — автоповтор при удержании
    TextInput,              // Codepoint (уже готовый символ, с учётом раскладки)
    MouseButtonPressed,     // Button, Modifiers, Position
    MouseButtonReleased,    // Button, Modifiers, Position
    MouseMoved,             // Position, Delta
    MouseWheel,             // Wheel (>0 — от себя/вверх)
    MouseEnter,             // курсор вошёл в окно
    MouseLeave,             // курсор вышел из окна
    GamepadButtonPressed,   // Gamepad, PadButton
    GamepadButtonReleased,  // Gamepad, PadButton
    GamepadAxisChanged,     // Gamepad, PadAxis, Value
    GamepadConnected,       // Gamepad
    GamepadDisconnected,    // Gamepad
};

struct InputEvent {
    InputEventType Type = InputEventType::None;

    // --- клавиатура ---
    Key Keyboard = Key::Unknown;
    uint8_t Modifiers = ModNone;
    unsigned int Codepoint = 0;   // TextInput: символ Unicode, а не код клавиши

    // --- мышь ---
    MouseButton Button = MouseButton::Left;
    // Положение курсора в пикселях от ЛЕВОГО ВЕРХНЕГО угла окна — так его
    // отдаёт оконная система и так его ждёт интерфейс.
    glm::vec2 Position{0.0f};
    // Смещение за событие. Y направлен ВВЕРХ (конвенция движка: мир, а не
    // экран) — обзор от первого лица складывает дельты напрямую, без знака
    // «минус» в каждой игре.
    glm::vec2 Delta{0.0f};
    float Wheel = 0.0f;

    // --- геймпад ---
    int Gamepad = 0;              // номер слота, 0..kMaxGamepads-1
    GamepadButton PadButton = GamepadButton::A;
    GamepadAxis PadAxis = GamepadAxis::LeftX;
    float Value = 0.0f;           // положение оси, -1..1 (курки: 0..1)

    // Событие уже обработано вышестоящим слоем — ниже не идёт (см. §29 ТЗ).
    bool Consumed = false;

    // --- Короткие конструкторы. Заполнять поля вручную на каждое нажатие —
    // верный способ однажды забыть Type и получить событие, которое молчит. ---
    static InputEvent KeyDown(Key key, uint8_t mods = ModNone) {
        InputEvent e; e.Type = InputEventType::KeyPressed; e.Keyboard = key; e.Modifiers = mods; return e;
    }
    static InputEvent KeyUp(Key key, uint8_t mods = ModNone) {
        InputEvent e; e.Type = InputEventType::KeyReleased; e.Keyboard = key; e.Modifiers = mods; return e;
    }
    static InputEvent KeyAgain(Key key, uint8_t mods = ModNone) {
        InputEvent e; e.Type = InputEventType::KeyRepeat; e.Keyboard = key; e.Modifiers = mods; return e;
    }
    static InputEvent Text(unsigned int codepoint) {
        InputEvent e; e.Type = InputEventType::TextInput; e.Codepoint = codepoint; return e;
    }
    static InputEvent MouseDown(MouseButton button, glm::vec2 pos = {}, uint8_t mods = ModNone) {
        InputEvent e; e.Type = InputEventType::MouseButtonPressed; e.Button = button; e.Position = pos; e.Modifiers = mods; return e;
    }
    static InputEvent MouseUp(MouseButton button, glm::vec2 pos = {}, uint8_t mods = ModNone) {
        InputEvent e; e.Type = InputEventType::MouseButtonReleased; e.Button = button; e.Position = pos; e.Modifiers = mods; return e;
    }
    static InputEvent MouseMove(glm::vec2 pos, glm::vec2 delta) {
        InputEvent e; e.Type = InputEventType::MouseMoved; e.Position = pos; e.Delta = delta; return e;
    }
    static InputEvent Wheeled(float wheel) {
        InputEvent e; e.Type = InputEventType::MouseWheel; e.Wheel = wheel; return e;
    }
    static InputEvent PadDown(GamepadButton button, int pad = 0) {
        InputEvent e; e.Type = InputEventType::GamepadButtonPressed; e.PadButton = button; e.Gamepad = pad; return e;
    }
    static InputEvent PadUp(GamepadButton button, int pad = 0) {
        InputEvent e; e.Type = InputEventType::GamepadButtonReleased; e.PadButton = button; e.Gamepad = pad; return e;
    }
    static InputEvent PadAxisMoved(GamepadAxis axis, float value, int pad = 0) {
        InputEvent e; e.Type = InputEventType::GamepadAxisChanged; e.PadAxis = axis; e.Value = value; e.Gamepad = pad; return e;
    }
    static InputEvent PadConnected(int pad, bool connected) {
        InputEvent e;
        e.Type = connected ? InputEventType::GamepadConnected : InputEventType::GamepadDisconnected;
        e.Gamepad = pad;
        return e;
    }

    // Какого устройства касается событие. Нужно фильтрам «интерфейс забрал
    // мышь, но не клавиатуру» — самому частому случаю в редакторе.
    bool IsKeyboard() const {
        return Type == InputEventType::KeyPressed || Type == InputEventType::KeyReleased ||
               Type == InputEventType::KeyRepeat || Type == InputEventType::TextInput;
    }
    bool IsMouse() const {
        return Type == InputEventType::MouseButtonPressed || Type == InputEventType::MouseButtonReleased ||
               Type == InputEventType::MouseMoved || Type == InputEventType::MouseWheel ||
               Type == InputEventType::MouseEnter || Type == InputEventType::MouseLeave;
    }
    bool IsGamepad() const {
        return Type == InputEventType::GamepadButtonPressed || Type == InputEventType::GamepadButtonReleased ||
               Type == InputEventType::GamepadAxisChanged || Type == InputEventType::GamepadConnected ||
               Type == InputEventType::GamepadDisconnected;
    }
};

} // namespace sage::input
