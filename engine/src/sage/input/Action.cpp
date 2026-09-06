#include "sage/input/Action.h"

#include <algorithm>
#include <cmath>

namespace sage::input {

namespace {

// Ось из ДВУХ клавиш собирается вычитанием, и складывать вклады надо с
// насыщением: зажатые одновременно W и S дают ноль (человек не едет в обе
// стороны сразу), а не «победила последняя».
float Clamp1(float v) { return std::max(-1.0f, std::min(1.0f, v)); }

// Какому устройству принадлежит привязка.
uint8_t DeviceOf(const Binding& b) {
    switch (b.Kind) {
        case SourceKind::Key: return DeviceKeyboard;
        case SourceKind::MouseButton:
        case SourceKind::MouseWheel:
        case SourceKind::MouseAxis: return DeviceMouse;
        case SourceKind::GamepadButton:
        case SourceKind::GamepadAxis: return DeviceGamepad;
        case SourceKind::None: return DeviceNone;
    }
    return DeviceNone;
}

} // namespace

bool Action::Bind(const std::string& source) {
    if (std::optional<Binding> b = Binding::Parse(source)) {
        m_bindings.push_back(*b);
        return true;
    }
    return false;
}

Action& Action::BindVector(const std::string& up, const std::string& down,
                           const std::string& left, const std::string& right) {
    // Y — «вперёд», X — «вправо»: конвенция движка (мир, а не экран), та же,
    // что у смещения мыши. Знак задаётся здесь ОДИН раз, чтобы каждая игра не
    // вспоминала, в какую сторону смотрит S.
    if (std::optional<Binding> b = Binding::Parse(up)) m_bindings.push_back(b->With(1.0f).On(Component::Y));
    if (std::optional<Binding> b = Binding::Parse(down)) m_bindings.push_back(b->With(-1.0f).On(Component::Y));
    if (std::optional<Binding> b = Binding::Parse(left)) m_bindings.push_back(b->With(-1.0f).On(Component::X));
    if (std::optional<Binding> b = Binding::Parse(right)) m_bindings.push_back(b->With(1.0f).On(Component::X));
    return *this;
}

bool Action::Rebind(const std::string& source) {
    if (std::optional<Binding> b = Binding::Parse(source)) {
        Rebind(*b);
        return true;
    }
    return false;
}

bool Action::Unbind(const Binding& binding) {
    const size_t before = m_bindings.size();
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
                                    [&](const Binding& b) { return b.SameSource(binding); }),
                     m_bindings.end());
    return m_bindings.size() != before;
}

bool Action::UsesSource(const Binding& binding) const {
    for (const Binding& b : m_bindings)
        if (b.SameSource(binding)) return true;
    return false;
}

Phase Action::CurrentPhase() const {
    if (m_released) return Phase::ReleasedThisFrame;
    if (m_pressed) return Phase::Pressed;
    if (m_down) return Phase::Held;
    return Phase::Released;
}

bool Action::ModifiersMatch(const Binding& b, const Devices& d) const {
    const uint8_t held = d.Keys().Modifiers();
    if ((held & b.Modifiers) != b.Modifiers) return false;
    // Точное совпадение — только там, где его попросили: см. комментарий у
    // ActionSettings::ExactModifiers (иначе бег ломал бы ходьбу).
    if (m_settings.ExactModifiers && held != b.Modifiers) return false;
    return true;
}

bool Action::SourceDown(const Binding& b, const Devices& d, uint8_t blocked) const {
    switch (b.Kind) {
        case SourceKind::Key:
            if (blocked & DeviceKeyboard) return false;
            return d.Keys().Down(b.AsKey()) && ModifiersMatch(b, d);
        case SourceKind::MouseButton:
            if (blocked & DeviceMouse) return false;
            return d.MouseState().Down(b.AsMouseButton()) && ModifiersMatch(b, d);
        case SourceKind::MouseWheel: {
            if (blocked & DeviceMouse) return false;
            const float wheel = d.MouseState().Wheel();
            // Тик колеса «нажат» ровно тот кадр, в котором он пришёл: колесо
            // физически некуда держать зажатым.
            return b.WheelIsUp() ? (wheel > 0.0f) : (wheel < 0.0f);
        }
        case SourceKind::MouseAxis:
        case SourceKind::GamepadAxis: {
            const float v = SourceValue(b, d, blocked);
            return std::fabs(v) >= m_settings.PressThreshold;
        }
        case SourceKind::GamepadButton: {
            if (blocked & DeviceGamepad) return false;
            const int pad = (b.Gamepad >= 0) ? (int)b.Gamepad : d.FirstConnected();
            if (pad < 0) return false;
            return d.Pad(pad).Down(b.AsPadButton());
        }
        case SourceKind::None:
            return false;
    }
    return false;
}

float Action::SourceValue(const Binding& b, const Devices& d, uint8_t blocked) const {
    switch (b.Kind) {
        case SourceKind::MouseAxis: {
            if (blocked & DeviceMouse) return 0.0f;
            const glm::vec2 delta = d.MouseState().Delta();
            return (b.Code == 0) ? delta.x : delta.y;
        }
        case SourceKind::GamepadAxis: {
            if (blocked & DeviceGamepad) return 0.0f;
            const int pad = (b.Gamepad >= 0) ? (int)b.Gamepad : d.FirstConnected();
            if (pad < 0) return 0.0f;
            return d.Pad(pad).Axis(b.AsPadAxis());
        }
        default:
            // Цифровой источник в аналоговом действии — это и есть «W даёт +1».
            return SourceDown(b, d, blocked) ? 1.0f : 0.0f;
    }
}

