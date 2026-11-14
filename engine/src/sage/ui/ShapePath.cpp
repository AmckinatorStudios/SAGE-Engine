#include "sage/ui/ShapePath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "sage/ui/ShapeGeometry.h"

namespace sage::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

glm::vec2 Map(const UIRect& r, glm::vec2 p) { return {r.x + p.x * r.w, r.y + p.y * r.h}; }

std::string Num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s = buf;
    // Без хвостовых нулей: «0.5», а не «0.5000» — путь читают и правят руками.
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

bool InTriangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const float d1 = Cross(b - a, p - a), d2 = Cross(c - b, p - b), d3 = Cross(a - c, p - c);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

bool PointInRing(const std::vector<glm::vec2>& ring, glm::vec2 p) {
    bool in = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const glm::vec2 a = ring[i], b = ring[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

bool SegmentsCross(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    const float d1 = Cross(b - a, c - a), d2 = Cross(b - a, d - a);
    const float d3 = Cross(d - c, a - c), d4 = Cross(d - c, b - c);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

// Ушная обрезка простого многоугольника (по часовой при оси Y вниз).
void EarClip(std::vector<glm::vec2> poly, std::vector<glm::vec2>& out) {
    if (SignedArea(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
    std::vector<size_t> idx(poly.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < poly.size() * poly.size() + 16) {
        bool cut = false;
        for (size_t k = 0; k < idx.size(); ++k) {
            const glm::vec2 a = poly[idx[(k + idx.size() - 1) % idx.size()]];
            const glm::vec2 b = poly[idx[k]];
            const glm::vec2 c = poly[idx[(k + 1) % idx.size()]];
            if (Cross(b - a, c - b) <= 0.0f) continue;   // вогнутая вершина — не ухо
            bool empty = true;
            for (size_t m = 0; m < idx.size() && empty; ++m) {
                const glm::vec2 q = poly[idx[m]];
                if (q == a || q == b || q == c) continue;
                if (InTriangle(q, a, b, c)) empty = false;
            }
            if (!empty) continue;
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            idx.erase(idx.begin() + (std::ptrdiff_t)k);
            cut = true;
            break;
        }
        // Ушей нет — контур сам себя пересекает или вырожден: заливаем
        // оставшееся веером, чем бросать дыру посреди формы.
        if (!cut) break;
    }
    for (size_t k = 1; k + 1 < idx.size(); ++k) {
        out.push_back(poly[idx[0]]);
        out.push_back(poly[idx[k]]);
        out.push_back(poly[idx[k + 1]]);
    }
}

// Отверстие врезается в внешний контур «мостом»: от самой правой точки
// отверстия к ближайшей видимой точке контура и обратно. Получается один
// простой многоугольник, который режется ушами.
void BridgeHole(std::vector<glm::vec2>& outer, std::vector<glm::vec2> hole,
                const std::vector<std::vector<glm::vec2>>& others) {
    if (SignedArea(hole) > 0.0f) std::reverse(hole.begin(), hole.end());   // против хода внешнего
    size_t hi = 0;
    for (size_t i = 1; i < hole.size(); ++i)
        if (hole[i].x > hole[hi].x) hi = i;
    const glm::vec2 m = hole[hi];
    size_t best = 0;
    float bestD = 1e30f;
    auto blocked = [&](glm::vec2 a, glm::vec2 b) {
        auto test = [&](const std::vector<glm::vec2>& ring) {
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
                if (SegmentsCross(a, b, ring[j], ring[i])) return true;
            return false;
        };
        if (test(outer) || test(hole)) return true;
        for (const auto& o : others)
            if (test(o)) return true;
        return false;
    };
    for (size_t i = 0; i < outer.size(); ++i) {
        const float d = glm::length(outer[i] - m);
        if (d < bestD && !blocked(m, outer[i])) {
            bestD = d;
            best = i;
        }
    }
    std::vector<glm::vec2> merged(outer.begin(), outer.begin() + (std::ptrdiff_t)best + 1);
    for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(hi + k) % hole.size()]);
    merged.insert(merged.end(), outer.begin() + (std::ptrdiff_t)best, outer.end());
    outer = std::move(merged);
}

} // namespace

float SignedArea(const std::vector<glm::vec2>& ring) {
    float a = 0.0f;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) a += Cross(ring[j], ring[i]);
    return a * 0.5f;
}

bool ParseShapePath(const std::string& text, std::vector<PathContour>& out) {
    out.clear();
    std::string clean = text;
    for (char& c : clean)
        if (c == ',') c = ' ';
    std::istringstream in(clean);
    std::string tok;
    PathContour* cur = nullptr;
    auto num = [&](float& v) { return static_cast<bool>(in >> v); };
    while (in >> tok) {
        if (tok == "M") {
            out.push_back({});
            cur = &out.back();
            cur->Closed = false;
            PathPoint p;
            if (!num(p.P.x) || !num(p.P.y)) return false;
            cur->Points.push_back(p);
        } else if (tok == "L" && cur) {
            PathPoint p;
            if (!num(p.P.x) || !num(p.P.y)) return false;
            cur->Points.push_back(p);
        } else if (tok == "C" && cur && !cur->Points.empty()) {
            glm::vec2 c1, c2;
            PathPoint p;
            if (!num(c1.x) || !num(c1.y) || !num(c2.x) || !num(c2.y) || !num(p.P.x) || !num(p.P.y)) return false;
            cur->Points.back().Out = c1 - cur->Points.back().P;
            p.In = c2 - p.P;
            cur->Points.push_back(p);
        } else if (tok == "Z" && cur) {
            cur->Closed = true;
            // Кривая, замыкающая контур, приходит в первую точку — это не
            // новая точка, а ручка входа первой.
            if (cur->Points.size() > 2 && glm::length(cur->Points.back().P - cur->Points.front().P) < 1e-5f) {
                cur->Points.front().In = cur->Points.back().In;
                cur->Points.pop_back();
            }
            cur = nullptr;
        } else {
            return false;
        }
    }
    return true;
}

std::string FormatShapePath(const std::vector<PathContour>& contours) {
    std::string s;
    auto pt = [&](glm::vec2 p) { s += Num(p.x) + " " + Num(p.y); };
    auto seg = [&](const PathPoint& a, const PathPoint& b) {
        if (a.Out == glm::vec2(0.0f) && b.In == glm::vec2(0.0f)) {
            s += " L ";
        } else {
            s += " C ";
            pt(a.P + a.Out);
            s += " ";
            pt(b.P + b.In);
            s += " ";
        }
        pt(b.P);
    };
    for (const PathContour& c : contours) {
        if (c.Points.empty()) continue;
        if (!s.empty()) s += " ";
        s += "M ";
        pt(c.Points[0].P);
        for (size_t i = 1; i < c.Points.size(); ++i) seg(c.Points[i - 1], c.Points[i]);
        if (c.Closed) {
            const PathPoint& last = c.Points.back();
            const PathPoint& first = c.Points.front();
            if (c.Points.size() > 1 && (last.Out != glm::vec2(0.0f) || first.In != glm::vec2(0.0f)))
                seg(last, first);
            s += " Z";
        }
    }
    return s;
}

std::vector<Polyline> FlattenShapePath(const std::vector<PathContour>& contours, const UIRect& r) {
    std::vector<Polyline> out;
    for (const PathContour& c : contours) {
        if (c.Points.size() < 2) continue;
        Polyline line;
        line.Closed = c.Closed;
        const size_t segs = c.Closed ? c.Points.size() : c.Points.size() - 1;
        for (size_t i = 0; i < segs; ++i) {
            const PathPoint& a = c.Points[i];
            const PathPoint& b = c.Points[(i + 1) % c.Points.size()];
            const glm::vec2 p0 = Map(r, a.P), p3 = Map(r, b.P);
            line.Points.push_back(p0);
            if (a.Out == glm::vec2(0.0f) && b.In == glm::vec2(0.0f)) continue;
            const glm::vec2 p1 = Map(r, a.P + a.Out), p2 = Map(r, b.P + b.In);
            // Отрезков столько, чтобы каждый был в несколько пикселей.
            const float len = glm::length(p1 - p0) + glm::length(p2 - p1) + glm::length(p3 - p2);
            const int n = std::clamp((int)std::ceil(len / 4.0f), 4, 64);
            for (int k = 1; k < n; ++k) {
                const float t = (float)k / (float)n, u = 1.0f - t;
                line.Points.push_back(u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 +
                                      t * t * t * p3);
            }
        }
        if (!c.Closed) line.Points.push_back(Map(r, c.Points.back().P));
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::vector<glm::vec2>> ShapeRings(const Shape& shape, const UIRect& r) {
    if (shape.Form != Shape::Geometry::Custom)
        return {ShapeOutline(shape.Form, shape.CornerRadius, r)};
    std::vector<PathContour> contours;
    ParseShapePath(shape.Path, contours);
    if (contours.empty()) return {ShapeOutline(Shape::Geometry::Rectangle, 0.0f, r)};
    std::vector<std::vector<glm::vec2>> rings;
    for (Polyline& l : FlattenShapePath(contours, r))
        if (l.Closed && l.Points.size() >= 3) rings.push_back(std::move(l.Points));
    return rings;
}

std::vector<glm::vec2> TriangulateRings(const std::vector<std::vector<glm::vec2>>& rings) {
    std::vector<glm::vec2> out;
    // Глубина вложенности: чётная — область, нечётная — отверстие в
    // ближайшем охватывающем контуре.
    const size_t n = rings.size();
    std::vector<int> depth(n, 0);
    std::vector<int> parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (rings[i].size() < 3) continue;
        float smallest = 1e30f;
        for (size_t j = 0; j < n; ++j) {
            if (i == j || rings[j].size() < 3 || !PointInRing(rings[j], rings[i][0])) continue;
            ++depth[i];
            const float area = std::abs(SignedArea(rings[j]));
            if (area < smallest) {
                smallest = area;
                parent[i] = (int)j;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (rings[i].size() < 3 || depth[i] % 2 != 0) continue;
        std::vector<glm::vec2> outer = rings[i];
        if (SignedArea(outer) < 0.0f) std::reverse(outer.begin(), outer.end());
        std::vector<std::vector<glm::vec2>> holes;
        for (size_t j = 0; j < n; ++j)
            if (parent[j] == (int)i && depth[j] % 2 == 1) holes.push_back(rings[j]);
        // Сначала — отверстия правее: их мосты не перегородят путь левым.
        std::sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
            float ax = -1e30f, bx = -1e30f;
            for (glm::vec2 p : a) ax = std::max(ax, p.x);
            for (glm::vec2 p : b) bx = std::max(bx, p.x);
            return ax > bx;
        });
        for (size_t h = 0; h < holes.size(); ++h) {
            std::vector<std::vector<glm::vec2>> rest(holes.begin() + (std::ptrdiff_t)h + 1, holes.end());
            BridgeHole(outer, holes[h], rest);
        }
        EarClip(std::move(outer), out);
    }
    return out;
}

std::vector<glm::vec2> StrokeTriangles(const std::vector<glm::vec2>& src, bool closed, float o0, float o1,
                                       Shape::Join join, Shape::Cap cap) {
    std::vector<glm::vec2> out;
    std::vector<glm::vec2> pts;
    for (glm::vec2 p : src)
        if (pts.empty() || glm::length(p - pts.back()) > 1e-4f) pts.push_back(p);
    if (closed && pts.size() > 2 && glm::length(pts.front() - pts.back()) < 1e-4f) pts.pop_back();
    if (pts.size() < 2 || o1 <= o0) return out;
    const size_t n = pts.size();
    const size_t segs = closed ? n : n - 1;
    auto dir = [&](size_t i) { return glm::normalize(pts[(i + 1) % n] - pts[i]); };
    auto left = [](glm::vec2 d) { return glm::vec2(d.y, -d.x); };   // ось Y вниз
    auto tri = [&](glm::vec2 a, glm::vec2 b, glm::vec2 c) {
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    };
    const float half = std::max(std::abs(o0), std::abs(o1));
    // Квадратные концы удлиняют крайние отрезки на полтолщины.
    glm::vec2 first = pts.front(), last = pts.back();
    if (!closed && cap == Shape::Cap::Square) {
        first -= dir(0) * half;
        last += dir(n - 2) * half;
    }
    for (size_t i = 0; i < segs; ++i) {
        const glm::vec2 d = dir(i), nrm = left(d);
        glm::vec2 a = pts[i], b = pts[(i + 1) % n];
        if (!closed && i == 0) a = first;
        if (!closed && i == segs - 1) b = last;
        const glm::vec2 a0 = a + nrm * o0, a1 = a + nrm * o1, b0 = b + nrm * o0, b1 = b + nrm * o1;
        tri(a0, a1, b1);
        tri(a0, b1, b0);
    }
    // Соединения: на ВНЕШНЕЙ стороне поворота между полосами остаётся щель.
    const size_t joints = closed ? n : n - 2;
    for (size_t k = 0; k < joints; ++k) {
        const size_t i = closed ? k : k + 1;
        const glm::vec2 dPrev = dir((i + n - 1) % n), dNext = dir(i);
        const glm::vec2 nPrev = left(dPrev), nNext = left(dNext);
        const float turn = Cross(dPrev, dNext);
        if (std::abs(turn) < 1e-5f && glm::dot(dPrev, dNext) > 0.0f) continue;
        // Поворот к левой нормали — внешняя сторона справа (отрицательный сдвиг).
        const float side = glm::dot(dNext, nPrev) > 0.0f ? -1.0f : 1.0f;
        const float r = side > 0.0f ? o1 : -o0;
        if (r <= 0.0f) continue;
        const glm::vec2 p = pts[i];
        const glm::vec2 e0 = p + nPrev * side * r, e1 = p + nNext * side * r;
        if (join == Shape::Join::Round) {
            const float a0 = std::atan2((e0 - p).y, (e0 - p).x);
            float a1 = std::atan2((e1 - p).y, (e1 - p).x);
            float da = a1 - a0;
            while (da > kPi) da -= 2.0f * kPi;
            while (da < -kPi) da += 2.0f * kPi;
            const int steps = std::max(2, (int)std::ceil(std::abs(da) * r / 3.0f));
            glm::vec2 prev = e0;
            for (int s = 1; s <= steps; ++s) {
                const float a = a0 + da * (float)s / (float)steps;
                const glm::vec2 q = p + glm::vec2(std::cos(a), std::sin(a)) * r;
                tri(p, prev, q);
                prev = q;
            }
        } else {
            tri(p, e0, e1);
            if (join == Shape::Join::Miter) {
                // Острие — пересечение внешних краёв; слишком длинное (острый
                // угол) обрезается фаской, иначе шип уходит на полэкрана.
                const glm::vec2 bis = glm::normalize(nPrev + nNext) * side;
                const float cosHalf = glm::dot(bis, nPrev * side);
                if (cosHalf > 0.25f) tri(e0, p + bis * (r / cosHalf), e1);
            }
        }
    }
    // Круглые концы открытого контура.
    if (!closed && cap == Shape::Cap::Round) {
        for (int end = 0; end < 2; ++end) {
            const glm::vec2 c = end == 0 ? pts.front() : pts.back();
            const glm::vec2 d = end == 0 ? -dir(0) : dir(n - 2);
            const float base = std::atan2(d.y, d.x);
            const int steps = std::max(4, (int)std::ceil(kPi * half / 3.0f));
            glm::vec2 prev = c + glm::vec2(std::cos(base - kPi * 0.5f), std::sin(base - kPi * 0.5f)) * half;
            for (int s = 1; s <= steps; ++s) {
                const float a = base - kPi * 0.5f + kPi * (float)s / (float)steps;
                const glm::vec2 q = c + glm::vec2(std::cos(a), std::sin(a)) * half;
                tri(c, prev, q);
                prev = q;
            }
        }
    }
    return out;
}

std::vector<bool> RingHoles(const std::vector<std::vector<glm::vec2>>& rings) {
    std::vector<bool> out(rings.size(), false);
    for (size_t i = 0; i < rings.size(); ++i) {
        if (rings[i].empty()) continue;
        int depth = 0;
        for (size_t j = 0; j < rings.size(); ++j)
            if (i != j && rings[j].size() >= 3 && PointInRing(rings[j], rings[i][0])) ++depth;
        out[i] = depth % 2 == 1;
    }
    return out;
}

bool ShapeContains(const Shape& shape, const UIRect& r, glm::vec2 p) {
    bool in = false;
    for (const auto& ring : ShapeRings(shape, r))
        if (PointInRing(ring, p)) in = !in;
    return in;
}

std::vector<unsigned char> RasterizeShape(const Shape& shape, int w, int h) {
    std::vector<float> cover((size_t)w * (size_t)h, 0.0f);
    const auto rings = ShapeRings(shape, UIRect{0.0f, 0.0f, (float)w, (float)h});
    // Четыре подстроки на пиксель и точная доля по горизонтали: край
    // сглаженный, а цена — проход по рёбрам на подстроку, без проверки
    // каждого пикселя.
    constexpr int kSub = 4;
    std::vector<float> xs;
    for (int y = 0; y < h; ++y) {
        for (int s = 0; s < kSub; ++s) {
            const float sy = (float)y + ((float)s + 0.5f) / (float)kSub;
            xs.clear();
            for (const auto& ring : rings)
                for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                    const glm::vec2 a = ring[j], b = ring[i];
                    if ((a.y > sy) != (b.y > sy)) xs.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
                }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const float x0 = std::clamp(xs[k], 0.0f, (float)w), x1 = std::clamp(xs[k + 1], 0.0f, (float)w);
                for (int x = (int)std::floor(x0); x < (int)std::ceil(x1) && x < w; ++x) {
                    const float c = std::min(x1, (float)x + 1.0f) - std::max(x0, (float)x);
                    if (c > 0.0f) cover[(size_t)y * (size_t)w + (size_t)x] += c / (float)kSub;
                }
            }
        }
    }
    std::vector<unsigned char> out(cover.size());
    for (size_t i = 0; i < cover.size(); ++i) out[i] = (unsigned char)std::clamp(cover[i] * 255.0f + 0.5f, 0.0f, 255.0f);
    return out;
}

} // namespace sage::ui
