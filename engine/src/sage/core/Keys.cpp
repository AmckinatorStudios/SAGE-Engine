#include "sage/core/Keys.h"

#include <GLFW/glfw3.h>

#include <cstring>

// ---------------------------------------------------------------------------
// ЕДИНСТВЕННОЕ место движка, где встречаются и sage::Key, и коды GLFW.
//
// Заголовок Keys.h объявляет клавиши своими значениями, чтобы игра не включала
// GLFW. Значения при этом СОВПАДАЮТ с GLFW — ради нулевой цены на опросе
// ввода. Совпадение проверяется здесь, статическими утверждениями: если GLFW
// когда-нибудь сменит код, сборка упадёт с именем конкретной клавиши. Без этой
// сверки расхождение проявилось бы иначе — у игрока молча перестала бы
// работать одна кнопка, и искали бы её в игре, а не в движке.
// ---------------------------------------------------------------------------
namespace sage {

namespace {

#define SAGE_CHECK_KEY(name, glfwName)                                            \
    static_assert(static_cast<int>(Key::name) == glfwName,                        \
                  "код клавиши " #name " разошёлся с " #glfwName " — см. Keys.h")

SAGE_CHECK_KEY(Space, GLFW_KEY_SPACE);
SAGE_CHECK_KEY(Apostrophe, GLFW_KEY_APOSTROPHE);
SAGE_CHECK_KEY(Comma, GLFW_KEY_COMMA);
SAGE_CHECK_KEY(Minus, GLFW_KEY_MINUS);
SAGE_CHECK_KEY(Period, GLFW_KEY_PERIOD);
SAGE_CHECK_KEY(Slash, GLFW_KEY_SLASH);
SAGE_CHECK_KEY(Num0, GLFW_KEY_0);
SAGE_CHECK_KEY(Num9, GLFW_KEY_9);
SAGE_CHECK_KEY(Semicolon, GLFW_KEY_SEMICOLON);
SAGE_CHECK_KEY(Equal, GLFW_KEY_EQUAL);
SAGE_CHECK_KEY(A, GLFW_KEY_A);
SAGE_CHECK_KEY(W, GLFW_KEY_W);
SAGE_CHECK_KEY(Z, GLFW_KEY_Z);
SAGE_CHECK_KEY(LeftBracket, GLFW_KEY_LEFT_BRACKET);
SAGE_CHECK_KEY(Backslash, GLFW_KEY_BACKSLASH);
SAGE_CHECK_KEY(RightBracket, GLFW_KEY_RIGHT_BRACKET);
SAGE_CHECK_KEY(GraveAccent, GLFW_KEY_GRAVE_ACCENT);
SAGE_CHECK_KEY(Escape, GLFW_KEY_ESCAPE);
SAGE_CHECK_KEY(Enter, GLFW_KEY_ENTER);
SAGE_CHECK_KEY(Tab, GLFW_KEY_TAB);
SAGE_CHECK_KEY(Backspace, GLFW_KEY_BACKSPACE);
SAGE_CHECK_KEY(Insert, GLFW_KEY_INSERT);
SAGE_CHECK_KEY(Delete, GLFW_KEY_DELETE);
SAGE_CHECK_KEY(Right, GLFW_KEY_RIGHT);
SAGE_CHECK_KEY(Left, GLFW_KEY_LEFT);
SAGE_CHECK_KEY(Down, GLFW_KEY_DOWN);
SAGE_CHECK_KEY(Up, GLFW_KEY_UP);
SAGE_CHECK_KEY(PageUp, GLFW_KEY_PAGE_UP);
SAGE_CHECK_KEY(PageDown, GLFW_KEY_PAGE_DOWN);
SAGE_CHECK_KEY(Home, GLFW_KEY_HOME);
SAGE_CHECK_KEY(End, GLFW_KEY_END);
SAGE_CHECK_KEY(CapsLock, GLFW_KEY_CAPS_LOCK);
SAGE_CHECK_KEY(ScrollLock, GLFW_KEY_SCROLL_LOCK);
SAGE_CHECK_KEY(NumLock, GLFW_KEY_NUM_LOCK);
SAGE_CHECK_KEY(PrintScreen, GLFW_KEY_PRINT_SCREEN);
SAGE_CHECK_KEY(Pause, GLFW_KEY_PAUSE);
SAGE_CHECK_KEY(F1, GLFW_KEY_F1);
SAGE_CHECK_KEY(F12, GLFW_KEY_F12);
SAGE_CHECK_KEY(Keypad0, GLFW_KEY_KP_0);
SAGE_CHECK_KEY(Keypad9, GLFW_KEY_KP_9);
SAGE_CHECK_KEY(KeypadDecimal, GLFW_KEY_KP_DECIMAL);
SAGE_CHECK_KEY(KeypadDivide, GLFW_KEY_KP_DIVIDE);
SAGE_CHECK_KEY(KeypadMultiply, GLFW_KEY_KP_MULTIPLY);
SAGE_CHECK_KEY(KeypadSubtract, GLFW_KEY_KP_SUBTRACT);
SAGE_CHECK_KEY(KeypadAdd, GLFW_KEY_KP_ADD);
SAGE_CHECK_KEY(KeypadEnter, GLFW_KEY_KP_ENTER);
SAGE_CHECK_KEY(KeypadEqual, GLFW_KEY_KP_EQUAL);
SAGE_CHECK_KEY(LeftShift, GLFW_KEY_LEFT_SHIFT);
SAGE_CHECK_KEY(LeftControl, GLFW_KEY_LEFT_CONTROL);
SAGE_CHECK_KEY(LeftAlt, GLFW_KEY_LEFT_ALT);
SAGE_CHECK_KEY(LeftSuper, GLFW_KEY_LEFT_SUPER);
SAGE_CHECK_KEY(RightShift, GLFW_KEY_RIGHT_SHIFT);
SAGE_CHECK_KEY(RightControl, GLFW_KEY_RIGHT_CONTROL);
SAGE_CHECK_KEY(RightAlt, GLFW_KEY_RIGHT_ALT);
SAGE_CHECK_KEY(RightSuper, GLFW_KEY_RIGHT_SUPER);
SAGE_CHECK_KEY(Menu, GLFW_KEY_MENU);
SAGE_CHECK_KEY(Unknown, GLFW_KEY_UNKNOWN);

#undef SAGE_CHECK_KEY

static_assert(static_cast<int>(MouseButton::Left) == GLFW_MOUSE_BUTTON_LEFT,
              "код левой кнопки мыши разошёлся с GLFW");
static_assert(static_cast<int>(MouseButton::Right) == GLFW_MOUSE_BUTTON_RIGHT,
              "код правой кнопки мыши разошёлся с GLFW");
static_assert(static_cast<int>(MouseButton::Middle) == GLFW_MOUSE_BUTTON_MIDDLE,
              "код средней кнопки мыши разошёлся с GLFW");

// Таблица имён. Одна на оба направления — иначе имя и разбор имени однажды
// разойдутся, и раскладка, сохранённая движком, перестанет им же читаться.
struct NamedKey {
    Key Code;
    const char* Name;
};

constexpr NamedKey kNamedKeys[] = {
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"},
    {Key::F, "F"}, {Key::G, "G"}, {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"},
    {Key::K, "K"}, {Key::L, "L"}, {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"},
    {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"},
    {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"}, {Key::Y, "Y"},
    {Key::Z, "Z"},
    {Key::Num0, "0"}, {Key::Num1, "1"}, {Key::Num2, "2"}, {Key::Num3, "3"},
    {Key::Num4, "4"}, {Key::Num5, "5"}, {Key::Num6, "6"}, {Key::Num7, "7"},
    {Key::Num8, "8"}, {Key::Num9, "9"},
    {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"},
    {Key::F5, "F5"}, {Key::F6, "F6"}, {Key::F7, "F7"}, {Key::F8, "F8"},
    {Key::F9, "F9"}, {Key::F10, "F10"}, {Key::F11, "F11"}, {Key::F12, "F12"},
    {Key::Space, "SPACE"}, {Key::Tab, "TAB"}, {Key::Enter, "ENTER"},
    {Key::Escape, "ESCAPE"}, {Key::Backspace, "BACKSPACE"}, {Key::Delete, "DELETE"},
    {Key::Insert, "INSERT"}, {Key::Home, "HOME"}, {Key::End, "END"},
    {Key::PageUp, "PAGE_UP"}, {Key::PageDown, "PAGE_DOWN"},
    {Key::LeftShift, "LEFT_SHIFT"}, {Key::RightShift, "RIGHT_SHIFT"},
    {Key::LeftControl, "LEFT_CONTROL"}, {Key::RightControl, "RIGHT_CONTROL"},
    {Key::LeftAlt, "LEFT_ALT"}, {Key::RightAlt, "RIGHT_ALT"},
    {Key::Up, "UP"}, {Key::Down, "DOWN"}, {Key::Left, "LEFT"}, {Key::Right, "RIGHT"},
    {Key::Minus, "MINUS"}, {Key::Equal, "EQUAL"}, {Key::Comma, "COMMA"},
    {Key::Period, "PERIOD"}, {Key::Slash, "SLASH"}, {Key::Semicolon, "SEMICOLON"},
    {Key::Apostrophe, "APOSTROPHE"}, {Key::GraveAccent, "GRAVE_ACCENT"},
    {Key::LeftBracket, "LEFT_BRACKET"}, {Key::RightBracket, "RIGHT_BRACKET"},
    {Key::Backslash, "BACKSLASH"},
};

} // namespace

const char* KeyName(Key key) {
    for (const NamedKey& entry : kNamedKeys) {
        if (entry.Code == key) return entry.Name;
    }
    return "";
}

Key KeyFromName(const char* name) {
    if (!name || !*name) return Key::Unknown;
    for (const NamedKey& entry : kNamedKeys) {
        if (std::strcmp(entry.Name, name) == 0) return entry.Code;
    }
    return Key::Unknown;
}

} // namespace sage
