#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "sage/rhi/Resources.h"

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

// Хранит геометрию на GPU (через rhi::Geometry) и умеет себя отрисовать.
// О графическом API ничего не знает — вся работа с буферами у бэкенда.
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);

    // GPU-ресурсом владеет единолично (unique_ptr) — копирование запрещено,
    // перемещение безопасно и разрешено.
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    void Draw() const;

    static Mesh CreateCube();

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};
