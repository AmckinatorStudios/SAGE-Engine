#include "sage/render/PrimitiveGenerator.h"

#include "sage/render/Mesh.h"
#include "sage/render/MeshData.h"

namespace sage::render {

std::shared_ptr<Mesh> PrimitiveGenerator::Get(MeshRef::Type type, bool keepCpuData) {
    const size_t index = (size_t)type;
    if (index >= kCount) return nullptr;
    std::shared_ptr<Mesh>& slot = m_defaults[index];
    if (slot) return slot;
    switch (type) {
        case MeshRef::Type::Cube: slot = std::make_shared<Mesh>(Mesh::CreateCube(keepCpuData)); break;
        case MeshRef::Type::Sphere: slot = std::make_shared<Mesh>(Mesh::CreateSphere(24, 32, keepCpuData)); break;
        case MeshRef::Type::Plane: slot = std::make_shared<Mesh>(Mesh::CreatePlane(1, keepCpuData)); break;
        case MeshRef::Type::Cylinder: slot = std::make_shared<Mesh>(Mesh::CreateCylinder(32, keepCpuData)); break;
        case MeshRef::Type::Cone: slot = std::make_shared<Mesh>(Mesh::CreateCone(32, keepCpuData)); break;
        case MeshRef::Type::Capsule: slot = std::make_shared<Mesh>(Mesh::CreateCapsule(12, 24, keepCpuData)); break;
        default: return nullptr;
    }
    return slot;
}

std::shared_ptr<Mesh> PrimitiveGenerator::Get(MeshRef::Type type, const PrimitiveParams& params,
                                              bool keepCpuData) {
    PrimitiveShape shape;
    if (!MeshRef::ShapeOf(type, shape)) return nullptr;
    if (params.SameAsDefault(shape)) return Get(type, keepCpuData);
    const std::string key = params.Key(shape);
    if (auto it = m_variants.find(key); it != m_variants.end()) return it->second;
    for (auto it = m_variants.begin(); it != m_variants.end();)
        it = it->second.use_count() == 1 ? m_variants.erase(it) : std::next(it);
    const MeshData data = BuildPrimitive(shape, params);
    auto mesh = std::make_shared<Mesh>(data.Vertices, data.Indices, keepCpuData);
    m_variants.emplace(key, mesh);
    return mesh;
}

void PrimitiveGenerator::Clear() {
    for (auto& primitive : m_defaults) primitive.reset();
    m_variants.clear();
}

} // namespace sage::render
