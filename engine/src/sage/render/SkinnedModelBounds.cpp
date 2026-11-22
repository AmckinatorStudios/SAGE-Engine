// Габариты скелетной модели в позе привязки — для кадрирования и посадки на
// пол в редакторе. Вынесено из SkinnedModel.cpp (предел размера файла).
#include <algorithm>
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

} // namespace sage::render
