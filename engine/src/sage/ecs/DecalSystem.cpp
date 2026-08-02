#include "sage/ecs/DecalSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include "sage/render/DecalProjection.h"
#include "sage/scene/Components.h"

namespace sage::ecs {

using sage::render::MeshData;

MeshData ReceiverGeometry(const MeshRendererComponent& mr) {
    switch (mr.Ref.type) {
        case MeshRef::Type::Cube:     return sage::render::BuildCube();
        case MeshRef::Type::Sphere:   return sage::render::BuildSphere();
        case MeshRef::Type::Plane:    return sage::render::BuildPlane();
        case MeshRef::Type::Cylinder: return sage::render::BuildCylinder();
        case MeshRef::Type::Cone:     return sage::render::BuildCone();
        case MeshRef::Type::Model: {
            // Только если копию геометрии просили сохранить при загрузке.
            // Иначе наклейку класть не на что: вершины остались на видеокарте,
            // и читать их обратно ради этого — дороже, чем вся проекция.
            if (!mr.MeshPtr) return {};
            const std::vector<Vertex>* v = mr.MeshPtr->CpuVertices();
            const std::vector<unsigned int>* i = mr.MeshPtr->CpuIndices();
            if (!v || !i) return {};
            MeshData out;
            out.Vertices = *v;
            out.Indices = *i;
            return out;
        }
        default: return {};
    }
}

std::shared_ptr<Mesh> MakeDecalMesh(const MeshData& data) {
    if (data.Empty()) return nullptr;
    return std::make_shared<Mesh>(data.Vertices, data.Indices);
}

DecalBuildStats BuildDecals(
    Scene& scene,
    const std::function<std::shared_ptr<Mesh>(const MeshData&)>& makeMesh) {
    DecalBuildStats stats;
    entt::registry& reg = scene.Registry();

    auto decals = reg.view<DecalComponent, MeshRendererComponent>();
    for (auto decalEntity : decals) {
        DecalComponent& dc = decals.get<DecalComponent>(decalEntity);
        if (!dc.Dirty) continue;

        const glm::mat4 decalToWorld = scene.WorldMatrix(decalEntity);
        const glm::mat4 worldToDecal = glm::inverse(decalToWorld);

        sage::render::DecalProjectOptions opts;
        opts.AngleLimitDeg = dc.AngleLimitDeg;
        opts.Offset = dc.Offset;

        MeshData built;
        auto receivers = reg.view<MeshRendererComponent>();
        for (auto receiver : receivers) {
            // Сама на себя наклейка не проецируется: её собственный меш —
            // результат прошлой сборки, и учесть его значило бы наращивать
            // геометрию с каждой пересборкой.
            if (receiver == decalEntity) continue;
            if (reg.all_of<DecalComponent>(receiver)) continue; // и на другие наклейки тоже

            const MeshRendererComponent& rmr = receivers.get<MeshRendererComponent>(receiver);
            const MeshData geom = ReceiverGeometry(rmr);
            if (geom.Empty()) continue;

            // Приёмник -> мир -> наклейка. Обе матрицы берутся у сцены, то есть
            // с учётом всей цепочки родителей: наклейка на борту корабля обязана
            // ехать вместе с ним, а не остаться в точке мира.
            const glm::mat4 toDecal = worldToDecal * scene.WorldMatrix(receiver);
            MeshData piece = sage::render::ProjectDecal(geom, toDecal, opts);
            if (piece.Empty()) continue;

            const unsigned base = (unsigned)built.Vertices.size();
            built.Vertices.insert(built.Vertices.end(), piece.Vertices.begin(),
                                  piece.Vertices.end());
            for (unsigned idx : piece.Indices) built.Indices.push_back(base + idx);
        }

        MeshRendererComponent& mr = decals.get<MeshRendererComponent>(decalEntity);
        dc.Triangles = (int)(built.Indices.size() / 3);
        stats.Triangles += dc.Triangles;
        ++stats.Rebuilt;
        dc.Dirty = false;

        if (built.Empty()) {
            mr.MeshPtr.reset(); // не на что легла — и рисовать нечего
            continue;
        }
        if (makeMesh) mr.MeshPtr = makeMesh(built);
    }
    return stats;
}

} // namespace sage::ecs
