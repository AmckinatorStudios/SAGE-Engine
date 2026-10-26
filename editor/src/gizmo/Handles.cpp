#include "gizmo/Handles.h"

#include <algorithm>
#include <cmath>

namespace sage::editor::handles {

bool ToScreen(const View& v, const glm::vec3& world, ImVec2& out) {
    const glm::vec4 c = v.Proj * v.ViewM * glm::vec4(world, 1.0f);
    if (c.w <= 1e-5f) return false;
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    out = ImVec2(v.Min.x + (ndc.x * 0.5f + 0.5f) * v.Size.x, v.Min.y + (0.5f - ndc.y * 0.5f) * v.Size.y);
    return true;
}

void Ray(const View& v, ImVec2 screen, glm::vec3& origin, glm::vec3& dir) {
    const float x = (screen.x - v.Min.x) / std::max(v.Size.x, 1.0f) * 2.0f - 1.0f;
    const float y = 1.0f - (screen.y - v.Min.y) / std::max(v.Size.y, 1.0f) * 2.0f;
    const glm::mat4 inv = glm::inverse(v.Proj * v.ViewM);
    glm::vec4 a = inv * glm::vec4(x, y, -1.0f, 1.0f);
    glm::vec4 b = inv * glm::vec4(x, y, 1.0f, 1.0f);
    a /= a.w;
    b /= b.w;
    origin = glm::vec3(a);
    dir = glm::normalize(glm::vec3(b - a));
}

float AxisParam(const glm::vec3& p0, const glm::vec3& axis, const glm::vec3& ro, const glm::vec3& rd) {
    // Ближайшие точки двух прямых: ось и луч. Почти параллельные (ось смотрит
    // в камеру) — ход по ней не определён, ручка стоит.
    const glm::vec3 w = p0 - ro;
    const float a = glm::dot(axis, axis), b = glm::dot(axis, rd), c = glm::dot(rd, rd);
    const float d = glm::dot(axis, w), e = glm::dot(rd, w);
    const float den = a * c - b * b;
    if (std::abs(den) < 1e-6f) return 0.0f;
    return (b * e - c * d) / den;
}

bool PlaneHit(const glm::vec3& p0, const glm::vec3& n, const glm::vec3& ro, const glm::vec3& rd, glm::vec3& out) {
    const float den = glm::dot(n, rd);
    if (std::abs(den) < 1e-6f) return false;
    out = ro + rd * (glm::dot(n, p0 - ro) / den);
    return true;
}

float Snap(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

ImU32 AxisColor(int axis, float alpha) {
    const int a = (int)(alpha * 255.0f);
    switch (axis) {
        case 0: return IM_COL32(240, 70, 70, a);
        case 1: return IM_COL32(110, 215, 80, a);
        case 2: return IM_COL32(70, 130, 255, a);
        default: return IM_COL32(235, 235, 235, a);
    }
}

int Hovered(const View& v, const std::vector<Point>& pts, ImVec2 mouse, float radiusPx) {
    int best = -1;
    float bestD = radiusPx * radiusPx;
    bool bestBack = true;
    for (const Point& p : pts) {
        ImVec2 s;
        if (!ToScreen(v, p.World, s)) continue;
        const float dx = s.x - mouse.x, dy = s.y - mouse.y, d = dx * dx + dy * dy;
        if (d > radiusPx * radiusPx) continue;
        // Передняя ручка побеждает заднюю, даже если задняя ближе к курсору:
        // за неё и хватают, она — та, что на виду.
        if ((bestBack && !p.Back) || (bestBack == p.Back && d < bestD)) {
            best = p.Id;
            bestD = d;
            bestBack = p.Back;
        }
    }
    return best;
}

void DrawPoint(const View& v, const Point& p, bool hot, bool active) {
    ImVec2 s;
    if (!v.Draw || !ToScreen(v, p.World, s)) return;
    const float r = active ? 6.5f : hot ? 6.0f : 4.5f;
    ImU32 fill = p.Color;
    if (p.Back && !hot && !active) fill = (fill & 0x00FFFFFF) | (90u << 24);
    if (active) fill = IM_COL32(255, 220, 60, 255);
    else if (hot) fill = IM_COL32(255, 255, 255, 255);
    const ImU32 rim = IM_COL32(15, 15, 18, p.Back && !hot ? 120 : 230);
    if (p.Square) {
        v.Draw->AddRectFilled(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), fill, 1.5f);
        v.Draw->AddRect(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), rim, 1.5f, 0, 1.5f);
    } else {
        v.Draw->AddCircleFilled(s, r, fill, 16);
        v.Draw->AddCircle(s, r, rim, 16, 1.5f);
    }
}

void Label(const View& v, ImVec2 at, const char* text) {
    if (!v.Draw || !text || !*text) return;
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 p(at.x + 14.0f, at.y + 10.0f);
    v.Draw->AddRectFilled(ImVec2(p.x - 5, p.y - 3), ImVec2(p.x + size.x + 5, p.y + size.y + 3),
                          IM_COL32(20, 22, 26, 225), 4.0f);
    v.Draw->AddText(p, IM_COL32(235, 235, 240, 255), text);
}

} // namespace sage::editor::handles
