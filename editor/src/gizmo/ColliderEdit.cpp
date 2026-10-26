#include "gizmo/ColliderEdit.h"

#include <algorithm>
#include <cmath>

namespace sage::editor::collideredit {

using sage::physics::ShapeType;

namespace {
float Uniform(const glm::vec3& s) { return std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z))); }
glm::vec3 Safe(const glm::vec3& s) { return glm::max(glm::abs(s), glm::vec3(1e-4f)); }
} // namespace

bool Editable(const ColliderComponent& col) {
    return col.Parts.empty() &&
           (col.Shape == ShapeType::Box || col.Shape == ShapeType::Sphere || col.Shape == ShapeType::Capsule);
}

glm::vec3 HandleLocal(const ColliderComponent& col, const glm::vec3& scale, Handle h) {
    if (h.Axis < 0) return col.Center;
    glm::vec3 off(0.0f);
    const glm::vec3 s = Safe(scale);
    const float u = std::max(Uniform(scale), 1e-4f);
    switch (col.Shape) {
        case ShapeType::Box: off[h.Axis] = col.HalfExtents[h.Axis]; break;
        // Радиус шара домножается на НАИБОЛЬШИЙ масштаб (как у физики), а
        // ручка стоит в осях объекта: делим на масштаб своей оси, чтобы в мире
        // она легла ровно на поверхность.
        case ShapeType::Sphere: off[h.Axis] = col.Radius * u / s[h.Axis]; break;
        case ShapeType::Capsule:
            if (h.Axis == 1) off.y = (col.HalfHeight * s.y + col.Radius * u) / s.y;
            else off[h.Axis] = col.Radius * u / s[h.Axis];
            break;
        default: break;
    }
    return col.Center + off * (float)h.Side;
}

ColliderComponent DragHandle(const ColliderComponent& start, const glm::vec3& scale, Handle h, float worldDelta,
                             bool symmetric) {
    ColliderComponent c = start;
    if (h.Axis < 0) return c;
    const glm::vec3 s = Safe(scale);
    const float u = std::max(Uniform(scale), 1e-4f);
    switch (start.Shape) {
        case ShapeType::Box: {
            const int a = h.Axis;
            const float d = worldDelta / s[a];   // ход грани в осях объекта
            if (symmetric) {
                c.HalfExtents[a] = std::max(start.HalfExtents[a] + d, kMinSize);
            } else {
                // Противоположная грань стоит: размер растёт на половину хода,
                // центр едет на половину хода за тянущей гранью.
                const float opposite = start.Center[a] - (float)h.Side * start.HalfExtents[a];
                c.HalfExtents[a] = std::max(start.HalfExtents[a] + d * 0.5f, kMinSize);
                c.Center[a] = opposite + (float)h.Side * c.HalfExtents[a];
            }
            break;
        }
        // У шара и капсулы ручки симметричны всегда: смещённая наполовину
        // форма с тем же радиусом не лежит под рукой, а прыгает от неё.
        case ShapeType::Sphere: c.Radius = std::max(start.Radius + worldDelta / u, kMinSize); break;
        case ShapeType::Capsule:
            if (h.Axis == 1) c.HalfHeight = std::max(start.HalfHeight + worldDelta / s.y, 0.0f);
            else c.Radius = std::max(start.Radius + worldDelta / u, kMinSize);
            break;
        default: break;
    }
    return c;
}

ColliderComponent MoveCenter(const ColliderComponent& start, const glm::vec3& scale, const glm::vec3& delta) {
    ColliderComponent c = start;
    c.Center = start.Center + delta / Safe(scale);
    return c;
}

} // namespace sage::editor::collideredit
