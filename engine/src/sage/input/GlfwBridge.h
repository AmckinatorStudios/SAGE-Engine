#pragma once
#include <array>

#include <glm/glm.hpp>

#include "sage/input/InputSystem.h"
#include "sage/input/Keys.h"

class Window;

// ---------------------------------------------------------------------------
// ГРАНИЦА ПЛАТФОРМЫ. Единственный файл ввода, который знает про GLFW.
//
// Ровно так же, как вызовы gl* живут только в engine/src/rhi/, платформенные
// коды клавиш живут только здесь. Всё остальное в sage/input/ говорит на своём
// словаре (Keys.h) и потому проверяется тестом без окна, переживает смену
// оконного слоя и не тащит <GLFW/glfw3.h> в каждый файл игры.
//
// Мост делает три вещи и ничего сверх:
//   1. подписывается на события окна и переводит их в InputEvent;
//   2. опрашивает геймпады (событий для них оконная система не шлёт вовсе —
//      джойстик приходится спрашивать самому, раз в кадр) и превращает
//      РАЗНИЦУ с прошлым кадром в события: нажали, отпустили, ось сдвинулась;
//   3. исполняет захват курсора (CursorControl).
// ---------------------------------------------------------------------------
namespace sage::input {

class GlfwBridge : public CursorControl {
public:
    // Подписывает мост на события окна и направляет их в input. Звать один
    // раз; окно и система ввода должны пережить мост.
    void Attach(Window& window, InputSystem& input);

    // Раз в кадр, ДО InputSystem::Update: у геймпадов нет событий, их состояние
    // приходится спрашивать.
    void PollGamepads();

    void SetCursorCaptured(bool captured) override;
    bool CursorCaptured() const override;

    // Перевод кодов GLFW в словарь движка. Публично, потому что тем же
    // переводом пользуется редактор: панель Game отдаёт игре события,
    // приходящие к ней от ImGui в кодах GLFW.
    static Key FromGlfwKey(int glfwKey);
    static MouseButton FromGlfwMouseButton(int glfwButton, bool* ok);
    static uint8_t FromGlfwMods(int glfwMods);

private:
    void EmitPadEvents(int slot);

    Window* m_window = nullptr;
    InputSystem* m_input = nullptr;

    // Положение курсора прошлого события — из него считается смещение. GLFW
    // отдаёт абсолютную позицию, а обзору от первого лица нужна разница.
    glm::vec2 m_lastCursor{0.0f};
    bool m_firstCursorEvent = true;

    // Снимок геймпадов прошлого кадра: событие «кнопку нажали» существует
    // только как РАЗНИЦА между двумя опросами.
    struct PadSnapshot {
        bool Connected = false;
        std::array<bool, (size_t)GamepadButton::Count> Buttons{};
        std::array<float, (size_t)GamepadAxis::Count> Axes{};
    };
    std::array<PadSnapshot, (size_t)kMaxGamepads> m_pads{};
};

} // namespace sage::input
