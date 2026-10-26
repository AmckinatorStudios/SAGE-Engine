#include "gizmo/ColliderHandles.h"

#include <algorithm>
#include <cstdio>

namespace sage::editor {

namespace ce = collideredit;
using sage::physics::ShapeType;

namespace {
// Номер ручки: ось * 2 + (сторона > 0); центр — kCenter.
int IdOf(int axis, int side) { return axis * 2 + (side > 0 ? 1 : 0); }
ce::Handle HandleOf(int id) { return ce::Handle{id / 2, (id % 2) ? 1 : -1}; }
} // namespace

ColliderHandles::Result ColliderHandles::Update(ColliderComponent& col, const glm::mat4& world,
                                                const handles::View& view, bool allowInput,
                                                const glm::vec3& snapMove) {
    Result res;
    if (!ce::Editable(col)) {
        m_active = kNone;
        return res;
    }
    const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                          glm::length(glm::vec3(world[2])));
    // Оси объекта без масштаба: масштаб уже сидит в положении ручек.
    glm::mat3 axes(1.0f);
    for (int k = 0; k < 3; ++k) axes[k] = scale[k] > 1e-6f ? glm::vec3(world[k]) / scale[k] : glm::vec3(0.0f);
    const glm::vec3 origin(world[3]);
    auto toWorld = [&](const glm::vec3& local) { return origin + axes * (local * scale); };

    // Набор ручек этой формы.
    std::vector<handles::Point> pts;
    const glm::vec3 centerW = toWorld(col.Center);
    for (int a = 0; a < 3; ++a)
        for (int s : {-1, 1}) {
            handles::Point p;
            p.Id = IdOf(a, s);
            p.World = toWorld(ce::HandleLocal(col, scale, {a, s}));
            p.Color = handles::AxisColor(a);
            // Ручка на обратной стороне — та, чья наружная нормаль смотрит от глаза.
            const glm::vec3 n = axes[a] * (float)s;
            p.Back = view.Ortho ? glm::dot(n, glm::vec3(glm::inverse(view.ViewM)[2])) < 0.0f
                                : glm::dot(n, view.Eye - p.World) < 0.0f;
            pts.push_back(p);
        }
    handles::Point c;
    c.Id = kCenter;
    c.World = centerW;
    c.Square = true;
    pts.push_back(c);

    const ImVec2 mouse = ImGui::GetMousePos();
    const int hot = (allowInput && m_active == kNone) ? handles::Hovered(view, pts, mouse) : -1;

    glm::vec3 ro, rd;
    handles::Ray(view, mouse, ro, rd);
    if (m_active == kNone && hot >= 0 && hot != kCenter && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Нажали на ручку размера.
        m_active = hot;
        m_start = col;
        const ce::Handle h = HandleOf(hot);
        m_axisWorld = glm::normalize(axes[h.Axis] * (float)h.Side);
        m_startWorld = toWorld(ce::HandleLocal(col, scale, h));
        m_startParam = handles::AxisParam(m_startWorld, m_axisWorld, ro, rd);
        res.Began = true;
    } else if (m_active == kNone && hot == kCenter && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_active = kCenter;
        m_start = col;
        m_startWorld = centerW;
        res.Began = true;
    }

    if (m_active != kNone) {
        res.WantMouse = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_active = kNone;
            res.Ended = true;
        } else if (m_active == kCenter) {
            // Центр — в плоскости экрана через исходную точку.
            const glm::vec3 fwd = glm::normalize(glm::vec3(glm::inverse(view.ViewM)[2]));
            glm::vec3 hit;
            if (handles::PlaneHit(m_startWorld, fwd, ro, rd, hit)) {
                glm::vec3 dLocal = glm::transpose(axes) * (hit - m_startWorld);   // оси объекта, мировые единицы
                for (int k = 0; k < 3; ++k) dLocal[k] = handles::Snap(dLocal[k], snapMove[k]);
                ColliderComponent next = ce::MoveCenter(m_start, scale, dLocal);
                res.Changed = next.Center != col.Center;
                col.Center = next.Center;
            }
        } else {
            const ce::Handle h = HandleOf(m_active);
            float d = handles::AxisParam(m_startWorld, m_axisWorld, ro, rd) - m_startParam;
            d = handles::Snap(d, snapMove[h.Axis]);
            const bool symmetric = ImGui::GetIO().KeyAlt;
            const ColliderComponent next = ce::DragHandle(m_start, scale, h, d, symmetric);
            res.Changed = next.HalfExtents != col.HalfExtents || next.Center != col.Center ||
                          next.Radius != col.Radius || next.HalfHeight != col.HalfHeight;
            col.HalfExtents = next.HalfExtents;
            col.Center = next.Center;
            col.Radius = next.Radius;
            col.HalfHeight = next.HalfHeight;
        }
    } else if (hot >= 0) {
        res.WantMouse = true;
    }

    // Отрисовка: заданные ручки — после пересчёта, чтобы стояли там, где форма.
    for (handles::Point& p : pts) {
        if (p.Id == kCenter) p.World = toWorld(col.Center);
        else p.World = toWorld(ce::HandleLocal(col, scale, HandleOf(p.Id)));
    }
    for (int pass = 0; pass < 2; ++pass)
        for (const handles::Point& p : pts)
            if (p.Back == (pass == 0)) handles::DrawPoint(view, p, p.Id == hot, p.Id == m_active);

    m_lastView = view;
    m_lastView.Draw = nullptr;
    m_lastPoints = pts;
    if (m_active != kNone) {
        // Текущий размер — в мировых единицах, как его видит человек.
        char buf[96];
        const float u = std::max(scale.x, std::max(scale.y, scale.z));
        if (m_active == kCenter)
            std::snprintf(buf, sizeof(buf), "%.3f  %.3f  %.3f", col.Center.x, col.Center.y, col.Center.z);
        else if (col.Shape == ShapeType::Box)
            std::snprintf(buf, sizeof(buf), "%.3f × %.3f × %.3f", col.HalfExtents.x * 2 * scale.x,
                          col.HalfExtents.y * 2 * scale.y, col.HalfExtents.z * 2 * scale.z);
        else if (col.Shape == ShapeType::Sphere)
            std::snprintf(buf, sizeof(buf), "R %.3f", col.Radius * u);
        else
            std::snprintf(buf, sizeof(buf), "R %.3f   H %.3f", col.Radius * u,
                          (col.HalfHeight * scale.y + col.Radius * u) * 2.0f);
        handles::Label(view, mouse, buf);
    }
    return res;
}

} // namespace sage::editor
