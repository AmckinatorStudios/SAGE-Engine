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

// Подписи видов ЗНАЧЕНИЯ. Порядок совпадает с ActionType — на этом держится
// перевод индекса комбо-бокса обратно в вид, и менять его нельзя. «Кнопкой»
// действие не называется: кнопка — физический источник, а не результат.
inline const char* ActionTypeLabel(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("Boolean");
        case ActionType::Axis:    return T("Axis");
        case ActionType::Vector:  return T("Vector2");
    }
    return "";
}

// Пояснение к виду — в подсказке и в окне действия.
inline const char* ActionTypeHint(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("true / false");
        case ActionType::Axis:    return T("A number, -1 .. 1");
        case ActionType::Vector:  return T("Two numbers at once: movement, look");
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

// «Кейкап» — компактная плашка с названием источника. Рисунком, а не кнопкой:
// в строке списка их десятки, и каждая кнопка ImGui — отдельный элемент со
// своим идентификатором. warn — рамка предупреждения (конфликт).
inline float KeycapWidth(const char* text) {
    return ImGui::CalcTextSize(text).x + Sage::UI::Get().PaddingControl * 1.5f;
}
inline void DrawKeycap(ImDrawList* dl, ImVec2 pos, float h, const char* text, bool warn) {
    const float w = KeycapWidth(text);
    const ImU32 line = ImGui::GetColorU32(
        warn ? EditorTheme::Color(EditorTheme::Role::Warn) : EditorTheme::Color(EditorTheme::Role::Line));
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Input)), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), line, 4.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                ImGui::GetColorU32(warn ? EditorTheme::Color(EditorTheme::Role::Warn)
                                        : EditorTheme::Color(EditorTheme::Role::Text)),
                text);
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
