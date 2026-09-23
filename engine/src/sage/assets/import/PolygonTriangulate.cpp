#include "sage/assets/import/PolygonTriangulate.h"

#include <cmath>

namespace sage::assets {

namespace {

// Удвоенная ориентированная площадь треугольника на плоскости: > 0 — обход
// против часовой, то есть вершина b выпуклая в многоугольнике того же обхода.
float Cross2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool InsideTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b,
                    const glm::vec2& c) {
    // Граница считается «внутри»: вершина, лежащая ровно на диагонали, делает
    // ухо недопустимым — иначе отрезанный треугольник перекрыл бы соседний.
    return Cross2(a, b, p) >= 0.0f && Cross2(b, c, p) >= 0.0f && Cross2(c, a, p) >= 0.0f;
}

void Fan(size_t n, std::vector<uint32_t>& out) {
    for (size_t k = 1; k + 1 < n; ++k) {
        out.push_back(0);
        out.push_back((uint32_t)k);
        out.push_back((uint32_t)k + 1);
    }
}

} // namespace

std::vector<uint32_t> TriangulatePolygon(const std::vector<glm::vec3>& points) {
    std::vector<uint32_t> out;
    const size_t n = points.size();
    if (n < 3) return out;
    if (n == 3) return {0, 1, 2};

    // Нормаль по Ньюэллу: сумма по рёбрам, а не одно векторное произведение —
    // у вогнутой грани «первые три вершины» могут дать нормаль наизнанку.
    glm::vec3 normal(0.0f);
    for (size_t i = 0; i < n; ++i) {
        const glm::vec3& a = points[i];
        const glm::vec3& b = points[(i + 1) % n];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    const float len = glm::length(normal);
    // Грань без площади (все точки на одной прямой или в одной) — разбивать не
    // по чему; веер хотя бы сохраняет число треугольников.
    if (!(len > 1e-20f)) {
        Fan(n, out);
        return out;
    }
    normal /= len;

    // Базис плоскости (u, v, normal) — правая тройка, поэтому обход грани в
    // проекции получается против часовой, и «выпуклая» значит Cross2 > 0.
    const glm::vec3 helper = std::fabs(normal.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    const glm::vec3 u = glm::normalize(glm::cross(helper, normal));
    const glm::vec3 v = glm::cross(normal, u);
    std::vector<glm::vec2> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = glm::vec2(glm::dot(points[i], u), glm::dot(points[i], v));

    // Порог выпуклости — относительный: модели бывают и в миллиметрах, и в
    // километрах, и абсолютный эпсилон где-то обязательно промахнётся.
    float extent = 0.0f;
    for (const glm::vec2& q : p) extent = std::fmax(extent, std::fmax(std::fabs(q.x), std::fabs(q.y)));
    const float eps = extent * extent * 1e-7f;

    if (n == 4) {
        // Диагональ 0-2 годится, если вершины 1 и 3 выпуклые (вогнутой может
        // быть максимум одна, и диагональ обязана из неё выходить).
        const bool reflex1 = Cross2(p[0], p[1], p[2]) <= eps;
        const bool reflex3 = Cross2(p[2], p[3], p[0]) <= eps;
        if (reflex1 || reflex3) return {0, 1, 3, 1, 2, 3};
        return {0, 1, 2, 0, 2, 3};
    }

    std::vector<uint32_t> ring(n);
    for (size_t i = 0; i < n; ++i) ring[i] = (uint32_t)i;
    out.reserve((n - 2) * 3);

    size_t guard = 0;
    size_t i = 0;
    while (ring.size() > 3 && guard < ring.size()) {
        const size_t m = ring.size();
        const uint32_t a = ring[(i + m - 1) % m], b = ring[i % m], c = ring[(i + 1) % m];
        bool ear = Cross2(p[a], p[b], p[c]) > eps;
        for (size_t k = 0; ear && k < m; ++k) {
            const uint32_t q = ring[k];
            if (q == a || q == b || q == c) continue;
            if (InsideTriangle(p[q], p[a], p[b], p[c])) ear = false;
        }
        if (ear) {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            ring.erase(ring.begin() + (long)(i % m));
            guard = 0;
            if (i >= ring.size()) i = 0;
        } else {
            i = (i + 1) % m;
            ++guard;
        }
    }
    if (ring.size() == 3) {
        out.push_back(ring[0]);
        out.push_back(ring[1]);
        out.push_back(ring[2]);
    } else if (ring.size() > 3) {
        // Ушей не нашлось — грань самопересекается. Правильного разбиения у
        // неё нет, а потерять остаток значило бы продырявить модель: добиваем
        // веером по оставшимся вершинам.
        for (size_t k = 1; k + 1 < ring.size(); ++k) {
            out.push_back(ring[0]);
            out.push_back(ring[k]);
            out.push_back(ring[k + 1]);
        }
    }
    return out;
}

} // namespace sage::assets
