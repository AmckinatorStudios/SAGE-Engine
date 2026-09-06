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

UIDesignTokens UIDesignTokens::Editor() {
    UIDesignTokens t;

    // ЦВЕТ. Три ступени тёмного вместо одной: фон приложения (то, что видно
    // между панелями), поверхность панели и приподнятое внутри неё — полосы
    // вкладок, тулбары, шапки. Без трёх ступеней панели сливаются в одно
    // пятно, и границы приходится рисовать линиями погуще, то есть шумом.
    t.SetColor("Color.Background", UIColorFromHex("#0D0E10"));
    t.SetColor("Color.Surface", UIColorFromHex("#17181B"));
    t.SetColor("Color.SurfaceRaised", UIColorFromHex("#1F2125"));
    // Разделитель тонкий и тёмный: его задача — обозначить край, а не быть
    // заметным. Заметные линии складываются в решётку, и взгляд цепляется за
    // неё вместо содержимого.
    t.SetColor("Color.Border", UIColorFromHex("#2A2C31"));
    t.SetColor("Color.Text", UIColorFromHex("#E6E8EB"));
    t.SetColor("Color.TextMuted", UIColorFromHex("#8A8E96"));
    // Золото SAGE. Только для того, что человек выбрал или вот-вот нажмёт:
    // активная вкладка, выделенная строка, Play, наведение. Жёлтого на экране
    // должно быть мало — иначе он перестаёт значить «сюда смотри».
    t.SetColor("Color.Accent", UIColorFromHex("#F0B429"));
    t.SetColor("Color.AccentText", UIColorFromHex("#141005"));
    // Подложка выделенной строки: акцент, приглушённый до фона. Заливать
    // строку самим акцентом нельзя — жёлтая полоса перекрикивает текст на ней.
    t.SetColor("Color.AccentMuted", UIColorFromHex("#2C2620"));
    t.SetColor("Color.Positive", UIColorFromHex("#4FB865"));
    t.SetColor("Color.Warning", UIColorFromHex("#D9A032"));
    t.SetColor("Color.Danger", UIColorFromHex("#D95757"));
    t.SetColor("Color.Shadow", UIColor(0.0f, 0.0f, 0.0f, 0.55f));

    // ПЛОТНОСТЬ. Отступы примерно вдвое меньше игровых: на экране редактора
    // одновременно полторы сотни строк, и каждый лишний пиксель отступа — это
    // минус строка в списке.
    t.SetNumber("Spacing.Tiny", 2.0f);
    t.SetNumber("Spacing.Small", 4.0f);
    t.SetNumber("Spacing.Medium", 8.0f);
    t.SetNumber("Spacing.Large", 12.0f);
    t.SetNumber("Spacing.Huge", 20.0f);

    // Скругления почти прямые. Круглые углы съедают место на стыках и делают
    // плотную сетку панелей рыхлой.
    t.SetNumber("Radius.Small", 3.0f);
    t.SetNumber("Radius.Medium", 4.0f);
    t.SetNumber("Radius.Large", 6.0f);
    t.SetNumber("Radius.Pill", 999.0f);

    t.SetNumber("FontSize.Caption", 12.0f);
    t.SetNumber("FontSize.Body", 13.0f);
    t.SetNumber("FontSize.Title", 15.0f);
    t.SetNumber("FontSize.Display", 20.0f);

    // Высота строки списка и высота поля — одно число на весь редактор. Пока
    // каждый список выбирал её сам, соседние панели стояли «в разлинейку».
    t.SetNumber("Size.Row", 22.0f);
    t.SetNumber("Size.Control", 22.0f);
    t.SetNumber("Size.IconButton", 26.0f);
    t.SetNumber("Size.Toolbar", 34.0f);
    t.SetNumber("Size.MenuBar", 28.0f);
    t.SetNumber("Size.StatusBar", 24.0f);
    t.SetNumber("Size.Tab", 26.0f);
    return t;
}

} // namespace sage::ui
