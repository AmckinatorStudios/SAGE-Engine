#pragma once
#include <cstdint>

#include "sage/core/Keys.h"

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
    // Код клавиши (sage::Key) или кнопки мыши (sage::MouseButton); для скролла
    // не используется. Хранится числом, потому что одно поле обслуживает оба
    // вида — какой именно, говорит Kind.
    int Code = 0;

    // ОСНОВНОЙ способ привязки: типизированный, без единого заголовка сторонней
    // библиотеки в игровом коде (см. sage/core/Keys.h).
    static InputBinding Key(sage::Key key) {
        return {BindingKind::Keyboard, sage::KeyCode(key)};
    }
    static InputBinding Mouse(sage::MouseButton button) {
        return {BindingKind::MouseButton, sage::ButtonCode(button)};
    }

    // Те же привязки числом. Нужны там, где клавиша приходит уже числом —
    // из файла раскладки, из скрипта, из плагина. Игровому коду они не нужны:
    // число вместо sage::Key::W ничего не сообщает читателю и не проверяется
    // компилятором.
    static InputBinding KeyCode(int code) { return {BindingKind::Keyboard, code}; }
    static InputBinding MouseCode(int code) { return {BindingKind::MouseButton, code}; }

    static InputBinding WheelUp() { return {BindingKind::ScrollUp, 0}; }
    static InputBinding WheelDown() { return {BindingKind::ScrollDown, 0}; }

    bool operator==(const InputBinding& other) const {
        return Kind == other.Kind && Code == other.Code;
    }
};
