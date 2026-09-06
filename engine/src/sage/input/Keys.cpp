#include "sage/input/Keys.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace sage::input {

namespace {

std::string Upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

// Таблицы имён строятся ИЗ ТЕХ ЖЕ списков, что и перечисления (см. Keys.h):
// добавленная клавиша получает имя автоматически, забыть её здесь невозможно.
const std::vector<const char*>& KeyNameTable() {
    static const std::vector<const char*> t = [] {
        std::vector<const char*> v;
        v.push_back("UNKNOWN");
#define SAGE_INPUT_KEY_NAME(id, name) v.push_back(name);
        SAGE_INPUT_KEY_LIST(SAGE_INPUT_KEY_NAME)
#undef SAGE_INPUT_KEY_NAME
        return v;
    }();
    return t;
}

const std::unordered_map<std::string, Key>& KeyLookup() {
    static const std::unordered_map<std::string, Key> m = [] {
        std::unordered_map<std::string, Key> t;
        const auto& names = KeyNameTable();
        for (size_t i = 1; i < names.size(); ++i) t[names[i]] = (Key)i;
        // Синонимы: так эти клавиши называют в других движках и в привычке.
        // Отказать из-за "RETURN" вместо "ENTER" — значит заставить человека
        // угадывать словарь, которого он не видел.
        t["RETURN"] = Key::Enter;
        t["ESC"] = Key::Escape;
        t["CTRL"] = Key::LeftControl;
        t["SHIFT"] = Key::LeftShift;
        t["ALT"] = Key::LeftAlt;
        t["SUPER"] = Key::LeftSuper;
        t["DEL"] = Key::Delete;
        t["PGUP"] = Key::PageUp;
        t["PGDN"] = Key::PageDown;
        return t;
    }();
    return m;
}

} // namespace

const char* KeyName(Key key) {
    const auto& t = KeyNameTable();
    const size_t i = (size_t)key;
    return i < t.size() ? t[i] : t[0];
}

Key ParseKey(const std::string& name) {
    const auto& m = KeyLookup();
    auto it = m.find(Upper(name));
    return it == m.end() ? Key::Unknown : it->second;
}

// --- Мышь -------------------------------------------------------------------

namespace {
const std::vector<const char*>& MouseNameTable() {
    static const std::vector<const char*> t = {
#define SAGE_INPUT_MOUSE_NAME(id, name) name,
        SAGE_INPUT_MOUSE_LIST(SAGE_INPUT_MOUSE_NAME)
#undef SAGE_INPUT_MOUSE_NAME
    };
    return t;
}
} // namespace

const char* MouseButtonName(MouseButton button) {
    const auto& t = MouseNameTable();
    const size_t i = (size_t)button;
    return i < t.size() ? t[i] : "MOUSE_LEFT";
}

MouseButton ParseMouseButton(const std::string& name, bool* ok) {
    const std::string up = Upper(name);
    const auto& t = MouseNameTable();
    for (size_t i = 0; i < t.size(); ++i) {
        if (up == t[i]) {
            if (ok) *ok = true;
            return (MouseButton)i;
        }
    }
    if (ok) *ok = false;
    return MouseButton::Left;
}

// --- Геймпад ----------------------------------------------------------------

namespace {
const std::vector<const char*>& PadButtonNameTable() {
    static const std::vector<const char*> t = {
#define SAGE_INPUT_PAD_BUTTON_NAME(id, name) name,
        SAGE_INPUT_PAD_BUTTON_LIST(SAGE_INPUT_PAD_BUTTON_NAME)
#undef SAGE_INPUT_PAD_BUTTON_NAME
    };
    return t;
}
const std::vector<const char*>& PadAxisNameTable() {
    static const std::vector<const char*> t = {
#define SAGE_INPUT_PAD_AXIS_NAME(id, name) name,
        SAGE_INPUT_PAD_AXIS_LIST(SAGE_INPUT_PAD_AXIS_NAME)
#undef SAGE_INPUT_PAD_AXIS_NAME
    };
    return t;
}
} // namespace

