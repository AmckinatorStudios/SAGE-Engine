#include "sage/input/Binding.h"

#include <cctype>

namespace sage::input {

std::string Binding::ToString() const {
    std::string out = ModifiersToString(Modifiers);
    // Минус ведущий, ПОСЛЕ модификаторов: "CTRL+-S" не читается, а "-CTRL+S"
    // читается как «минус на сочетании» — это и имеется в виду.
    std::string body;
    switch (Kind) {
        case SourceKind::Key:           body = KeyName(AsKey()); break;
        case SourceKind::MouseButton:   body = MouseButtonName(AsMouseButton()); break;
        case SourceKind::MouseWheel:    body = WheelIsUp() ? "WHEEL_UP" : "WHEEL_DOWN"; break;
        case SourceKind::MouseAxis:     body = (Code == 0) ? "MOUSE_X" : "MOUSE_Y"; break;
        case SourceKind::GamepadButton: body = GamepadButtonName(AsPadButton()); break;
        case SourceKind::GamepadAxis:   body = GamepadAxisName(AsPadAxis()); break;
        case SourceKind::None:          body = "NONE"; break;
    }
    const std::string sign = (Scale < 0.0f) ? "-" : "";
    return sign + out + body;
}

std::optional<Binding> Binding::Parse(const std::string& text) {
    if (text.empty()) return std::nullopt;

    std::string work = text;
    float scale = 1.0f;
    if (work[0] == '-') {
        scale = -1.0f;
        work = work.substr(1);
    } else if (work[0] == '+') {
        work = work.substr(1);
    }
    if (work.empty()) return std::nullopt;

    std::string body;
    const uint8_t mods = ParseModifiers(work, body);
    if (body.empty()) return std::nullopt;

    Binding b;
    b.Modifiers = mods;
    b.Scale = scale;

    // Порядок проверок — от самого частого к самому редкому, но главное, что
    // словари не пересекаются: имя принадлежит ровно одному виду источника.
    if (const Key key = ParseKey(body); key != Key::Unknown) {
        b.Kind = SourceKind::Key;
        b.Code = (uint16_t)key;
        return b;
    }

    bool ok = false;
    const input::MouseButton mb = ParseMouseButton(body, &ok);
    if (ok) {
        b.Kind = SourceKind::MouseButton;
        b.Code = (uint16_t)mb;
        return b;
    }

    std::string up = body;
    for (char& c : up) c = (char)std::toupper((unsigned char)c);
    if (up == "WHEEL_UP" || up == "SCROLL_UP") {
        b.Kind = SourceKind::MouseWheel; b.Code = 0; return b;
    }
    if (up == "WHEEL_DOWN" || up == "SCROLL_DOWN") {
        b.Kind = SourceKind::MouseWheel; b.Code = 1; return b;
    }
    if (up == "MOUSE_X") { b.Kind = SourceKind::MouseAxis; b.Code = 0; return b; }
    if (up == "MOUSE_Y") { b.Kind = SourceKind::MouseAxis; b.Code = 1; return b; }

    const input::GamepadButton pb = ParseGamepadButton(body, &ok);
    if (ok) {
        b.Kind = SourceKind::GamepadButton;
        b.Code = (uint16_t)pb;
        return b;
    }
    const input::GamepadAxis pa = ParseGamepadAxis(body, &ok);
    if (ok) {
        b.Kind = SourceKind::GamepadAxis;
        b.Code = (uint16_t)pa;
        return b;
    }
    return std::nullopt;
}

} // namespace sage::input
