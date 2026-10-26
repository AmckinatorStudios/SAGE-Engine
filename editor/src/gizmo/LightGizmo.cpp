#include "gizmo/LightGizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/gtc/constants.hpp>

namespace sage::editor {

namespace lightedit {
float Range(float start, float worldDelta) { return std::max(start + worldDelta, 0.05f); }

void SpotAngle(const LightComponent& start, float rimRadius, LightComponent& out) {
    const float range = std::max(start.Range, 1e-3f);
    out.OuterConeDeg = glm::clamp(glm::degrees(std::atan(std::max(rimRadius, 0.0f) / range)), 1.0f, 89.0f);
    // Внутренний угол держит ту же ДОЛЮ внешнего, что и был: мягкость края
    // прожектора при раскрытии конуса не меняется.
    const float ratio = start.OuterConeDeg > 1e-3f ? start.InnerConeDeg / start.OuterConeDeg : 0.7f;
    out.InnerConeDeg = glm::clamp(out.OuterConeDeg * ratio, 0.0f, out.OuterConeDeg);
}

float AreaSide(float start, float worldDelta) { return std::max(start + worldDelta * 2.0f, 0.01f); }
} // namespace lightedit

namespace {
struct Frame {
    glm::vec3 Pos, Fwd, Right, Up;
};
Frame FrameOf(const glm::mat4& world) {
    Frame f;
    f.Pos = glm::vec3(world[3]);
    auto norm = [](glm::vec3 v, glm::vec3 fb) { return glm::length(v) > 1e-6f ? glm::normalize(v) : fb; };
    f.Fwd = norm(glm::vec3(world * glm::vec4(0, 0, -1, 0)), {0, 0, -1});
    f.Right = norm(glm::vec3(world[0]), {1, 0, 0});
    f.Up = norm(glm::vec3(world[1]), {0, 1, 0});
    return f;
}
} // namespace

void DrawLightGizmo(DebugDraw& dd, const LightComponent& light, const glm::mat4& world, bool selected) {
    const Frame f = FrameOf(world);
    // Цвет — цвет самого света, но не темнее читаемого: синий свет на тёмном
    // фоне иначе пропадал вместе с гизмо.
    glm::vec3 c = glm::vec3(light.Color);
    c = glm::max(c / std::max(std::max(c.r, std::max(c.g, c.b)), 1e-3f), glm::vec3(0.25f));
    const glm::vec3 col = c * (selected ? 1.0f : 0.75f);
    const glm::vec3 dim = c * 0.45f;
    switch (light.Kind) {
        case LightComponent::Type::Directional: {
            // Диск, обращённый по свету, и параллельные лучи: солнце не имеет
            // ни положения, ни дальности — только направление.
            const float r = 0.35f, len = selected ? 2.4f : 1.2f;
            dd.Circle(f.Pos, f.Fwd, r, col, 24);
            dd.Arrow(f.Pos, f.Pos + f.Fwd * (len + 0.3f), col, 0.2f);
            for (int i = 0; i < 6; ++i) {
                const float a = glm::two_pi<float>() * (float)i / 6.0f;
                const glm::vec3 o = f.Pos + (f.Right * std::cos(a) + f.Up * std::sin(a)) * r;
                dd.Line(o, o + f.Fwd * len, col);
            }
            break;
        }
        case LightComponent::Type::Point: {
            // Маленькая звёздочка из трёх колец — видно, что светит во все
            // стороны; дальность — только у выделенного: десятки сфер в сцене
            // с лампами закрыли бы всё.
            for (const glm::vec3& n : {f.Right, f.Up, f.Fwd}) dd.Circle(f.Pos, n, 0.18f, col, 16);
            if (selected)
                for (const glm::vec3& n : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)})
                    dd.Circle(f.Pos, n, light.Range, dim, 48);
            break;
        }
        case LightComponent::Type::Spot: {
            // Конус: четыре образующих и два основания — внешний край ярко,
            // внутренний бледно. Невыделенный — коротким (метр): видно, куда
            // смотрит, и сцена не превращается в паутину.
            const float len = selected ? light.Range : std::min(light.Range, 1.0f);
            const glm::vec3 base = f.Pos + f.Fwd * len;
            const float outer = len * std::tan(glm::radians(glm::clamp(light.OuterConeDeg, 0.5f, 89.0f)));
            const float inner = len * std::tan(glm::radians(glm::clamp(light.InnerConeDeg, 0.0f, 89.0f)));
            dd.Circle(base, f.Fwd, outer, col, 40);
            if (selected && inner > 1e-3f) dd.Circle(base, f.Fwd, inner, dim, 40);
            for (const glm::vec3& s : {f.Right, -f.Right, f.Up, -f.Up}) dd.Line(f.Pos, base + s * outer, col);
            if (selected) dd.Line(f.Pos, base, dim);
            dd.Circle(f.Pos, f.Fwd, 0.08f, col, 12);
            break;
        }
        case LightComponent::Type::Area: {
            const glm::vec3 r = f.Right * (light.AreaWidth * 0.5f), u = f.Up * (light.AreaHeight * 0.5f);
            const glm::vec3 q[4] = {f.Pos + r + u, f.Pos - r + u, f.Pos - r - u, f.Pos + r - u};
            for (int i = 0; i < 4; ++i) dd.Line(q[i], q[(i + 1) % 4], col);
            // Диагонали — бледно: панель читается плоскостью, а не рамкой.
            dd.Line(q[0], q[2], dim);
            dd.Line(q[1], q[3], dim);
            // Светит одной стороной — стрелка показывает какой.
            const float a = std::clamp(std::min(light.AreaWidth, light.AreaHeight) * 0.6f, 0.25f, 1.0f);
            dd.Arrow(f.Pos, f.Pos + f.Fwd * (selected ? std::min(light.Range, a * 2.0f) : a), col, 0.12f);
            break;
        }
    }
}

