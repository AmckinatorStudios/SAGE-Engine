#include "sage/ui/UILayoutTools.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sage::ui {

namespace {

// Один кандидат на притяжку по одной оси.
struct Candidate {
    float Delta = 0.0f;             // насколько подвинуть, чтобы совпало
    float Line = 0.0f;              // где пройдёт направляющая
    SnapGuide::Source From = SnapGuide::Source::Grid;
    float SpanBegin = 0.0f, SpanEnd = 0.0f; // отрезок вдоль другой оси
    bool Valid = false;
};

// Лучший из двух: ближе — лучше; при равном расстоянии выигрывает край, а не
// сетка. Иначе на сетке шагом 8 края соседей никогда не побеждали бы: узел
// сетки почти всегда найдётся не дальше края.
bool Better(const Candidate& a, const Candidate& b) {
    if (!b.Valid) return true;
    if (!a.Valid) return false;
    const float da = std::fabs(a.Delta), db = std::fabs(b.Delta);
    if (std::fabs(da - db) > 1e-4f) return da < db;
    return a.From != SnapGuide::Source::Grid && b.From == SnapGuide::Source::Grid;
}

// Проверяет одну пару «подвижная координата — неподвижная линия».
void Consider(Candidate& best, float mover, float line, float threshold,
              SnapGuide::Source from, float spanBegin, float spanEnd) {
    const float delta = line - mover;
    if (std::fabs(delta) > threshold) return;
    Candidate c;
    c.Delta = delta;
    c.Line = line;
    c.From = from;
    c.SpanBegin = spanBegin;
    c.SpanEnd = spanEnd;
    c.Valid = true;
    if (Better(c, best)) best = c;
}

// Отрезок направляющей: от дальнего края одного прямоугольника до дальнего края
// другого — так линия ВИДНО СОЕДИНЯЕТ то, что совпало, а не тянется через весь
// экран мимо всего.
void SpanOf(const UIRect& a, const UIRect& b, bool alongY, float& begin, float& end) {
    if (alongY) {
        begin = std::min(a.y, b.y);
        end = std::max(a.y + a.h, b.y + b.h);
    } else {
        begin = std::min(a.x, b.x);
        end = std::max(a.x + a.w, b.x + b.w);
    }
}

// Подвижные координаты по оси: что именно ведёт мышь.
void MoversX(const UIRect& r, const SnapEdges& e, float out[3], int& count) {
    count = 0;
    if (e.MovesWhole()) {
        out[count++] = r.x;
        out[count++] = r.x + r.w * 0.5f;
        out[count++] = r.x + r.w;
        return;
    }
    if (e.Left) out[count++] = r.x;
    if (e.Right) out[count++] = r.x + r.w;
}

void MoversY(const UIRect& r, const SnapEdges& e, float out[3], int& count) {
    count = 0;
    if (e.MovesWhole()) {
        out[count++] = r.y;
        out[count++] = r.y + r.h * 0.5f;
        out[count++] = r.y + r.h;
        return;
    }
    if (e.Top) out[count++] = r.y;
    if (e.Bottom) out[count++] = r.y + r.h;
}

} // namespace

SnapResult Snap(const UIRect& moving, const SnapEdges& edges, const UIRect& parent,
                const std::vector<UIRect>& siblings, const SnapSettings& settings) {
    SnapResult out;
    if (settings.Threshold <= 0.0f) return out;

    float moversX[3], moversY[3];
    int countX = 0, countY = 0;
    MoversX(moving, edges, moversX, countX);
    MoversY(moving, edges, moversY, countY);

    Candidate bestX, bestY;

    if (settings.ToEdges) {
        // Родитель — такой же источник линий, как соседи: «по центру панели» и
        // «вплотную к её краю» нужны не реже, чем «в ряд с кнопкой».
        auto against = [&](const UIRect& other, SnapGuide::Source from) {
            const float linesX[3] = {other.x, other.x + other.w * 0.5f, other.x + other.w};
            const float linesY[3] = {other.y, other.y + other.h * 0.5f, other.y + other.h};
            float b = 0.0f, e = 0.0f;
            SpanOf(moving, other, true, b, e);
            for (int i = 0; i < countX; ++i)
                for (float line : linesX)
                    Consider(bestX, moversX[i], line, settings.Threshold, from, b, e);
            SpanOf(moving, other, false, b, e);
            for (int i = 0; i < countY; ++i)
                for (float line : linesY)
                    Consider(bestY, moversY[i], line, settings.Threshold, from, b, e);
        };
        against(parent, SnapGuide::Source::Parent);
        for (const UIRect& s : siblings) against(s, SnapGuide::Source::Sibling);
    }

    if (settings.ToGrid && settings.GridStep > 0.0f) {
        // Сетка отсчитывается от начала КАДРА, а не от родителя: рисуется она
        // тоже по кадру, и притяжка обязана совпадать с тем, что видно. Сетка,
        // начинающаяся у каждой панели по-своему, выглядит как сбой.
        const float step = settings.GridStep;
        for (int i = 0; i < countX; ++i) {
            const float line = std::round(moversX[i] / step) * step;
            Consider(bestX, moversX[i], line, settings.Threshold, SnapGuide::Source::Grid,
                     moving.y, moving.y + moving.h);
        }
        for (int i = 0; i < countY; ++i) {
            const float line = std::round(moversY[i] / step) * step;
            Consider(bestY, moversY[i], line, settings.Threshold, SnapGuide::Source::Grid,
                     moving.x, moving.x + moving.w);
        }
    }

    if (bestX.Valid) {
        out.Delta.x = bestX.Delta;
        // Направляющую от сетки не показываем: сетка и так нарисована, а
        // лишняя линия поверх неё читается как «притянулось к чему-то ещё».
        if (bestX.From != SnapGuide::Source::Grid)
            out.Guides.push_back(SnapGuide{SnapGuide::Axis::X, bestX.From, bestX.Line,
                                           bestX.SpanBegin, bestX.SpanEnd});
    }
    if (bestY.Valid) {
        out.Delta.y = bestY.Delta;
        if (bestY.From != SnapGuide::Source::Grid)
            out.Guides.push_back(SnapGuide{SnapGuide::Axis::Y, bestY.From, bestY.Line,
                                           bestY.SpanBegin, bestY.SpanEnd});
    }
    return out;
}

