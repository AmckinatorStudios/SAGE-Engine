#include "ui/UIStyle.h"

#include <cmath>

namespace Sage::UI {

namespace {

float& ScaleRef() {
    static float scale = 1.0f;
    return scale;
}

// Токены СЧИТАЮТСЯ ЧИСТОЙ ФУНКЦИЕЙ и возвращаются значением.
//
// Не «функция, которая правит статическую переменную»: такая, вызванная из
// инициализатора той же переменной, входит в саму себя, и программа не падает,
// а ВИСНЕТ на защите статической инициализации. Отлаживать это неприятно —
// редактор просто не доходит до первого кадра.
Style Make(float scale) {
    // ЦЕЛЫЕ ТОЧКИ. Половина точки на границе элемента даёт размытый край, а
    // при 125% таких границ становится большинство — именно из-за них масштаб
    // выглядит «кривым», а не из-за самих размеров.
    auto px = [scale](float v) { return std::floor(v * scale + 0.5f); };
    Style s;

    s.SpacingXS = px(kGrid);          // 4
    s.SpacingSM = px(kGrid * 2);      // 8
    s.SpacingMD = px(kGrid * 3);      // 12
    s.SpacingLG = px(kGrid * 4);      // 16
    s.SpacingXL = px(kGrid * 6);      // 24

    s.PaddingPanel = px(kGrid * 3);   // 12
    s.PaddingControl = px(kGrid * 2); // 8
    s.PaddingControlY = px(kGrid);    // 4 — компактно, но не тесно

    s.CornerRadius = px(kGrid);             // 4
    s.CornerRadiusLarge = px(kGrid * 1.5f); // 6
    s.CornerRadiusSmall = px(kGrid * 0.5f); // 2
    s.BorderWidth = 1.0f;                   // всегда одна точка: 1.25 мылит

    s.ControlHeight = px(kGrid * 6);   // 24 — строка «подпись + поле» без тесноты
    s.ToolbarHeight = px(kGrid * 9);   // 36
    s.HeaderHeight = px(kGrid * 7);    // 28
    s.RowHeight = px(kGrid * 5.5f);    // 22 — плотное дерево иерархии
    s.StatusBarHeight = px(kGrid * 6); // 24

    s.IconSize = px(kGrid * 4);        // 16
    s.IconSizeLarge = px(kGrid * 5);   // 20

    s.LabelColumn = px(kGrid * 22);    // 88 — «Receive Shadows» помещается
    s.ScrollbarSize = px(kGrid * 2.5f);// 10
    s.IndentSpacing = px(kGrid * 4);   // 16

    s.FontDisplay = px(20.0f);
    s.FontTitle = px(15.0f);
    s.FontSection = px(14.0f);
    s.FontBody = px(14.0f);
    s.FontCaption = px(12.0f);
    return s;
}

Style& Data() {
    static Style s = Make(1.0f);
    return s;
}

} // namespace

const Style& Get() { return Data(); }

float Scale() { return ScaleRef(); }

void Rebuild(float scale) {
    ScaleRef() = scale;
    Data() = Make(scale);
}

} // namespace Sage::UI