LightHandles::Result LightHandles::Update(LightComponent& light, const glm::mat4& world, const handles::View& view,
                                          bool allowInput) {
    Result res;
    const Frame f = FrameOf(world);
    struct H {
        int Id;
        glm::vec3 At, Axis;
    };
    std::vector<H> hs;
    switch (light.Kind) {
        case LightComponent::Type::Point:
            hs.push_back({kRange, f.Pos + f.Up * light.Range, f.Up});
            break;
        case LightComponent::Type::Spot: {
            const glm::vec3 base = f.Pos + f.Fwd * light.Range;
            const float rim = light.Range * std::tan(glm::radians(glm::clamp(light.OuterConeDeg, 0.5f, 89.0f)));
            hs.push_back({kRange, base, f.Fwd});
            hs.push_back({kAngle, base + f.Up * rim, f.Up});
            break;
        }
        case LightComponent::Type::Area:
            hs.push_back({kWidthPos, f.Pos + f.Right * (light.AreaWidth * 0.5f), f.Right});
            hs.push_back({kWidthNeg, f.Pos - f.Right * (light.AreaWidth * 0.5f), -f.Right});
            hs.push_back({kHeightPos, f.Pos + f.Up * (light.AreaHeight * 0.5f), f.Up});
            hs.push_back({kHeightNeg, f.Pos - f.Up * (light.AreaHeight * 0.5f), -f.Up});
            break;
        case LightComponent::Type::Directional: break;   // ни дальности, ни размера
    }
    if (hs.empty()) {
        m_active = -1;
        return res;
    }
    std::vector<handles::Point> pts;
    for (const H& h : hs) {
        handles::Point p;
        p.Id = h.Id;
        p.World = h.At;
        p.Color = IM_COL32(255, 214, 90, 255);   // ручки света — тёплые, не цвета осей
        pts.push_back(p);
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const int hot = (allowInput && m_active < 0) ? handles::Hovered(view, pts, mouse) : -1;
    glm::vec3 ro, rd;
    handles::Ray(view, mouse, ro, rd);
    if (m_active < 0 && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        for (const H& h : hs)
            if (h.Id == hot) {
                m_active = hot;
                m_start = light;
                m_startWorld = h.At;
                m_axis = h.Axis;
                m_startParam = handles::AxisParam(h.At, h.Axis, ro, rd);
                res.Began = true;
            }
    }
    if (m_active >= 0) {
        res.WantMouse = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_active = -1;
            res.Ended = true;
        } else {
            const float d = handles::AxisParam(m_startWorld, m_axis, ro, rd) - m_startParam;
            LightComponent next = light;
            switch (m_active) {
                case kRange: next.Range = lightedit::Range(m_start.Range, d); break;
                case kAngle: {
                    const float rim0 =
                        m_start.Range * std::tan(glm::radians(glm::clamp(m_start.OuterConeDeg, 0.5f, 89.0f)));
                    lightedit::SpotAngle(m_start, rim0 + d, next);
                    break;
                }
                case kWidthPos: case kWidthNeg: next.AreaWidth = lightedit::AreaSide(m_start.AreaWidth, d); break;
                default: next.AreaHeight = lightedit::AreaSide(m_start.AreaHeight, d); break;
            }
            res.Changed = next.Range != light.Range || next.OuterConeDeg != light.OuterConeDeg ||
                          next.InnerConeDeg != light.InnerConeDeg || next.AreaWidth != light.AreaWidth ||
                          next.AreaHeight != light.AreaHeight;
            light.Range = next.Range;
            light.OuterConeDeg = next.OuterConeDeg;
            light.InnerConeDeg = next.InnerConeDeg;
            light.AreaWidth = next.AreaWidth;
            light.AreaHeight = next.AreaHeight;
        }
    } else if (hot >= 0) {
        res.WantMouse = true;
    }
    // Ручки — по новому состоянию, чтобы стояли на форме, а не на прошлой.
    for (handles::Point& p : pts) {
        const Frame g = f;
        switch (p.Id) {
            case kRange:
                p.World = light.Kind == LightComponent::Type::Point ? g.Pos + g.Up * light.Range
                                                                    : g.Pos + g.Fwd * light.Range;
                break;
            case kAngle:
                p.World = g.Pos + g.Fwd * light.Range +
                          g.Up * (light.Range * std::tan(glm::radians(glm::clamp(light.OuterConeDeg, 0.5f, 89.0f))));
                break;
            case kWidthPos: p.World = g.Pos + g.Right * (light.AreaWidth * 0.5f); break;
            case kWidthNeg: p.World = g.Pos - g.Right * (light.AreaWidth * 0.5f); break;
            case kHeightPos: p.World = g.Pos + g.Up * (light.AreaHeight * 0.5f); break;
            default: p.World = g.Pos - g.Up * (light.AreaHeight * 0.5f); break;
        }
        handles::DrawPoint(view, p, p.Id == hot, p.Id == m_active);
    }
    if (m_active >= 0) {
        char buf[64];
        if (m_active == kRange) std::snprintf(buf, sizeof(buf), "%.2f m", light.Range);
        else if (m_active == kAngle)
            std::snprintf(buf, sizeof(buf), "%.1f° / %.1f°", light.OuterConeDeg, light.InnerConeDeg);
        else std::snprintf(buf, sizeof(buf), "%.2f × %.2f m", light.AreaWidth, light.AreaHeight);
        handles::Label(view, mouse, buf);
    }
    return res;
}

} // namespace sage::editor
