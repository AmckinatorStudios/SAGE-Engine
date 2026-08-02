#include "sage/render/DecalProjection.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

namespace sage::render {
namespace {

// Вершина промежуточного многоугольника: позиция в пространстве наклейки плюс
// нормаль ГРАНИ-приёмника. Нормаль берётся у грани, а не интерполируется по
// вершинам, потому что наклейка обязана лежать на плоскости, на которую её
// положили: сглаженная нормаль увела бы её подъём в сторону и на округлых
// поверхностях снова начались бы драки за глубину.
struct PolyVertex {
    glm::vec3 Pos;
    glm::vec3 Normal;
};

// Обрезка многоугольника одной плоскостью коробки (алгоритм Сазерленда—Ходжмана).
// axis — ось (0/1/2), positive — какая из двух граней: true отсекает всё, что
// больше limit, false — всё, что меньше.
void ClipToPlane(std::vector<PolyVertex>& poly, int axis, bool positive, float limit) {
    if (poly.empty()) return;
    // «Внутри» — расстояние до плоскости со знаком; знак выбран так, чтобы
    // положительное значило «оставляем».
    auto dist = [&](const PolyVertex& v) {
        return positive ? (limit - v.Pos[axis]) : (v.Pos[axis] - limit);
    };

    std::vector<PolyVertex> out;
    out.reserve(poly.size() + 2);
    for (size_t i = 0; i < poly.size(); ++i) {
        const PolyVertex& cur = poly[i];
        const PolyVertex& next = poly[(i + 1) % poly.size()];
        const float dCur = dist(cur), dNext = dist(next);
        const bool inCur = dCur >= 0.0f, inNext = dNext >= 0.0f;

        if (inCur) out.push_back(cur);
        // Ребро пересекает плоскость — добавляем точку пересечения. Нормаль
        // копируем: она у всей грани одна.
        if (inCur != inNext) {
            const float denom = dCur - dNext;
            if (std::fabs(denom) > 1e-12f) {
                const float t = dCur / denom;
                PolyVertex mid;
                mid.Pos = cur.Pos + (next.Pos - cur.Pos) * t;
                mid.Normal = cur.Normal;
                // Точка обязана лечь ТОЧНО на плоскость: накопленная ошибка
                // здесь вылезает щелью между наклейкой и краем коробки.
                mid.Pos[axis] = limit;
                out.push_back(mid);
            }
        }
    }
    poly.swap(out);
}

} // namespace

MeshData ProjectDecal(const std::vector<Vertex>& verts, const std::vector<unsigned int>& idx,
                      const glm::mat4& toDecal, const DecalProjectOptions& opts) {
    MeshData out;
    if (verts.empty() || idx.size() < 3) return out;

    // Нормали переводятся обратной транспонированной: неравномерный масштаб
    // приёмника иначе перекосил бы их, и отсечение по углу считало бы не то.
    const glm::mat3 normalMat = glm::inverseTranspose(glm::mat3(toDecal));

    // Проекция идёт вдоль -Z, значит принимаем грани, смотрящие на +Z.
    const float cosLimit = std::cos(glm::radians(std::clamp(opts.AngleLimitDeg, 0.0f, 89.9f)));

    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;

        const glm::vec3 p0 = glm::vec3(toDecal * glm::vec4(verts[a].Position, 1.0f));
        const glm::vec3 p1 = glm::vec3(toDecal * glm::vec4(verts[b].Position, 1.0f));
        const glm::vec3 p2 = glm::vec3(toDecal * glm::vec4(verts[c].Position, 1.0f));

        // Геометрическая нормаль грани в пространстве наклейки. Именно
        // геометрическая: вершинные нормали у сглаженной модели смотрят
        // «средне», и грань, реально повёрнутая от проектора, прошла бы отбор.
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        const float len2 = glm::dot(n, n);
        if (len2 < 1e-16f) continue; // вырожденный треугольник
        n *= 1.0f / std::sqrt(len2);

        // Смотрит ли грань на проектор (на +Z) и достаточно ли прямо.
        if (n.z < cosLimit) continue;

        std::vector<PolyVertex> poly = {{p0, n}, {p1, n}, {p2, n}};
        ClipToPlane(poly, 0, true, 0.5f);   ClipToPlane(poly, 0, false, -0.5f);
        ClipToPlane(poly, 1, true, 0.5f);   ClipToPlane(poly, 1, false, -0.5f);
        ClipToPlane(poly, 2, true, 0.5f);   ClipToPlane(poly, 2, false, -0.5f);
        if (poly.size() < 3) continue;

        // Веер треугольников из обрезанного многоугольника. Он выпуклый (это
        // пересечение треугольника с коробкой), поэтому веера достаточно.
        const unsigned base = (unsigned)out.Vertices.size();
        for (const PolyVertex& pv : poly) {
            Vertex v{};
            // Нормаль вершины наклейки — нормаль грани, вдоль неё же и подъём.
            const glm::vec3 nrm = pv.Normal;
            v.Position = pv.Pos + nrm * opts.Offset;
            v.Normal = nrm;
            v.TexCoords = glm::vec2(pv.Pos.x + 0.5f,
                                    opts.FlipV ? (0.5f - pv.Pos.y) : (pv.Pos.y + 0.5f));
            out.Vertices.push_back(v);
        }
        for (size_t k = 1; k + 1 < poly.size(); ++k) {
            out.Indices.push_back(base);
            out.Indices.push_back(base + (unsigned)k);
            out.Indices.push_back(base + (unsigned)k + 1);
        }
    }
    return out;
}

MeshData ProjectDecal(const MeshData& receiver, const glm::mat4& toDecal,
                      const DecalProjectOptions& opts) {
    return ProjectDecal(receiver.Vertices, receiver.Indices, toDecal, opts);
}

} // namespace sage::render
