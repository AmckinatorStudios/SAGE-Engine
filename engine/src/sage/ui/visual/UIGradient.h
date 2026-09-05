#pragma once
#include <algorithm>
#include <vector>

#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// ГРАДИЕНТ — самостоятельный объект (§24 ТЗ), а не «второй цвет заливки».
//
// Прежняя заливка умела ровно два цвета сверху вниз. Из этого не собирается ни
// одна настоящая тема: стеклянная панель — это три-четыре остановки, металл —
// пять, подсветка кнопки — резкая остановка в середине. И направление у
// градиента всегда было одно, вертикальное.
//
// Здесь градиент — список остановок с произвольным числом цветов, тип (линейный,
// радиальный, угловой) и своя геометрия. Он reusable: один и тот же градиент
// лежит в теме и используется десятком узлов.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct UIGradientStop {
    float Position = 0.0f; // 0..1
    UIColor Color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct UIGradient {
    enum class Kind {
        None,     // градиента нет — заливка плоская
        Linear,
        Radial,
        Angular,
    };

    Kind Type = Kind::None;
    // Для линейного: направление в градусах (0 — сверху вниз, 90 — слева
    // направо). Для радиального/углового: центр в долях прямоугольника.
    float Angle = 0.0f;
    glm::vec2 Center{0.5f, 0.5f};
    float Radius = 0.5f; // для радиального, в долях от меньшей стороны

    std::vector<UIGradientStop> Stops;

    bool Active() const { return Type != Kind::None && Stops.size() >= 2; }

    // Цвет в точке 0..1 вдоль градиента. Линейная интерполяция между
    // соседними остановками; за краями — крайние цвета.
    UIColor Evaluate(float t) const;

    // Готовый градиент из двух цветов — самый частый случай, и ради него не
    // должно приходиться собирать вектор руками.
    static UIGradient TwoColor(const UIColor& a, const UIColor& b, float angleDeg = 0.0f);

    void Sort() {
        std::stable_sort(Stops.begin(), Stops.end(),
                         [](const UIGradientStop& x, const UIGradientStop& y) {
                             return x.Position < y.Position;
                         });
    }
};

const char* const* UIGradientKindNames();
int UIGradientKindCount();

} // namespace sage::ui
