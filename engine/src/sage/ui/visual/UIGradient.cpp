#include "sage/ui/visual/UIGradient.h"

namespace sage::ui {

const char* const* UIGradientKindNames() {
    static const char* names[] = {"None", "Linear", "Radial", "Angular"};
    return names;
}
int UIGradientKindCount() { return 4; }

UIColor UIGradient::Evaluate(float t) const {
    if (Stops.empty()) return UIColor(1.0f);
    if (Stops.size() == 1) return Stops[0].Color;
    t = UIClamp01(t);
    // За краями — крайние цвета, а не прозрачность: градиент, «затухающий» на
    // границе, почти всегда ошибка ввода, а не замысел.
    if (t <= Stops.front().Position) return Stops.front().Color;
    if (t >= Stops.back().Position) return Stops.back().Color;
    for (size_t i = 1; i < Stops.size(); ++i) {
        const UIGradientStop& a = Stops[i - 1];
        const UIGradientStop& b = Stops[i];
        if (t <= b.Position) {
            const float span = b.Position - a.Position;
            const float k = span > 0.0001f ? (t - a.Position) / span : 0.0f;
            return a.Color + (b.Color - a.Color) * k;
        }
    }
    return Stops.back().Color;
}

UIGradient UIGradient::TwoColor(const UIColor& a, const UIColor& b, float angleDeg) {
    UIGradient g;
    g.Type = Kind::Linear;
    g.Angle = angleDeg;
    g.Stops = {{0.0f, a}, {1.0f, b}};
    return g;
}

} // namespace sage::ui