glm::vec2 Action::ApplyDeadZone(glm::vec2 raw) const {
    const float dz = std::max(0.0f, std::min(0.95f, m_settings.DeadZone));
    if (dz <= 0.0f) return raw;

    if (m_settings.Zone == DeadZoneMode::Radial) {
        const float len = std::sqrt(raw.x * raw.x + raw.y * raw.y);
        if (len <= dz) return glm::vec2(0.0f);
        // Растягиваем остаток обратно на весь диапазон: иначе максимум стика
        // оказывается меньше единицы, и персонаж не выходит на полную скорость.
        const float scaled = (len - dz) / (1.0f - dz);
        return raw * (std::min(scaled, 1.0f) / len);
    }

    glm::vec2 out = raw;
    for (int i = 0; i < 2; ++i) {
        float& v = (i == 0) ? out.x : out.y;
        const float a = std::fabs(v);
        if (a <= dz) { v = 0.0f; continue; }
        v = (v < 0.0f ? -1.0f : 1.0f) * std::min((a - dz) / (1.0f - dz), 1.0f);
    }
    return out;
}

void Action::Evaluate(const Devices& devices, float dt, uint8_t blocked) {
    const bool wasDown = m_down;

    // --- Цифровая половина: нажато ли хоть что-нибудь из привязанного -------
    //
    // Без досрочного выхода: маска задействованных устройств должна быть
    // полной, иначе потребление отберёт у нижнего слоя не то устройство.
    bool down = false;
    m_activeDevices = DeviceNone;
    for (const Binding& b : m_bindings) {
        if (!SourceDown(b, devices, blocked)) continue;
        down = true;
        m_activeDevices |= DeviceOf(b);
    }

    m_down = down;
    m_pressed = down && !wasDown;
    m_released = !down && wasDown;

    if (down) {
        m_heldTime = wasDown ? m_heldTime + dt : 0.0f;
    }

    // --- Режим срабатывания (§23) ------------------------------------------
    m_triggered = false;
    switch (m_settings.Trigger) {
        case TriggerMode::Press:
            m_triggered = m_pressed;
            break;
        case TriggerMode::Release:
            m_triggered = m_released;
            break;
        case TriggerMode::Hold:
            // Один раз за удержание, а не каждый кадр после порога: «удержали»
            // — это событие, а не состояние.
            if (down && !m_holdFired && m_heldTime >= m_settings.HoldTime) {
                m_triggered = true;
                m_holdFired = true;
            }
            break;
        case TriggerMode::Tap:
            m_triggered = m_released && m_heldTime <= m_settings.TapTime;
            break;
    }
    if (!down) m_holdFired = false;
    if (!down) m_heldTime = 0.0f;

    // --- Аналоговая половина ------------------------------------------------
    if (m_type == ActionType::Digital) {
        m_value = glm::vec2(down ? 1.0f : 0.0f, 0.0f);
        m_smoothed = m_value;
        return;
    }

    // Цифровые и аналоговые вклады считаются РАЗДЕЛЬНО и только потом
    // складываются: цифровые насыщаются в ±1 (W и S вместе дают ноль), а
    // аналоговые проходят мёртвую зону — применить её к «W = 1.0» значило бы
    // резать клавиатуру порогом, придуманным для дрожащего стика.
    glm::vec2 digital(0.0f);
    glm::vec2 analog(0.0f);
    for (const Binding& b : m_bindings) {
        const float v = SourceValue(b, devices, blocked) * b.Scale;
        if (v == 0.0f) continue;
        m_activeDevices |= DeviceOf(b);
        glm::vec2& target = b.IsAnalog() ? analog : digital;
        if (b.Axis == Component::Y) target.y += v; else target.x += v;
    }
    digital = glm::vec2(Clamp1(digital.x), Clamp1(digital.y));
    analog = ApplyDeadZone(analog);

    glm::vec2 value = digital + analog;
    if (m_type == ActionType::Axis) value.y = 0.0f;

    if (m_settings.Normalize) {
        const float len = std::sqrt(value.x * value.x + value.y * value.y);
        if (len > 1.0f) value /= len;
    }
    value *= m_settings.Sensitivity;

    // Сглаживание — приближение к цели за Smoothing секунд, независимое от
    // частоты кадров: коэффициент, привязанный к кадру, на 144 Гц сглаживает
    // втрое сильнее, чем на 48, и настройка чувствительности перестаёт значить
    // что-либо конкретное.
    if (m_settings.Smoothing > 0.0f && dt > 0.0f) {
        const float k = std::min(1.0f, dt / m_settings.Smoothing);
        m_smoothed += (value - m_smoothed) * k;
        m_value = m_smoothed;
    } else {
        m_smoothed = value;
        m_value = value;
    }
}

void Action::Reset() {
    m_released = m_down;
    m_activeDevices = DeviceNone;   // честное отпускание, а не молчаливое обнуление
    m_down = false;
    m_pressed = false;
    m_triggered = false;
    m_holdFired = false;
    m_heldTime = 0.0f;
    m_value = glm::vec2(0.0f);
    m_smoothed = glm::vec2(0.0f);
}

} // namespace sage::input
