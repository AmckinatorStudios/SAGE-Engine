#include "Mesh.h"
#include "sage/render/MeshData.h"
#include "sage/rhi/GraphicsDevice.h"

#include <cmath>
#include <utility>
#include <glm/gtc/constants.hpp>

using namespace sage::rhi;

// Вычисляет касательные (tangent) для normal mapping из позиций и UV каждого
// треугольника; аккумулирует касательную И бинормаль по вершинам, затем
// ортонормирует касательную относительно нормали (Gram–Schmidt) и определяет
// ЗНАК ориентации (handedness) w = sign(dot(cross(N,T), B_accum)). Он пишется в
// Tangent.w и в шейдере даёт бинормаль cross(N,T)*w — без него грани с
// зеркальной UV-развёрткой показывали бы ИНВЕРТИРОВАННЫЙ рельеф нормал-карты.
static void ComputeTangents(std::vector<Vertex>& v, const std::vector<unsigned int>& idx) {
    std::vector<glm::vec3> tanAcc(v.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitAcc(v.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        glm::vec3 e1 = v[b].Position - v[a].Position;
        glm::vec3 e2 = v[c].Position - v[a].Position;
        glm::vec2 d1 = v[b].TexCoords - v[a].TexCoords;
        glm::vec2 d2 = v[c].TexCoords - v[a].TexCoords;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-8f) continue;
        float f = 1.0f / det;
        glm::vec3 t = f * (d2.y * e1 - d1.y * e2);
        glm::vec3 bt = f * (d1.x * e2 - d2.x * e1);
        tanAcc[a] += t; tanAcc[b] += t; tanAcc[c] += t;
        bitAcc[a] += bt; bitAcc[b] += bt; bitAcc[c] += bt;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        glm::vec3 n = v[i].Normal;
        glm::vec3 t = tanAcc[i];
        t = t - n * glm::dot(n, t); // ортогонализация к нормали
        if (glm::dot(t, t) < 1e-10f) {
            // Запасная касательная, перпендикулярная нормали.
            t = glm::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0, 1, 0), n) : glm::vec3(1, 0, 0);
        }
        t = glm::normalize(t);
        // Знак: если накопленная бинормаль смотрит противоположно cross(N,T) —
        // UV зеркальны на этой грани, w = -1 (иначе +1).
        float w = (glm::dot(glm::cross(n, t), bitAcc[i]) < 0.0f) ? -1.0f : 1.0f;
        v[i].Tangent = glm::vec4(t, w);
    }
}

// Приводит порядок обхода треугольников к CCW-СНАРУЖИ: если геометрическая
// нормаль грани (cross рёбер) противоположна усреднённой нормали её вершин,
// меняем местами два индекса. Вершинные нормали примитивов/моделей заданы
// наружу, поэтому это делает winding согласованным с ними. Без этого некоторые
// генераторы (сфера/цилиндр/конус) выдавали обратный порядок обхода, и back-face
// culling отсекал ЛИЦЕВЫЕ грани — объекты выглядели вывернутыми наизнанку
// (видна была дальняя внутренняя поверхность). Для уже корректных мешей (куб,
// правильные модели) — no-op.
static void FixWinding(const std::vector<Vertex>& v, std::vector<unsigned int>& idx) {
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        glm::vec3 faceN = glm::cross(v[b].Position - v[a].Position, v[c].Position - v[a].Position);
        glm::vec3 vtxN = v[a].Normal + v[b].Normal + v[c].Normal;
        if (glm::dot(faceN, vtxN) < 0.0f) std::swap(idx[i + 1], idx[i + 2]);
    }
}

