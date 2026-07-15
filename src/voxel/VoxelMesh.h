#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

struct VoxelVertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec3 Color;
    glm::vec2 UV;
};

// Как Mesh, но с цветом на вершину вместо текстурных координат —
// удобно для вокселей, где у каждого блока просто свой цвет.
class VoxelMesh {
public:
    VoxelMesh() = default;
    ~VoxelMesh();

    // Запрещаем копирование (владеет GPU-ресурсами), разрешаем перемещение
    VoxelMesh(const VoxelMesh&) = delete;
    VoxelMesh& operator=(const VoxelMesh&) = delete;
    VoxelMesh(VoxelMesh&& other) noexcept;
    VoxelMesh& operator=(VoxelMesh&& other) noexcept;

    void Upload(const std::vector<VoxelVertex>& vertices, const std::vector<unsigned int>& indices);
    void Draw() const;
    bool IsEmpty() const { return m_indexCount == 0; }

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    size_t m_indexCount = 0;

    void Release();
};
