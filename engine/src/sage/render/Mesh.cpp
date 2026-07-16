#include "Mesh.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
    m_indexCount = indices.size();

    VertexLayout layout;
    layout.Stride = sizeof(Vertex);
    layout.Attributes = {
        {0, 3, AttribType::Float, (int)offsetof(Vertex, Position)},
        {1, 3, AttribType::Float, (int)offsetof(Vertex, Normal)},
        {2, 2, AttribType::Float, (int)offsetof(Vertex, TexCoords)},
    };

    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(Vertex), /*dynamic=*/false);
    m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/false);
}

void Mesh::Draw() const {
    m_geometry->DrawIndexed(m_indexCount);
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