Mesh::Mesh(const std::vector<Vertex>& verticesIn, const std::vector<unsigned int>& indicesIn,
           bool keepCpuData) {
    m_indexCount = indicesIn.size();

    std::vector<Vertex> vertices = verticesIn;
    std::vector<unsigned int> indices = indicesIn;
    FixWinding(vertices, indices);      // единый CCW-снаружи порядок (не даёт «вывернутых» мешей)
    ComputeTangents(vertices, indices); // касательные для normal mapping

    // Копию сохраняем ПОСЛЕ исправления обхода и подсчёта касательных: уровни
    // детализации должны строиться из той же геометрии, что ушла в GPU, иначе
    // грубый уровень отличался бы от подробного не только плотностью.
    if (keepCpuData) {
        m_cpuVertices = vertices;
        m_cpuIndices = indices;
    }

    VertexLayout layout;
    layout.Stride = sizeof(Vertex);
    layout.Attributes = {
        {0, 3, AttribType::Float, (int)offsetof(Vertex, Position)},
        {1, 3, AttribType::Float, (int)offsetof(Vertex, Normal)},
        {2, 2, AttribType::Float, (int)offsetof(Vertex, TexCoords)},
        {3, 4, AttribType::Float, (int)offsetof(Vertex, Tangent)},
        // Лайтмап-UV (GI): loc 11 — после per-instance атрибутов 4..10, чтобы
        // не перенумеровывать существующий инстанс-поток.
        {11, 2, AttribType::Float, (int)offsetof(Vertex, TexCoords2)},
    };
    // Per-instance поток (батчинг): модельная матрица (loc 4..7 — 4 строки) +
    // цвет (loc 8) + metallic (loc 9) + roughness (loc 10) + alpha (loc 12).
    // Divisor 1. (loc 11 занят вторым UV-каналом вершины — лайтмапой.)
    // Не-инстансная отрисовка эти локации не читает.
    layout.InstanceStride = sizeof(MeshInstance);
    layout.InstanceAttributes = {
        {4, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 0},
        {5, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 16},
        {6, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 32},
        {7, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 48},
        {8, 3, AttribType::Float, (int)offsetof(MeshInstance, Color)},
        {9, 1, AttribType::Float, (int)offsetof(MeshInstance, Metallic)},
        {10, 1, AttribType::Float, (int)offsetof(MeshInstance, Roughness)},
        {12, 1, AttribType::Float, (int)offsetof(MeshInstance, Alpha)},
        {13, 1, AttribType::Float, (int)offsetof(MeshInstance, PlanarReflectivity)},
    };

    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(Vertex), /*dynamic=*/false);
    m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/false);

    // Затравка инстанс-буфера одним элементом — чтобы enabled instance-атрибуты
    // не читали из пустого буфера при обычной (не инстансной) отрисовке.
    MeshInstance seed{};
    m_geometry->SetInstanceData(&seed, sizeof(seed));

    // Ограничивающая сфера в локальных координатах: центр — середина AABB,
    // радиус — макс. расстояние вершины от центра. Для отсечения по фрустуму.
    if (!vertices.empty()) {
        glm::vec3 lo = vertices[0].Position, hi = vertices[0].Position;
        for (const Vertex& v : vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
        m_boundsCenter = (lo + hi) * 0.5f;
        float r2 = 0.0f;
        for (const Vertex& v : vertices) r2 = glm::max(r2, glm::dot(v.Position - m_boundsCenter, v.Position - m_boundsCenter));
        m_boundsRadius = std::sqrt(r2);
    }
}

void Mesh::Draw() const {
    m_geometry->DrawIndexed(m_indexCount);
}

void Mesh::SetInstances(const MeshInstance* data, size_t count) const {
    m_geometry->SetInstanceData(data, count * sizeof(MeshInstance));
}

void Mesh::DrawInstances(size_t count) const {
    if (count == 0) return;
    m_geometry->DrawIndexedInstanced(m_indexCount, count);
}

// Генераторы примитивов живут в MeshData.cpp (CPU, без GL) — здесь только
// обёртки, заливающие ту же геометрию на GPU.
Mesh Mesh::CreateCube(bool keepCpu)          { auto d = sage::render::BuildCube();              return Mesh(d.Vertices, d.Indices, keepCpu); }
Mesh Mesh::CreateSphere(int rings, int sectors, bool keepCpu) { auto d = sage::render::BuildSphere(rings, sectors); return Mesh(d.Vertices, d.Indices, keepCpu); }
Mesh Mesh::CreatePlane(int subdivisions, bool keepCpu) { auto d = sage::render::BuildPlane(subdivisions); return Mesh(d.Vertices, d.Indices, keepCpu); }
Mesh Mesh::CreateCylinder(int sectors, bool keepCpu) { auto d = sage::render::BuildCylinder(sectors);   return Mesh(d.Vertices, d.Indices, keepCpu); }
Mesh Mesh::CreateCone(int sectors, bool keepCpu) { auto d = sage::render::BuildCone(sectors);       return Mesh(d.Vertices, d.Indices, keepCpu); }
