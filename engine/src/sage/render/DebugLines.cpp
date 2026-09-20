#include "sage/render/DebugLines.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Log.h"

namespace sage::render {

void DebugLines::Line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color,
                      float duration) {
    if (m_lines.size() >= kMaxLines) {
        if (!m_warned) {
            LOG_WARN("Debug") << "отладочных линий за кадр больше " << kMaxLines
                              << " — лишние не рисуются (скорее всего, отрисовка внутри цикла)";
            m_warned = true;
        }
        return;
    }
    m_lines.push_back(DebugLine{a, b, color, duration});
}

void DebugLines::Ray(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& color,
                     float duration) {
    Line(origin, origin + direction, color, duration);
}

void DebugLines::Sphere(const glm::vec3& center, float radius, const glm::vec3& color,
                        float duration, int segments) {
    const int n = std::clamp(segments, 4, 64);
    const float step = 6.28318530718f / (float)n;
    for (int i = 0; i < n; ++i) {
        const float a0 = step * (float)i;
        const float a1 = step * (float)(i + 1);
        const float c0 = std::cos(a0) * radius, s0 = std::sin(a0) * radius;
        const float c1 = std::cos(a1) * radius, s1 = std::sin(a1) * radius;
        Line(center + glm::vec3(c0, s0, 0.0f), center + glm::vec3(c1, s1, 0.0f), color, duration);
        Line(center + glm::vec3(c0, 0.0f, s0), center + glm::vec3(c1, 0.0f, s1), color, duration);
        Line(center + glm::vec3(0.0f, c0, s0), center + glm::vec3(0.0f, c1, s1), color, duration);
    }
}

void DebugLines::Box(const glm::vec3& center, const glm::vec3& half, const glm::vec3& color,
                     float duration) {
    const glm::vec3 lo = center - half, hi = center + half;
    const glm::vec3 c[8] = {
        {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
        {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                     {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) Line(c[e[0]], c[e[1]], color, duration);
}

void DebugLines::Tick(float dt) {
    // Линия «на один кадр» уходит СРАЗУ: её уже нарисовали (или рисовать было
    // некому — в headless-прогоне), и держать её дальше значит копить память
    // ровно до конца прогона.
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(),
                                 [dt](DebugLine& l) {
                                     if (l.TimeLeft <= 0.0f) return true;
                                     l.TimeLeft -= dt;
                                     return l.TimeLeft <= 0.0f;
                                 }),
                  m_lines.end());
    if (m_lines.empty()) m_warned = false;
}

} // namespace sage::render
