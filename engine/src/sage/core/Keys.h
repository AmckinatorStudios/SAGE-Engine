#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Клавиши и кнопки мыши — СВОИ, а не коды GLFW.
//
// ЗАЧЕМ. Игра, которая хотела повесить движение на WASD, писала так:
//
//     #include <GLFW/glfw3.h>
//     actions.Register("Вперёд").Bind(InputBinding::Key(GLFW_KEY_W));
//
// То есть, чтобы воспользоваться вводом ДВИЖКА, автор игры был обязан знать,
// что внутри стоит GLFW, включить его заголовок и писать его константы. Это
// протечка реализации в публичный API, и вредна она не эстетически:
//
//   • движок перестаёт быть «единой платформой» — игра подключает и связывает
//     сторонние библиотеки сама, хотя просила у движка всего лишь ввод;
//   • смена оконной библиотеки (или появление второго бэкенда — консоль,
//     мобильные) ломает КАЖДУЮ игру, а не одну реализацию внутри движка;
//   • автор игры вынужден читать документацию GLFW там, где рассчитывал
//     обойтись документацией SAGE.
//
// Теперь игра пишет `InputBinding::Key(sage::Key::W)` и про GLFW не знает.
//
// ПОЧЕМУ ЗНАЧЕНИЯ СОВПАДАЮТ С GLFW. Чтобы привязка не стоила ни одной
// трансляции в горячем цикле опроса ввода. Совпадение при этом не «известно
// из документации», а ПРОВЕРЯЕТСЯ компилятором: Keys.cpp — единственное место,
// которое включает и этот заголовок, и GLFW, и сверяет каждую константу
// статическим утверждением. Разъедутся — сборка упадёт с именем клавиши, а не
// молча перестанет работать управление у игрока.
// ---------------------------------------------------------------------------
namespace sage {

enum class Key : int {
    Unknown = -1,

    Space = 32,
    Apostrophe = 39,
    Comma = 44,
    Minus = 45,
    Period = 46,
    Slash = 47,

    Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

    Semicolon = 59,
    Equal = 61,

    A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    LeftBracket = 91,
    Backslash = 92,
    RightBracket = 93,
    GraveAccent = 96,

    Escape = 256,
    Enter = 257,
    Tab = 258,
    Backspace = 259,
    Insert = 260,
    Delete = 261,
    Right = 262,
    Left = 263,
    Down = 264,
    Up = 265,
    PageUp = 266,
    PageDown = 267,
    Home = 268,
    End = 269,
    CapsLock = 280,
    ScrollLock = 281,
    NumLock = 282,
    PrintScreen = 283,
    Pause = 284,

    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    Keypad0 = 320, Keypad1, Keypad2, Keypad3, Keypad4,
    Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal = 330,
    KeypadDivide = 331,
    KeypadMultiply = 332,
    KeypadSubtract = 333,
    KeypadAdd = 334,
    KeypadEnter = 335,
    KeypadEqual = 336,

    LeftShift = 340,
    LeftControl = 341,
    LeftAlt = 342,
    LeftSuper = 343,
    RightShift = 344,
    RightControl = 345,
    RightAlt = 346,
    RightSuper = 347,
    Menu = 348,
};

enum class MouseButton : int {
    Left = 0,
    Right = 1,
    Middle = 2,
    Extra1 = 3,
    Extra2 = 4,
    Extra3 = 5,
    Extra4 = 6,
    Extra5 = 7,
};

// Числовой код — для тех мест, где клавиша приходит уже числом: файл настроек,
// скрипт, плагин. Обычному игровому коду это не нужно.
constexpr int KeyCode(Key key) { return static_cast<int>(key); }
constexpr int ButtonCode(MouseButton button) { return static_cast<int>(button); }

// Имя клавиши в том виде, в каком оно пишется в файле раскладки («W», «SPACE»,
// «LEFT_SHIFT»). Пусто — клавиша без имени. Обратное преобразование — в
// KeyNames.h, вместе с разбором строк раскладки.
const char* KeyName(Key key);
// Разбор имени в клавишу. Key::Unknown — имя не опознано.
Key KeyFromName(const char* name);

} // namespace sage
