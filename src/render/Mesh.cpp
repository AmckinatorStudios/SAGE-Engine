#include "Mesh.h"
#include "../core/Stats.h"

Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
    SetupMesh(vertices, indices);
}

Mesh::~Mesh() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
}

void Mesh::SetupMesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
    m_indexCount = indices.size();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Position));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

    glBindVertexArray(0);
}

void Mesh::Draw() const {
    ++g_renderStats.DrawCalls;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_indexCount), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

Mesh Mesh::CreateCube() {
    std::vector<Vertex> vertices = {
        // задняя грань (-Z)
        {{0.5f,-0.5f,-0.5f},{0,0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{0,0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{0,0,-1},{1,1}}, {{0.5f, 0.5f,-0.5f},{0,0,-1},{0,1}},
        // передняя грань (+Z)
        {{-0.5f,-0.5f, 0.5f},{0,0,1},{0,0}}, {{0.5f,-0.5f, 0.5f},{0,0,1},{1,0}},
        {{0.5f, 0.5f, 0.5f},{0,0,1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{0,0,1},{0,1}},
        // левая грань (-X)
        {{-0.5f, 0.5f, 0.5f},{-1,0,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{-1,0,0},{1,1}},
        {{-0.5f,-0.5f,-0.5f},{-1,0,0},{0,1}}, {{-0.5f,-0.5f, 0.5f},{-1,0,0},{0,0}},
        // правая грань (+X)
        {{0.5f, 0.5f,-0.5f},{1,0,0},{1,0}}, {{0.5f, 0.5f, 0.5f},{1,0,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{1,0,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{1,0,0},{0,0}},
        // нижняя грань (-Y)
        {{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{0,-1,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{0,-1,0},{1,0}}, {{-0.5f,-0.5f, 0.5f},{0,-1,0},{0,0}},
        // верхняя грань (+Y)
        {{-0.5f, 0.5f, 0.5f},{0,1,0},{0,1}}, {{0.5f, 0.5f, 0.5f},{0,1,0},{1,1}},
        {{0.5f, 0.5f,-0.5f},{0,1,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{0,1,0},{0,0}},
    };

    std::vector<unsigned int> indices;
    for (unsigned int face = 0; face < 6; ++face) {
        unsigned int o = face * 4;
        indices.insert(indices.end(), { o, o+1, o+2, o+2, o+3, o });
    }

    return Mesh(vertices, indices);
}