const char* GamepadButtonName(GamepadButton button) {
    const auto& t = PadButtonNameTable();
    const size_t i = (size_t)button;
    return i < t.size() ? t[i] : "PAD_A";
}

GamepadButton ParseGamepadButton(const std::string& name, bool* ok) {
    const std::string up = Upper(name);
    const auto& t = PadButtonNameTable();
    for (size_t i = 0; i < t.size(); ++i) {
        if (up == t[i]) {
            if (ok) *ok = true;
            return (GamepadButton)i;
        }
    }
    if (ok) *ok = false;
    return GamepadButton::A;
}

const char* GamepadAxisName(GamepadAxis axis) {
    const auto& t = PadAxisNameTable();
    const size_t i = (size_t)axis;
    return i < t.size() ? t[i] : "PAD_LEFT_X";
}

GamepadAxis ParseGamepadAxis(const std::string& name, bool* ok) {
    const std::string up = Upper(name);
    const auto& t = PadAxisNameTable();
    for (size_t i = 0; i < t.size(); ++i) {
        if (up == t[i]) {
            if (ok) *ok = true;
            return (GamepadAxis)i;
        }
    }
    if (ok) *ok = false;
    return GamepadAxis::LeftX;
}

// --- Модификаторы -----------------------------------------------------------

uint8_t ParseModifiers(const std::string& text, std::string& rest) {
    uint8_t mods = ModNone;
    std::string tail = text;
    // Разбираем ПО ОДНОМУ префиксу до последнего '+', а не по всем '+' сразу:
    // сама клавиша может называться "KP_ADD", и делить строку по каждому плюсу
    // значило бы не понять "CTRL+KP_ADD".
    for (;;) {
        const size_t plus = tail.find('+');
        if (plus == std::string::npos || plus == 0) break;
        const std::string head = Upper(tail.substr(0, plus));
        uint8_t bit = ModNone;
        if (head == "CTRL" || head == "CONTROL") bit = ModCtrl;
        else if (head == "SHIFT") bit = ModShift;
        else if (head == "ALT") bit = ModAlt;
        else if (head == "SUPER" || head == "CMD" || head == "WIN") bit = ModSuper;
        if (bit == ModNone) break; // не модификатор — значит начался сам код
        mods |= bit;
        tail = tail.substr(plus + 1);
    }
    rest = tail;
    return mods;
}

std::string ModifiersToString(uint8_t mods) {
    // Порядок фиксирован (Ctrl, Shift, Alt, Super), чтобы одна и та же привязка
    // всегда писалась в файл одинаково: иначе сравнение строк и различия в
    // истории версий начинают зависеть от порядка нажатия при записи.
    std::string out;
    if (mods & ModCtrl) out += "CTRL+";
    if (mods & ModShift) out += "SHIFT+";
    if (mods & ModAlt) out += "ALT+";
    if (mods & ModSuper) out += "SUPER+";
    return out;
}

// --- Списки для интерфейса --------------------------------------------------

const std::vector<std::string>& AllKeyNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        const auto& t = KeyNameTable();
        for (size_t i = 1; i < t.size(); ++i) out.emplace_back(t[i]);
        return out;
    }();
    return v;
}

const std::vector<std::string>& AllMouseNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const char* n : MouseNameTable()) out.emplace_back(n);
        out.emplace_back("WHEEL_UP");
        out.emplace_back("WHEEL_DOWN");
        out.emplace_back("MOUSE_X");
        out.emplace_back("MOUSE_Y");
        return out;
    }();
    return v;
}

const std::vector<std::string>& AllGamepadNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const char* n : PadButtonNameTable()) out.emplace_back(n);
        for (const char* n : PadAxisNameTable()) out.emplace_back(n);
        return out;
    }();
    return v;
}

} // namespace sage::input
