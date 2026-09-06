#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// СЛОВАРЬ УСТРОЙСТВ ВВОДА — клавиши, кнопки и оси, названные СВОИМИ именами.
//
// Здесь нет ни одного включения GLFW, и это главное свойство файла. Ввод
// движка описывается его собственным перечислением, а платформенные коды
// живут ровно в одном месте — sage/input/GlfwBridge.h, где GLFW переводится
// сюда. Пока раскладка хранила GLFW_KEY_*, любая сохранённая настройка
// управления была привязана к версии стороннего заголовка: сменился бы
// оконный слой (или сами константы) — и файл настроек игрока начал бы значить
// другое, молча.
//
// ИМЕНА — часть формата данных. Раскладка сохраняется в проект и в настройки
// игрока строками ("SPACE", "LEFT_SHIFT", "MOUSE_LEFT"), а не числами: число
// в файле нечитаемо, не переживает вставку новой клавиши в середину
// перечисления и ничего не говорит человеку, который открыл файл руками.
// Имена совпадают с теми, что уже пишут в скриптах игр (BindAction("Jump",
// "SPACE")), поэтому старые скрипты продолжают работать без правок.
// ---------------------------------------------------------------------------
namespace sage::input {

// Список клавиш ОДИН на всё: и перечисление, и таблица имён строятся из него.
// Двумя списками они рано или поздно разъезжаются — добавили клавишу в enum,
// забыли в имена, и клавиша есть, а сохранить её нельзя.
#define SAGE_INPUT_KEY_LIST(X)                                                 \
    X(A, "A") X(B, "B") X(C, "C") X(D, "D") X(E, "E") X(F, "F") X(G, "G")      \
    X(H, "H") X(I, "I") X(J, "J") X(K, "K") X(L, "L") X(M, "M") X(N, "N")      \
    X(O, "O") X(P, "P") X(Q, "Q") X(R, "R") X(S, "S") X(T, "T") X(U, "U")      \
    X(V, "V") X(W, "W") X(X_, "X") X(Y, "Y") X(Z, "Z")                         \
    X(Num0, "0") X(Num1, "1") X(Num2, "2") X(Num3, "3") X(Num4, "4")           \
    X(Num5, "5") X(Num6, "6") X(Num7, "7") X(Num8, "8") X(Num9, "9")           \
    X(F1, "F1") X(F2, "F2") X(F3, "F3") X(F4, "F4") X(F5, "F5") X(F6, "F6")    \
    X(F7, "F7") X(F8, "F8") X(F9, "F9") X(F10, "F10") X(F11, "F11")            \
    X(F12, "F12") X(F13, "F13") X(F14, "F14") X(F15, "F15") X(F16, "F16")      \
    X(Space, "SPACE") X(Tab, "TAB") X(Enter, "ENTER") X(Escape, "ESCAPE")      \
    X(Backspace, "BACKSPACE") X(Insert, "INSERT") X(Delete, "DELETE")          \
    X(Up, "UP") X(Down, "DOWN") X(Left, "LEFT") X(Right, "RIGHT")              \
    X(PageUp, "PAGE_UP") X(PageDown, "PAGE_DOWN") X(Home, "HOME") X(End, "END")\
    X(CapsLock, "CAPS_LOCK") X(ScrollLock, "SCROLL_LOCK") X(NumLock, "NUM_LOCK")\
    X(PrintScreen, "PRINT_SCREEN") X(Pause, "PAUSE") X(Menu, "MENU")           \
    X(LeftShift, "LEFT_SHIFT") X(RightShift, "RIGHT_SHIFT")                    \
    X(LeftControl, "LEFT_CONTROL") X(RightControl, "RIGHT_CONTROL")            \
    X(LeftAlt, "LEFT_ALT") X(RightAlt, "RIGHT_ALT")                            \
    X(LeftSuper, "LEFT_SUPER") X(RightSuper, "RIGHT_SUPER")                    \
    X(Minus, "MINUS") X(Equal, "EQUAL")                                        \
    X(LeftBracket, "LEFT_BRACKET") X(RightBracket, "RIGHT_BRACKET")            \
    X(Backslash, "BACKSLASH") X(Semicolon, "SEMICOLON")                        \
    X(Apostrophe, "APOSTROPHE") X(GraveAccent, "GRAVE_ACCENT")                 \
    X(Comma, "COMMA") X(Period, "PERIOD") X(Slash, "SLASH")                    \
    X(World1, "WORLD_1") X(World2, "WORLD_2")                                  \
    X(Kp0, "KP_0") X(Kp1, "KP_1") X(Kp2, "KP_2") X(Kp3, "KP_3")                \
    X(Kp4, "KP_4") X(Kp5, "KP_5") X(Kp6, "KP_6") X(Kp7, "KP_7")                \
    X(Kp8, "KP_8") X(Kp9, "KP_9") X(KpDecimal, "KP_DECIMAL")                   \
    X(KpDivide, "KP_DIVIDE") X(KpMultiply, "KP_MULTIPLY")                      \
    X(KpSubtract, "KP_SUBTRACT") X(KpAdd, "KP_ADD") X(KpEnter, "KP_ENTER")     \
    X(KpEqual, "KP_EQUAL")

// X_, а не X: буква X занята самим макросом-параметром списка. Имя в файлах и
// в интерфейсе от этого не страдает — оно берётся из второй колонки ("X").
enum class Key : uint16_t {
    Unknown = 0,
#define SAGE_INPUT_KEY_ENUM(id, name) id,
    SAGE_INPUT_KEY_LIST(SAGE_INPUT_KEY_ENUM)
#undef SAGE_INPUT_KEY_ENUM
    Count
};

#define SAGE_INPUT_MOUSE_LIST(X)                                               \
    X(Left, "MOUSE_LEFT") X(Right, "MOUSE_RIGHT") X(Middle, "MOUSE_MIDDLE")    \
    X(Extra1, "MOUSE_4") X(Extra2, "MOUSE_5")

enum class MouseButton : uint8_t {
#define SAGE_INPUT_MOUSE_ENUM(id, name) id,
    SAGE_INPUT_MOUSE_LIST(SAGE_INPUT_MOUSE_ENUM)
#undef SAGE_INPUT_MOUSE_ENUM
    Count
};

// Кнопки геймпада названы по РАСКЛАДКЕ XBOX не из любви к ней, а потому что
// это единственная раскладка, которую понимает и оконный слой (GLFW отдаёт
// «стандартный геймпад»), и человек, читающий файл настроек. Кто держит
// PlayStation, видит на экране Cross вместо A — это дело подписи в интерфейсе,
// а не хранимого имени.
#define SAGE_INPUT_PAD_BUTTON_LIST(X)                                          \
    X(A, "PAD_A") X(B, "PAD_B") X(X_, "PAD_X") X(Y, "PAD_Y")                   \
    X(LeftBumper, "PAD_LB") X(RightBumper, "PAD_RB")                           \
    X(Back, "PAD_BACK") X(Start, "PAD_START") X(Guide, "PAD_GUIDE")            \
    X(LeftThumb, "PAD_LEFT_THUMB") X(RightThumb, "PAD_RIGHT_THUMB")            \
    X(DPadUp, "PAD_DPAD_UP") X(DPadRight, "PAD_DPAD_RIGHT")                    \
    X(DPadDown, "PAD_DPAD_DOWN") X(DPadLeft, "PAD_DPAD_LEFT")

enum class GamepadButton : uint8_t {
#define SAGE_INPUT_PAD_BUTTON_ENUM(id, name) id,
    SAGE_INPUT_PAD_BUTTON_LIST(SAGE_INPUT_PAD_BUTTON_ENUM)
#undef SAGE_INPUT_PAD_BUTTON_ENUM
    Count
};

// Курки (LeftTrigger/RightTrigger) — тоже оси, а не кнопки: они аналоговые, и
// «газ нажат наполовину» существует. Кнопкой курок становится сравнением с
// порогом, и это решение действия, а не устройства.
#define SAGE_INPUT_PAD_AXIS_LIST(X)                                            \
    X(LeftX, "PAD_LEFT_X") X(LeftY, "PAD_LEFT_Y")                              \
    X(RightX, "PAD_RIGHT_X") X(RightY, "PAD_RIGHT_Y")                          \
    X(LeftTrigger, "PAD_LEFT_TRIGGER") X(RightTrigger, "PAD_RIGHT_TRIGGER")

enum class GamepadAxis : uint8_t {
#define SAGE_INPUT_PAD_AXIS_ENUM(id, name) id,
    SAGE_INPUT_PAD_AXIS_LIST(SAGE_INPUT_PAD_AXIS_ENUM)
#undef SAGE_INPUT_PAD_AXIS_ENUM
    Count
};

// Сколько геймпадов движок готов видеть одновременно. Четыре — потому что
// столько посадочных мест у дивана; больше поддерживает оконный слой, но
// каждый лишний слот стоит опроса каждый кадр и ничего не даёт.
constexpr int kMaxGamepads = 4;

// Модификаторы — БИТОВАЯ МАСКА, а не отдельные клавиши: привязка «Ctrl+S»
// обязана отличать «S при зажатом Ctrl» от просто «S», и хранить это парой
// «клавиша + требуемые модификаторы» короче и надёжнее, чем списком клавиш.
// Левый и правый Ctrl здесь неразличимы намеренно: ни одна игра не назначает
// разное действие на левый и правый Shift, а различать их — значит требовать
// от игрока попасть в конкретный.
enum Modifier : uint8_t {
    ModNone  = 0,
    ModShift = 1 << 0,
    ModCtrl  = 1 << 1,
    ModAlt   = 1 << 2,
    ModSuper = 1 << 3,
};

// --- Имена <-> значения ----------------------------------------------------
//
// Разбор ЧУВСТВИТЕЛЕН К РЕГИСТРУ лишь настолько, насколько это безопасно:
// "space", "Space" и "SPACE" — одно и то же. Файл настроек правит человек, и
// отказ из-за строчной буквы он прочитает как «моя настройка не работает».

const char* KeyName(Key key);
Key ParseKey(const std::string& name);           // Key::Unknown, если не понято

const char* MouseButtonName(MouseButton button);
MouseButton ParseMouseButton(const std::string& name, bool* ok = nullptr);

const char* GamepadButtonName(GamepadButton button);
GamepadButton ParseGamepadButton(const std::string& name, bool* ok = nullptr);

const char* GamepadAxisName(GamepadAxis axis);
GamepadAxis ParseGamepadAxis(const std::string& name, bool* ok = nullptr);

// Маска модификаторов из префикса строки ("CTRL+SHIFT+S" -> ModCtrl|ModShift,
// остаток "S" кладётся в rest). Отдельной функцией, потому что префикс общий
// для клавиш, кнопок мыши и кнопок геймпада — правило разбора должно быть
// записано один раз.
uint8_t ParseModifiers(const std::string& text, std::string& rest);
std::string ModifiersToString(uint8_t mods); // "CTRL+SHIFT+" или пустая строка

// Все известные имена клавиш/кнопок/осей — для выпадающего списка в настройках
// управления и для проверок. Порядок детерминированный (как в списках выше).
const std::vector<std::string>& AllKeyNames();
const std::vector<std::string>& AllMouseNames();
const std::vector<std::string>& AllGamepadNames();

} // namespace sage::input
