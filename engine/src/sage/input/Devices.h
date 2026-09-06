#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/input/InputEvent.h"
#include "sage/input/Keys.h"

// ---------------------------------------------------------------------------
// СОСТОЯНИЕ УСТРОЙСТВ — «что нажато прямо сейчас», сведённое в одном месте.
//
// Слой между сырыми событиями окна и именованными действиями. Событие говорит
// «клавишу W нажали», состояние отвечает «W удерживается» — и то, и другое
// нужно, потому что ходьбе нужен ответ на второй вопрос, а прыжку на первый.
//
// Состояние ОБНОВЛЯЕТСЯ СОБЫТИЯМИ, а не опросом окна. Раньше движок опрашивал
// GLFW раз в кадр и терял нажатия короче кадра; лечилось это защёлкой, которая
// подменяла опрос на полсобытия. Теперь источник один: события приходят в
// Handle(), а BeginFrame() открывает новый кадр. Нажатие, случившееся и
// закончившееся внутри одного кадра, живёт РОВНО ОДИН кадр: Pressed в этом
// кадре, Released — в следующем. Так короткий тап никогда не пропадает и
// никогда не превращается в удержание.
// ---------------------------------------------------------------------------
namespace sage::input {

// Состояние одной кнопки: удержание плюс два однокадровых признака.
struct ButtonState {
    bool Down = false;       // удерживается прямо сейчас
    bool Pressed = false;    // стала нажатой ИМЕННО в этом кадре
    bool Released = false;   // была отпущена ИМЕННО в этом кадре

    void BeginFrame() {
        Pressed = false;
        Released = false;
        // Отложенное отпускание: кнопку нажали и отпустили внутри одного кадра
        // (см. заголовок файла) — «отпустили» доезжает сюда.
        if (m_releasePending) {
            Down = false;
            Released = true;
            m_releasePending = false;
        }
    }

    void Press(uint64_t frame) {
        if (Down) return;         // автоповтор системы — не новое нажатие
        Down = true;
        Pressed = true;
        m_pressedFrame = frame;
    }

    void Release(uint64_t frame) {
        if (!Down) return;
        if (m_pressedFrame == frame) {
            m_releasePending = true; // держим кадр, чтобы IsDown хоть раз увидел
            return;
        }
        Down = false;
        Released = true;
    }

    // Ввод отобрали (окно потеряло фокус, редактор увёл фокус из игры). Всё
    // гасится ЧЕСТНО, через Released: иначе действие навсегда останется
    // нажатым, и игрок вернётся в игру, которая сама идёт вперёд.
    void ForceUp() {
        if (Down) Released = true;
        Down = false;
        m_releasePending = false;
    }

private:
    uint64_t m_pressedFrame = 0;
    bool m_releasePending = false;
};

// --- Клавиатура -------------------------------------------------------------
class Keyboard {
public:
    bool Down(Key key) const { return State(key).Down; }
    bool Pressed(Key key) const { return State(key).Pressed; }
    bool Released(Key key) const { return State(key).Released; }

    // Модификаторы считаются ПО СОСТОЯНИЮ САМИХ КЛАВИШ, а не только по тому,
    // что сообщила система в событии. Причина: оконный слой сообщает маску
    // по-разному на разных платформах (в событии нажатия самого Ctrl он то
    // включает его, то нет), а «Ctrl+S сработал через раз» — ошибка, которую
    // ищут неделями. Маска из события при этом добавляется, а не отбрасывается:
    // она единственный источник правды, когда окно получило фокус с уже
    // зажатым Shift и события нажатия не было вовсе.
    uint8_t Modifiers() const;

    // Набранный за кадр ТЕКСТ — готовые символы Unicode, а не коды клавиш:
    // раскладка, Shift, мёртвые клавиши и композиция (ё, é, иероглифы)
    // превращают нажатия в символы по правилам системы, и повторять эти
    // правила в движке нельзя.
    const std::vector<unsigned int>& TypedText() const { return m_text; }

    void BeginFrame();
    void Handle(const InputEvent& e, uint64_t frame);
    void ReleaseAll();

private:
    const ButtonState& State(Key key) const {
        const size_t i = (size_t)key;
        return i < m_keys.size() ? m_keys[i] : m_keys[0];
    }
    std::array<ButtonState, (size_t)Key::Count> m_keys{};
    std::vector<unsigned int> m_text;
    uint8_t m_modifiers = ModNone;
};

// --- Мышь -------------------------------------------------------------------
class Mouse {
public:
    bool Down(MouseButton b) const { return State(b).Down; }
    bool Pressed(MouseButton b) const { return State(b).Pressed; }
    bool Released(MouseButton b) const { return State(b).Released; }

