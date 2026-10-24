#pragma once
// Общее для файлов панели «Управление» (InputPanel*.cpp). Только для них.
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <vector>
#include <imgui.h>
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "panels/InputPanel.h"

using sage::input::Action;
using sage::input::ActionType;
using sage::input::Binding;
using sage::input::Component;
using sage::input::Context;
using sage::input::DeadZoneMode;
using sage::input::DeviceKeyboard;
using sage::input::DeviceMouse;
using sage::input::DeviceNone;
using sage::input::InputEvent;
using sage::input::InputEventType;
using sage::input::InputSystem;
using sage::input::SourceKind;
using sage::input::TriggerMode;

namespace inputpanel_detail {

// Подписи видов действий. Порядок совпадает с ActionType — на этом держится
// перевод индекса комбо-бокса обратно в вид, и менять его нельзя.
inline const char* ActionTypeLabel(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("Button (pressed / not pressed)");
        case ActionType::Axis:    return T("Axis (-1 .. 1)");
        case ActionType::Vector:  return T("Vector (two axes)");
    }
    return "";
}

// Короткая подпись — для бейджа в строке списка, где полная не влезает.
inline const char* ActionTypeShort(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("Button");
        case ActionType::Axis:    return T("Axis");
        case ActionType::Vector:  return T("Vector");
    }
    return "";
}

inline const char* TriggerLabel(TriggerMode mode) {
    switch (mode) {
        case TriggerMode::Press:   return T("On press");
        case TriggerMode::Release: return T("On release");
        case TriggerMode::Hold:    return T("On hold");
        case TriggerMode::Tap:     return T("On short tap");
    }
    return "";
}

// Значок устройства привязки. Рисунком, а не словом: в строке списка клавиш
// пять штук, и «Клавиатура, Клавиатура, Мышь» занимает больше места, чем сами
// клавиши, ради сведения, которое видно по значку без чтения.
inline const char* DeviceIcon(const Binding& b) {
    switch (b.Kind) {
        case SourceKind::Key: return "keyboard";
        case SourceKind::MouseButton:
        case SourceKind::MouseWheel:
        case SourceKind::MouseAxis: return "select";
        case SourceKind::GamepadButton:
        case SourceKind::GamepadAxis: return "pilot";
        case SourceKind::None: return "question";
    }
    return "question";
}

// Из события ввода — привязка, которую человек только что нажал. Пусто, если
// событие не про назначение (движение мыши, отпускание, набранный символ).
//
// Движение мыши сюда НЕ попадает намеренно: назначить «ось мыши» ловлей
// нельзя — курсор шевелится всегда, и первая же дрожь руки заняла бы поле.
// Оси мыши и стиков назначаются списком из восьми пунктов, где выбор осмыслен.
inline bool BindingFromEvent(const InputEvent& e, Binding& out) {
    switch (e.Type) {
        case InputEventType::KeyPressed: {
            // Сам модификатор действием не назначают: «действие на Shift»
            // почти всегда означает, что человек ещё не дожал сочетание.
            switch (e.Keyboard) {
                case sage::input::Key::LeftShift:
                case sage::input::Key::RightShift:
                case sage::input::Key::LeftControl:
                case sage::input::Key::RightControl:
                case sage::input::Key::LeftAlt:
                case sage::input::Key::RightAlt:
                case sage::input::Key::LeftSuper:
                case sage::input::Key::RightSuper:
                    return false;
                default: break;
            }
            out = Binding::OfKey(e.Keyboard, e.Modifiers);
            return true;
        }
        case InputEventType::MouseButtonPressed:
            out = Binding::OfMouse(e.Button, e.Modifiers);
            return true;
        case InputEventType::MouseWheel:
            out = (e.Wheel > 0.0f) ? Binding::WheelUp() : Binding::WheelDown();
            return true;
        case InputEventType::GamepadButtonPressed:
            out = Binding::OfPadButton(e.PadButton);
            return true;
        default:
            return false;
    }
}

// Подстрока без учёта регистра — тот же приём, что и в поиске других панелей.
inline bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Все клавиши действия одной строкой — для строки списка.
inline std::string BindingSummary(const Action& action) {
    std::string out;
    for (const Binding& b : action.Bindings()) {
        if (!out.empty()) out += "   ";
        out += b.ToString();
    }
    return out;
}

// Оси, которые нельзя поймать нажатием (см. BindingFromEvent). Список общий у
// свойств действия и у модалки создания: один и тот же набор, написанный
// дважды, однажды разойдётся.
struct AxisItem { const char* Label; Binding (*Make)(); };
inline const AxisItem kAxes[] = {
    {"MOUSE_X", [] { return Binding::MouseAxisX(); }},
    {"MOUSE_Y", [] { return Binding::MouseAxisY(); }},
    {"PAD_LEFT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftX); }},
    {"PAD_LEFT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftY); }},
    {"PAD_RIGHT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightX); }},
    {"PAD_RIGHT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightY); }},
    {"PAD_LEFT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftTrigger); }},
    {"PAD_RIGHT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightTrigger); }},
};

} // namespace inputpanel_detail
using namespace inputpanel_detail;
