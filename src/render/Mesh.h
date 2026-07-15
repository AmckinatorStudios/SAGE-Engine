#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

// Хранит геометрию на GPU (VAO/VBO/EBO) и умеет себя отрисовать
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
    ~Mesh();

    void Draw() const;

    static Mesh CreateCube();

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    size_t m_indexCount = 0;

    void SetupMesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
};
