// Контуры произвольной формы: веер заливки и полоса обводки (см. UIRenderer.h).
// Отдельным файлом: у рендерера своя тысяча строк, а это — самостоятельная
// часть, которой нужен только PushFreeQuad.
#include "UIRenderer.h"

namespace {

void Split(const glm::vec4& c, glm::vec3& rgb, float& a) {
    rgb = {c.r, c.g, c.b};
    a = c.a;
}

} // namespace

void UIRenderer::FillPolygon(glm::vec2 center, glm::vec4 centerColor, const std::vector<glm::vec2>& ring,
                             const std::vector<glm::vec4>& colors) {
    const size_t n = ring.size();
    if (n < 3 || colors.size() != n) return;
    glm::vec3 cc;
    float ca;
    Split(centerColor, cc, ca);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        // Треугольник — квад со схлопнутой четвёртой вершиной (как Triangle).
        const glm::vec2 p[4] = {center, ring[i], ring[j], ring[j]};
        glm::vec3 c[4] = {cc, {}, {}, {}};
        float a[4] = {ca, 0.0f, 0.0f, 0.0f};
        Split(colors[i], c[1], a[1]);
        Split(colors[j], c[2], a[2]);
        c[3] = c[2];
        a[3] = a[2];
        CurrentSegment(nullptr).QuadCount++;
        PushFreeQuad(p, c, a);
    }
}

void UIRenderer::StripPolygon(const std::vector<glm::vec2>& inner, const std::vector<glm::vec4>& innerColors,
                              const std::vector<glm::vec2>& outer, const std::vector<glm::vec4>& outerColors) {
    const size_t n = inner.size();
    if (n < 2 || outer.size() != n || innerColors.size() != n || outerColors.size() != n) return;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const glm::vec2 p[4] = {inner[i], outer[i], outer[j], inner[j]};
        glm::vec3 c[4];
        float a[4];
        Split(innerColors[i], c[0], a[0]);
        Split(outerColors[i], c[1], a[1]);
        Split(outerColors[j], c[2], a[2]);
        Split(innerColors[j], c[3], a[3]);
        CurrentSegment(nullptr).QuadCount++;
        PushFreeQuad(p, c, a);
    }
}
