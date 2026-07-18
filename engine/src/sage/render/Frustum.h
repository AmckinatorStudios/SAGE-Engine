#pragma once
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Frustum — 6 плоскостей усечённой пирамиды камеры (или света), извлечённые из
// матрицы view-projection методом Gribb–Hartmann. Используется для отсечения:
// объекты, чья ограничивающая сфера целиком снаружи, не рисуются. Так стоимость
// кадра зависит от ВИДИМЫХ объектов, а не от всех в сцене.
// ---------------------------------------------------------------------------
namespace sage::render {

struct Frustum {
    // Плоскости в форме (a,b,c,d): точка внутри, если a*x+b*y+c*z+d >= 0.
    glm::vec4 Planes[6];

    // m — это proj * view (для камеры) или lightSpace (для тени).
    static Frustum FromViewProj(const glm::mat4& m) {
        // glm — column-major: m[col][row]. Строки матрицы:
        glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
        glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
        glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
        glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);

        Frustum f;
        f.Planes[0] = r3 + r0; // left
        f.Planes[1] = r3 - r0; // right
        f.Planes[2] = r3 + r1; // bottom
        f.Planes[3] = r3 - r1; // top
        f.Planes[4] = r3 + r2; // near
        f.Planes[5] = r3 - r2; // far
        for (auto& p : f.Planes) {
            float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
        return f;
    }

    // Пересекает ли сфера (центр в мировых координатах, радиус) фрустум.
    bool IntersectsSphere(const glm::vec3& c, float radius) const {
        for (const glm::vec4& p : Planes) {
            float dist = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
            if (dist < -radius) return false; // целиком за плоскостью — снаружи
        }
        return true;
    }
};

} // namespace sage::render
