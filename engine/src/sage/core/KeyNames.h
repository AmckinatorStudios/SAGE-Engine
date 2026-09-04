#pragma once
#include "InputBinding.h"
#include <GLFW/glfw3.h>
#include <optional>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------
// Сопоставление человекочитаемых имён клавиш/кнопок (для файлов настроек,
// см. GameActions::LoadBindingsFromFile) и GLFW-кодов — чтобы в конфиге
// можно было написать "Jump=Space", а не сырой числовой код.
// ---------------------------------------------------------------------
namespace KeyNames {

inline const std::unordered_map<std::string, int>& KeyTable() {
    static const std::unordered_map<std::string, int> table = {
        {"A", GLFW_KEY_A}, {"B", GLFW_KEY_B}, {"C", GLFW_KEY_C}, {"D", GLFW_KEY_D},
        {"E", GLFW_KEY_E}, {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"H", GLFW_KEY_H},
        {"I", GLFW_KEY_I}, {"J", GLFW_KEY_J}, {"K", GLFW_KEY_K}, {"L", GLFW_KEY_L},
        {"M", GLFW_KEY_M}, {"N", GLFW_KEY_N}, {"O", GLFW_KEY_O}, {"P", GLFW_KEY_P},
        {"Q", GLFW_KEY_Q}, {"R", GLFW_KEY_R}, {"S", GLFW_KEY_S}, {"T", GLFW_KEY_T},
        {"U", GLFW_KEY_U}, {"V", GLFW_KEY_V}, {"W", GLFW_KEY_W}, {"X", GLFW_KEY_X},
        {"Y", GLFW_KEY_Y}, {"Z", GLFW_KEY_Z},
        {"0", GLFW_KEY_0}, {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3},
        {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7},
        {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9},
        {"F1", GLFW_KEY_F1}, {"F2", GLFW_KEY_F2}, {"F3", GLFW_KEY_F3}, {"F4", GLFW_KEY_F4},
        {"F5", GLFW_KEY_F5}, {"F6", GLFW_KEY_F6}, {"F7", GLFW_KEY_F7}, {"F8", GLFW_KEY_F8},
        {"F9", GLFW_KEY_F9}, {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
        {"SPACE", GLFW_KEY_SPACE}, {"TAB", GLFW_KEY_TAB}, {"ENTER", GLFW_KEY_ENTER},
        {"ESCAPE", GLFW_KEY_ESCAPE},
        {"LEFT_SHIFT", GLFW_KEY_LEFT_SHIFT}, {"RIGHT_SHIFT", GLFW_KEY_RIGHT_SHIFT},
        {"LEFT_CONTROL", GLFW_KEY_LEFT_CONTROL}, {"RIGHT_CONTROL", GLFW_KEY_RIGHT_CONTROL},
        {"LEFT_ALT", GLFW_KEY_LEFT_ALT}, {"RIGHT_ALT", GLFW_KEY_RIGHT_ALT},
        {"UP", GLFW_KEY_UP}, {"DOWN", GLFW_KEY_DOWN}, {"LEFT", GLFW_KEY_LEFT}, {"RIGHT", GLFW_KEY_RIGHT},
    };
    return table;
}

// Разбирает имя привязки из файла настроек в InputBinding. Понимает буквы,
// цифры, именованные клавиши из KeyTable() и MOUSE_LEFT/MOUSE_RIGHT/
// MOUSE_MIDDLE. Возвращает std::nullopt, если имя не распознано.
inline std::optional<InputBinding> Parse(const std::string& name) {
    if (name == "MOUSE_LEFT") return InputBinding::Mouse(sage::MouseButton::Left);
    if (name == "MOUSE_RIGHT") return InputBinding::Mouse(sage::MouseButton::Right);
    if (name == "MOUSE_MIDDLE") return InputBinding::Mouse(sage::MouseButton::Middle);

    auto it = KeyTable().find(name);
    if (it != KeyTable().end()) return InputBinding::KeyCode(it->second);
    return std::nullopt;
}

} // namespace KeyNames