bool AlignIsHorizontal(AlignEdge edge) {
    return edge == AlignEdge::Left || edge == AlignEdge::CenterX || edge == AlignEdge::Right;
}

glm::vec2 AlignDelta(const UIRect& r, const UIRect& target, AlignEdge edge) {
    switch (edge) {
        case AlignEdge::Left:    return {target.x - r.x, 0.0f};
        case AlignEdge::CenterX: return {(target.x + target.w * 0.5f) - (r.x + r.w * 0.5f), 0.0f};
        case AlignEdge::Right:   return {(target.x + target.w) - (r.x + r.w), 0.0f};
        case AlignEdge::Top:     return {0.0f, target.y - r.y};
        case AlignEdge::CenterY: return {0.0f, (target.y + target.h * 0.5f) - (r.y + r.h * 0.5f)};
        case AlignEdge::Bottom:  return {0.0f, (target.y + target.h) - (r.y + r.h)};
    }
    return {0.0f, 0.0f};
}

namespace {

// Порядок прямоугольников вдоль оси. Распределять надо в том порядке, в каком
// они СТОЯТ, а не в каком их выделили: иначе «поровну» переставит их местами.
std::vector<size_t> OrderAlong(const std::vector<UIRect>& rects, bool horizontal) {
    std::vector<size_t> order(rects.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const float ca = horizontal ? rects[a].x + rects[a].w * 0.5f
                                    : rects[a].y + rects[a].h * 0.5f;
        const float cb = horizontal ? rects[b].x + rects[b].w * 0.5f
                                    : rects[b].y + rects[b].h * 0.5f;
        return ca < cb;
    });
    return order;
}

} // namespace

std::vector<float> DistributeGapDeltas(const std::vector<UIRect>& rects, bool horizontal) {
    std::vector<float> deltas(rects.size(), 0.0f);
    if (rects.size() < 3) return deltas;

    const std::vector<size_t> order = OrderAlong(rects, horizontal);
    auto start = [&](size_t i) { return horizontal ? rects[i].x : rects[i].y; };
    auto size = [&](size_t i) { return horizontal ? rects[i].w : rects[i].h; };

    const size_t first = order.front(), last = order.back();
    const float from = start(first);
    const float to = start(last) + size(last);

    float occupied = 0.0f;
    for (size_t i : order) occupied += size(i);
    const float gap = (to - from - occupied) / (float)(order.size() - 1);

    float cursor = from;
    for (size_t i : order) {
        deltas[i] = cursor - start(i);
        cursor += size(i) + gap;
    }
    return deltas;
}

std::vector<float> DistributeCenterDeltas(const std::vector<UIRect>& rects, bool horizontal) {
    std::vector<float> deltas(rects.size(), 0.0f);
    if (rects.size() < 3) return deltas;

    const std::vector<size_t> order = OrderAlong(rects, horizontal);
    auto center = [&](size_t i) {
        return horizontal ? rects[i].x + rects[i].w * 0.5f : rects[i].y + rects[i].h * 0.5f;
    };

    const float from = center(order.front());
    const float to = center(order.back());
    const float step = (to - from) / (float)(order.size() - 1);

    for (size_t k = 0; k < order.size(); ++k) {
        const size_t i = order[k];
        deltas[i] = (from + step * (float)k) - center(i);
    }
    return deltas;
}

UIRect Union(const std::vector<UIRect>& rects) {
    if (rects.empty()) return UIRect{};
    float x0 = rects[0].x, y0 = rects[0].y;
    float x1 = rects[0].x + rects[0].w, y1 = rects[0].y + rects[0].h;
    for (const UIRect& r : rects) {
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    return UIRect{x0, y0, x1 - x0, y1 - y0};
}

} // namespace sage::ui
