#include "sage/assets/import/MeshNormalize.h"

#include <algorithm>

namespace sage::assets {

using sage::render::MeshData;
// Vertex живёт в ГЛОБАЛЬНОМ пространстве имён (см. render/Mesh.h).

bool HasNormals(const MeshData& mesh) {
    for (const Vertex& v : mesh.Vertices)
        if (glm::dot(v.Normal, v.Normal) > 1e-8f) return true;
    return false;
}

void GenerateNormals(MeshData& mesh) {
    for (Vertex& v : mesh.Vertices) v.Normal = glm::vec3(0.0f);
    for (std::size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        const unsigned ia = mesh.Indices[i], ib = mesh.Indices[i + 1], ic = mesh.Indices[i + 2];
        if (ia >= mesh.Vertices.size() || ib >= mesh.Vertices.size() ||
            ic >= mesh.Vertices.size())
            continue;
        const glm::vec3 a = mesh.Vertices[ia].Position;
        const glm::vec3 b = mesh.Vertices[ib].Position;
        const glm::vec3 c = mesh.Vertices[ic].Position;
        // БЕЗ НОРМАЛИЗАЦИИ: длина векторного произведения равна удвоенной
        // площади треугольника, и она же — правильный вес при усреднении.
        // Нормализовав здесь, мы дали бы крошечному треугольнику тот же голос,
        // что и большому, и сглаживание поехало бы на неравномерной сетке.
        const glm::vec3 n = glm::cross(b - a, c - a);
        mesh.Vertices[ia].Normal += n;
        mesh.Vertices[ib].Normal += n;
        mesh.Vertices[ic].Normal += n;
    }
    for (Vertex& v : mesh.Vertices) {
        const float len = glm::length(v.Normal);
        // Вершина, не попавшая ни в один треугольник (или вырожденные соседи), —
        // не повод отдать в шейдер нулевой вектор: он даст чёрный пиксель и NaN
        // в освещении.
        v.Normal = len > 1e-8f ? v.Normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

bool HasTangents(const MeshData& mesh) {
    // Поле Vertex::Tangent инициализировано значением по умолчанию (1,0,0,1) —
    // именно оно и означает «касательной нет». Отличать приходится по нему:
    // отдельного признака у вершины нет, а заводить его ради импорта значило бы
    // раздувать вершину на каждом меше движка.
    for (const Vertex& v : mesh.Vertices) {
        const glm::vec3 t(v.Tangent);
        if (glm::dot(t, t) < 1e-8f) continue;
        if (std::abs(v.Tangent.x - 1.0f) > 1e-5f || std::abs(v.Tangent.y) > 1e-5f ||
            std::abs(v.Tangent.z) > 1e-5f)
            return true;
    }
    return false;
}

bool HasUV(const MeshData& mesh) {
    for (const Vertex& v : mesh.Vertices)
        if (std::abs(v.TexCoords.x) > 1e-8f || std::abs(v.TexCoords.y) > 1e-8f) return true;
    return false;
}

void GenerateTangents(MeshData& mesh) {
    if (mesh.Vertices.empty() || mesh.Indices.size() < 3) return;
    if (!HasUV(mesh)) return;   // без развёртки касательную не из чего считать
    BuildTangents(
        mesh.Vertices.size(), mesh.Indices,
        [&](std::size_t i) { return mesh.Vertices[i].Position; },
        [&](std::size_t i) { return mesh.Vertices[i].TexCoords; },
        [&](std::size_t i) { return mesh.Vertices[i].Normal; },
        [&](std::size_t i, glm::vec4 t) { mesh.Vertices[i].Tangent = t; });
}

void FlipWinding(MeshData& mesh) {
    for (std::size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
}

void NormalizeImportedMesh(MeshData& mesh) {
    if (mesh.Vertices.empty()) return;
    // ПОРЯДОК ВАЖЕН: касательные считаются ПО нормалям (ортогонализация к ним),
    // поэтому нормали достраиваются первыми.
    if (!HasNormals(mesh)) GenerateNormals(mesh);
    if (!HasTangents(mesh)) GenerateTangents(mesh);
}

} // namespace sage::assets
