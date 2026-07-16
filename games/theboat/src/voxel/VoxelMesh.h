#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "sage/rhi/Resources.h"

struct VoxelVertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec3 Color;
    glm::vec2 UV;
};

// Как Mesh, но с цветом на вершину вместо текстурных координат —
// удобно для вокселей, где у каждого блока просто свой цвет.
// Геометрия хранится через RHI (rhi::Geometry) — игровой код не знает,
// какой графический API под капотом.
class VoxelMesh {
public:
    VoxelMesh() = default;

    // GPU-ресурсом владеет единолично — копирование запрещено, перемещение
    // безопасно (unique_ptr переезжает).
    VoxelMesh(const VoxelMesh&) = delete;
    VoxelMesh& operator=(const VoxelMesh&) = delete;
    VoxelMesh(VoxelMesh&&) noexcept = default;
    VoxelMesh& operator=(VoxelMesh&&) noexcept = default;

    void Upload(const std::vector<VoxelVertex>& vertices, const std::vector<unsigned int>& indices);
    void Draw() const;
    bool IsEmpty() const { return m_indexCount == 0; }

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};
