#include "sage/render/MeshRaycast.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "sage/render/Mesh.h"

namespace sage::render {

RayHit RayAabb(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo,
               const glm::vec3& hi) {
    RayHit out;
    float tMin = -std::numeric_limits<float>::infinity();
    float tMax = std::numeric_limits<float>::infinity();

    for (int a = 0; a < 3; ++a) {
        // Луч, параллельный паре плоскостей, либо всегда между ними, либо
        // никогда: делить на нулевое направление здесь нельзя — получилась бы
        // неопределённость 0/0 на самой границе.
        if (std::fabs(dir[a]) < 1e-8f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) return out;
            continue;
        }
        const float inv = 1.0f / dir[a];
        float t1 = (lo[a] - origin[a]) * inv;
        float t2 = (hi[a] - origin[a]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return out;
    }
    if (tMax < 0.0f) return out;   // коробка целиком позади начала луча

    out.Hit = true;
    // Начало внутри коробки — расстояние 0, а не отрицательное t входа: иначе
    // объект, внутри которого стоит камера, «выигрывал» бы у всех остальных.
    out.Distance = std::max(tMin, 0.0f);
    return out;
}

namespace {

// Мёллер—Трумбор. Двусторонний: у моделей нередко вывернуты нормали, и
// отбрасывать «заднюю» сторону значило бы не выбирать такие объекты вовсе.
bool RayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& v0, const glm::vec3& v1,
                 const glm::vec3& v2, float& t) {
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;   // луч в плоскости треугольника
    const float invDet = 1.0f / det;
    const glm::vec3 s = o - v0;
    const float u = glm::dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(e2, q) * invDet;
    return t >= 0.0f;
}

} // namespace

RayHit RayGeometry(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo,
                   const glm::vec3& hi, const std::vector<Vertex>* verts,
                   const std::vector<unsigned int>* idx) {
    const RayHit box = RayAabb(origin, dir, lo, hi);
    if (!box.Hit) return box;

    if (!verts || !idx || idx->size() < 3) return box;   // копии геометрии нет — точность коробки

    RayHit best;
    best.Distance = std::numeric_limits<float>::infinity();
    const size_t triCount = idx->size() / 3;
    for (size_t i = 0; i < triCount; ++i) {
        const unsigned int i0 = (*idx)[i * 3 + 0];
        const unsigned int i1 = (*idx)[i * 3 + 1];
        const unsigned int i2 = (*idx)[i * 3 + 2];
        if (i0 >= verts->size() || i1 >= verts->size() || i2 >= verts->size()) continue;
        float t = 0.0f;
        if (RayTriangle(origin, dir, (*verts)[i0].Position, (*verts)[i1].Position,
                        (*verts)[i2].Position, t) &&
            t < best.Distance) {
            best.Hit = true;
            best.Exact = true;
            best.Distance = t;
        }
    }
    // Коробку пробили, а ни одного треугольника не задели — значит промах:
    // именно этим точный тест и отличается от приблизительного.
    return best.Hit ? best : RayHit{};
}

RayHit RayMesh(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir) {
    return RayGeometry(origin, dir, mesh.BoundsMin(), mesh.BoundsMax(), mesh.CpuVertices(),
                       mesh.CpuIndices());
}

} // namespace sage::render
