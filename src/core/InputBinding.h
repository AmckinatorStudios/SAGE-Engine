#pragma once
#include <GLFW/glfw3.h>
#include <cstdint>

// ---------------------------------------------------------------------
// Один физический источник ввода, к которому можно привязать действие:
// клавиша клавиатуры, кнопка мыши или направление колеса мыши.
//
// Специально не отделяем "клавиатуру" от "мыши" через отдельные enum'ы —
// действию всё равно, откуда пришёл сигнал (Jump можно повесить и на
// Space, и на, скажем, среднюю кнопку мыши), поэтому Binding — это один
// общий тип с полем Kind, определяющим, как его читать.
// ---------------------------------------------------------------------

enum class BindingKind : uint8_t {
    Keyboard,
    MouseButton,
    ScrollUp,   // разовое "нажатие" при прокрутке колеса вверх
    ScrollDown,
};

struct InputBinding {
    BindingKind Kind = BindingKind::Keyboard;
    int Code = 0; // GLFW_KEY_* или GLFW_MOUSE_BUTTON_*, для скролла не используется

    static InputBinding Key(int glfwKey) { return {BindingKind::Keyboard, glfwKey}; }
    static InputBinding Mouse(int glfwButton) { return {BindingKind::MouseButton, glfwButton}; }
    static InputBinding WheelUp() { return {BindingKind::ScrollUp, 0}; }
    static InputBinding WheelDown() { return {BindingKind::ScrollDown, 0}; }

    bool operator==(const InputBinding& other) const {
        return Kind == other.Kind && Code == other.Code;
    }
};