    // Положение курсора в пикселях от левого верхнего угла окна.
    glm::vec2 Position() const { return m_position; }
    // Смещение за ТЕКУЩИЙ кадр. Читать можно сколько угодно раз и откуда
    // угодно — значение не «съедается» первым читателем: потребителей у мыши
    // несколько (камера, скрипт, интерфейс), а событие приходит одно.
    glm::vec2 Delta() const { return m_delta; }
    float Wheel() const { return m_wheel; }
    bool Inside() const { return m_inside; }

    void BeginFrame();
    void Handle(const InputEvent& e, uint64_t frame);
    void ReleaseAll();

private:
    const ButtonState& State(MouseButton b) const {
        const size_t i = (size_t)b;
        return i < m_buttons.size() ? m_buttons[i] : m_buttons[0];
    }
    std::array<ButtonState, (size_t)MouseButton::Count> m_buttons{};
    glm::vec2 m_position{0.0f};
    glm::vec2 m_delta{0.0f};
    float m_wheel = 0.0f;
    bool m_inside = true;
};

// --- Геймпад ----------------------------------------------------------------
//
// Мёртвой зоны здесь НЕТ намеренно. Порог «шевеление меньше 0.15 не считается»
// — это решение ДЕЙСТВИЯ, а не устройства: обзору нужна одна зона, движению
// другая, а меню вообще нужен только знак. Устройство обязано сообщать правду,
// а обрезает её действие (см. ActionSettings::DeadZone).
class Gamepad {
public:
    bool Connected() const { return m_connected; }
    const std::string& Name() const { return m_name; }

    bool Down(GamepadButton b) const { return State(b).Down; }
    bool Pressed(GamepadButton b) const { return State(b).Pressed; }
    bool Released(GamepadButton b) const { return State(b).Released; }

    float Axis(GamepadAxis a) const {
        const size_t i = (size_t)a;
        return i < m_axes.size() ? m_axes[i] : 0.0f;
    }

    void BeginFrame();
    void Handle(const InputEvent& e, uint64_t frame);
    void ReleaseAll();
    void SetName(std::string name) { m_name = std::move(name); }

private:
    const ButtonState& State(GamepadButton b) const {
        const size_t i = (size_t)b;
        return i < m_buttons.size() ? m_buttons[i] : m_buttons[0];
    }
    std::array<ButtonState, (size_t)GamepadButton::Count> m_buttons{};
    std::array<float, (size_t)GamepadAxis::Count> m_axes{};
    bool m_connected = false;
    std::string m_name;
};

// --- Все устройства вместе --------------------------------------------------
//
// Один объект, а не три поля в разных местах: действие привязывают и к
// клавише, и к кнопке геймпада одновременно (§21 ТЗ), поэтому вычислителю
// действия нужен доступ ко всем устройствам сразу и по одному указателю.
class Devices {
public:
    // Открыть новый кадр: погасить однокадровые признаки, обнулить дельты.
    // Зовётся РОВНО ОДИН раз за кадр, до разбора событий.
    void BeginFrame();

    // Применить одно событие ввода к состоянию.
    void Handle(const InputEvent& e);

    // Отпустить всё. Нужно, когда ввод у игры отбирают (потеря фокуса окна,
    // уход фокуса из панели Game в редакторе): без этого клавиша, зажатая в
    // момент alt-tab, остаётся зажатой навсегда.
    void ReleaseAll();

    const Keyboard& Keys() const { return m_keyboard; }
    const Mouse& MouseState() const { return m_mouse; }
    const Gamepad& Pad(int index) const {
        return (index >= 0 && index < kMaxGamepads) ? m_gamepads[(size_t)index] : m_gamepads[0];
    }
    Gamepad& PadMutable(int index) {
        return (index >= 0 && index < kMaxGamepads) ? m_gamepads[(size_t)index] : m_gamepads[0];
    }
    // Первый подключённый геймпад (или 0, если их нет). Привязка «любой
    // геймпад» — то, чего хочет одиночная игра: человек воткнул джойстик в
    // любой порт и играет, не выбирая номер слота.
    int FirstConnected() const;
    bool AnyGamepadConnected() const { return FirstConnected() >= 0; }

    uint64_t Frame() const { return m_frame; }

private:
    Keyboard m_keyboard;
    Mouse m_mouse;
    std::array<Gamepad, (size_t)kMaxGamepads> m_gamepads{};
    uint64_t m_frame = 1;
};

} // namespace sage::input
