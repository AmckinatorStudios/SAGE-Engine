#include "sage/ui/style/UIDesignTokens.h"

namespace sage::ui {

UIColor UIDesignTokens::Color(const std::string& name, const UIColor& fallback) const {
    auto it = m_colors.find(name);
    return it == m_colors.end() ? fallback : it->second;
}
float UIDesignTokens::Number(const std::string& name, float fallback) const {
    auto it = m_numbers.find(name);
    return it == m_numbers.end() ? fallback : it->second;
}
std::string UIDesignTokens::String(const std::string& name, const std::string& fallback) const {
    auto it = m_strings.find(name);
    return it == m_strings.end() ? fallback : it->second;
}
bool UIDesignTokens::Has(const std::string& name) const {
    return m_colors.count(name) || m_numbers.count(name) || m_strings.count(name);
}
void UIDesignTokens::Clear() {
    m_colors.clear();
    m_numbers.clear();
    m_strings.clear();
}

UIDesignTokens UIDesignTokens::Default() {
    // Не «дизайн движка», а рабочее начало: нейтральная тёмная палитра, шкала
    // отступов, радиусов и кеглей. Игра заменяет тему целиком, и ни один
    // компонент на эти имена не завязан жёстко.
    UIDesignTokens t;
    t.SetColor("Color.Background", UIColorFromHex("#12141A"));
    t.SetColor("Color.Surface", UIColorFromHex("#1A1D25"));
    t.SetColor("Color.SurfaceRaised", UIColorFromHex("#232833"));
    t.SetColor("Color.Border", UIColorFromHex("#333A47"));
    t.SetColor("Color.Text", UIColorFromHex("#ECEFF4"));
    t.SetColor("Color.TextMuted", UIColorFromHex("#9AA3B2"));
    t.SetColor("Color.Accent", UIColorFromHex("#F2C230"));
    t.SetColor("Color.AccentText", UIColorFromHex("#1A1200"));
    t.SetColor("Color.Positive", UIColorFromHex("#5CBF6B"));
    t.SetColor("Color.Warning", UIColorFromHex("#E0A63A"));
    t.SetColor("Color.Danger", UIColorFromHex("#E05C5C"));
    t.SetColor("Color.Shadow", UIColor(0.0f, 0.0f, 0.0f, 0.45f));

    t.SetNumber("Spacing.Tiny", 4.0f);
    t.SetNumber("Spacing.Small", 8.0f);
    t.SetNumber("Spacing.Medium", 16.0f);
    t.SetNumber("Spacing.Large", 24.0f);
    t.SetNumber("Spacing.Huge", 40.0f);

    t.SetNumber("Radius.Small", 4.0f);
    t.SetNumber("Radius.Medium", 10.0f);
    t.SetNumber("Radius.Large", 18.0f);
    t.SetNumber("Radius.Pill", 999.0f);

    t.SetNumber("FontSize.Caption", 13.0f);
    t.SetNumber("FontSize.Body", 17.0f);
    t.SetNumber("FontSize.Title", 24.0f);
    t.SetNumber("FontSize.Display", 40.0f);

    t.SetNumber("Opacity.Disabled", 0.45f);
    t.SetNumber("Duration.Fast", 0.12f);
    t.SetNumber("Duration.Normal", 0.22f);
    return t;
}

} // namespace sage::ui
