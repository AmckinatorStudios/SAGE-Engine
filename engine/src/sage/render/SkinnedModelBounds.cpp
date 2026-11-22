// Габариты скелетной модели в позе привязки — для кадрирования и посадки на
// пол в редакторе. Вынесено из SkinnedModel.cpp (предел размера файла).
#include <algorithm>
#include <cmath>
#include <limits>

#include "sage/render/ModelLoader.h"
#include "sage/render/SkinnedModel.h"

namespace sage::render {

// Габарит вершин подмеша — из тех же вершин, что уходят на видеокарту: после
// загрузки CPU-копии у модели нет.
void SkinnedSubMesh::SetBounds(const std::vector<SkinnedVertex>& vertices) {
    BoundsVertices = vertices.size();
    if (vertices.empty()) return;
    BoundsLo = glm::vec3(std::numeric_limits<float>::max());
    BoundsHi = glm::vec3(std::numeric_limits<float>::lowest());
    for (const SkinnedVertex& v : vertices) {
        BoundsLo = glm::min(BoundsLo, v.Position);
        BoundsHi = glm::max(BoundsHi, v.Position);
    }
}

bool SkinnedModel::BindBounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(std::numeric_limits<float>::max());
    hi = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const SkinnedSubMesh& s : m_subMeshes) {
        if (s.BoundsVertices == 0) continue;
        lo = glm::min(lo, s.BoundsLo);
        hi = glm::max(hi, s.BoundsHi);
        any = true;
    }
    return any;
}

bool SkinnedModel::RobustBindBounds(glm::vec3& lo, glm::vec3& hi) const {
    std::vector<::ModelLoader::PartBounds> parts;
    for (const SkinnedSubMesh& s : m_subMeshes) {
        if (s.BoundsVertices == 0) continue;
        parts.push_back({s.BoundsLo, s.BoundsHi, s.BoundsVertices});
    }
    if (parts.empty()) return false;
    ::ModelLoader::RobustBounds(parts, lo, hi);
    return true;
}

// ПОЧЕМУ ЭТО СТРОГАЯ ГРАНИЦА. Вершина в позе — взвешенная сумма Σ wᵢ·Bᵢ·v с
// весами, дающими в сумме единицу (вершина без весов — сама v). Каждое Bᵢ·v
// лежит в образе сферы привязки под костью i, значит вершина лежит в выпуклой
// оболочке этих образов — и в любой выпуклой фигуре, их накрывающей. Сфера
// ниже накрывает все образы (и саму позу привязки), так что ни одна вершина
// из неё не выходит, как бы клип ни двигал кости.
bool SkinnedModel::PoseBoundingSphere(const glm::mat4& entityModel,
                                      const std::vector<glm::mat4>& bones, glm::vec3& center,
                                      float& radius) const {
    glm::vec3 lo, hi;
    if (!BindBounds(lo, hi)) return false;
    const glm::vec3 bindCenter = (lo + hi) * 0.5f;
    const float bindRadius = glm::length(hi - lo) * 0.5f;
    const glm::mat4 model = entityModel * m_import;

    struct Ball { glm::vec3 C; float R; };
    std::vector<Ball> balls;
    balls.reserve(bones.size() + 1);
    auto add = [&](const glm::mat4& m) {
        const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                                      glm::length(glm::vec3(m[2]))});
        balls.push_back({glm::vec3(m * glm::vec4(bindCenter, 1.0f)), bindRadius * scale});
    };
    add(model); // вершины без весов остаются в позе привязки
    for (const glm::mat4& b : bones) add(model * b);

    glm::vec3 bl(std::numeric_limits<float>::max()), bh(std::numeric_limits<float>::lowest());
    for (const Ball& b : balls) {
        bl = glm::min(bl, b.C - glm::vec3(b.R));
        bh = glm::max(bh, b.C + glm::vec3(b.R));
    }
    center = (bl + bh) * 0.5f;
    radius = 0.0f;
    for (const Ball& b : balls) radius = std::max(radius, glm::length(b.C - center) + b.R);
    return std::isfinite(radius);
}

} // namespace sage::render
