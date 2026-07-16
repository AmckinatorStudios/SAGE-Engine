#include "VoxelMesh.h"
#include "sage/core/Stats.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

void VoxelMesh::Upload(const std::vector<VoxelVertex>& vertices, const std::vector<unsigned int>& indices) {
    m_geometry.reset();
    m_indexCount = indices.size();
    if (vertices.empty() || indices.empty()) return;

    VertexLayout layout;
    layout.Stride = sizeof(VoxelVertex);
    layout.Attributes = {
        {0, 3, AttribType::Float, (int)offsetof(VoxelVertex, Position)},
        {1, 3, AttribType::Float, (int)offsetof(VoxelVertex, Normal)},
        {2, 3, AttribType::Float, (int)offsetof(VoxelVertex, Color)},
        {3, 2, AttribType::Float, (int)offsetof(VoxelVertex, UV)},
    };

    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(VoxelVertex), /*dynamic=*/false);
    m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/false);
}

void VoxelMesh::Draw() const {
    if (m_indexCount == 0 || !m_geometry) return;
    ++g_renderStats.ChunksRendered; // DrawCalls считает сам бэкенд
    m_geometry->DrawIndexed(m_indexCount);
}
