#include "sage/gi/GI.h"

#include <algorithm>
#include <cstring>

#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/ModelLoader.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

// Сценозависимая часть модуля GI: сбор входа бейкера из ECS, отпечаток
// статичной геометрии и перенос бейка между снапшотами сцены. Чистая
// CPU-логика без GL (работает и в headless-тестах, и в фоновом потоке бейка
// вход уже не трогает сцену — он собран здесь заранее).
namespace sage::gi {

namespace {

// FNV-1a (64 бита) по сырым байтам — хватает как отпечаток «та же ли геометрия».
struct Fnv {
    uint64_t H = 1469598103934665603ull;
    void Add(const void* data, size_t bytes) {
        const unsigned char* p = (const unsigned char*)data;
        for (size_t i = 0; i < bytes; ++i) {
            H ^= p[i];
            H *= 1099511628211ull;
        }
    }
    template <typename T>
    void AddPod(const T& v) { Add(&v, sizeof(T)); }
};

// Геометрия по MeshRef (CPU, без GL). Модели — только .obj (CPU-стадия
// загрузчика); прочие форматы моделей в бейке пропускаются с предупреждением.
sage::render::MeshData ResolveMeshData(const MeshRef& ref) {
    using sage::render::MeshData;
    switch (ref.type) {
        case MeshRef::Type::Cube:     return sage::render::BuildCube();
        case MeshRef::Type::Sphere:   return sage::render::BuildSphere();
        case MeshRef::Type::Plane:    return sage::render::BuildPlane();
        case MeshRef::Type::Cylinder: return sage::render::BuildCylinder();
        case MeshRef::Type::Cone:     return sage::render::BuildCone();
        case MeshRef::Type::Model: {
            const std::string& p = ref.path;
            if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".obj") == 0) {
                try {
                    // Любой поддерживаемый формат, а не только .obj: иначе
                    // модель в glTF не отбрасывала бы запечённой тени и не
                    // получала бы лайтмапу — то есть на бейке исчезала бы.
                    return ModelLoader::LoadMeshData(p);
                } catch (const std::exception& e) {
                    LOG_WARN("GI") << "Модель не вошла в бейк (" << p << "): " << e.what();
                }
            } else if (!p.empty()) {
                LOG_WARN("GI") << "GI-бейк моделей поддерживает только .obj, пропуск: " << p;
            }
            return {};
        }
        default: return {};
    }
}

// Единый сбор статичных сущностей: и для входа бейкера, и для отпечатка.
// Сортировка по id — порядок итерации entt-реестра не гарантирован, а
// развёртка/упаковка обязаны быть детерминированными.
struct CollectedItem {
    int Id;
    entt::entity Entity;
    const GIStaticComponent* GIC;
    const MeshRendererComponent* MR;
};

std::vector<CollectedItem> CollectStatics(Scene& scene) {
    std::vector<CollectedItem> out;
    auto view = scene.Registry().view<GIStaticComponent, Transform, MeshRendererComponent>();
    for (auto e : view) {
        const auto& mr = view.get<MeshRendererComponent>(e);
        if (mr.Ref.type == MeshRef::Type::None) continue;
        const auto* idc = scene.Registry().try_get<IdComponent>(e);
        if (!idc) continue;
        out.push_back({idc->Id, e, &view.get<GIStaticComponent>(e), &mr});
    }
    std::sort(out.begin(), out.end(),
              [](const CollectedItem& a, const CollectedItem& b) { return a.Id < b.Id; });
    return out;
}

} // namespace

uint64_t ComputeGeometryHash(Scene& scene, const GISettings& s) {
    Fnv f;
    f.AddPod(s.TexelsPerUnit);
    f.AddPod(s.AtlasSize);
    f.AddPod(s.Seed);
    for (const CollectedItem& c : CollectStatics(scene)) {
        f.AddPod(c.Id);
        glm::mat4 world = scene.WorldMatrix(c.Entity);
        f.AddPod(world);
        f.AddPod(c.GIC->Lightmapped);
        f.AddPod(c.GIC->TexelScale);
        f.AddPod(c.MR->Ref.type);
        f.Add(c.MR->Ref.path.data(), c.MR->Ref.path.size());
    }
    return f.H;
}

BakeInput CollectBakeInput(Scene& scene, const GISettings& settings) {
    BakeInput in;
    in.Settings = settings;
    // Полное освещение кадра: окружение сцены + света-сущности (LightComponent),
    // как его видит realtime-рендер — бейк и рантайм считают один и тот же свет.
    in.Env = sage::ecs::CollectLighting(scene);

    for (const CollectedItem& c : CollectStatics(scene)) {
        StaticItem item;
        item.EntityId = c.Id;
        item.World = scene.WorldMatrix(c.Entity);
        item.Albedo = EffectiveColor(*c.MR);
        item.TexelScale = c.GIC->TexelScale > 0.0f ? c.GIC->TexelScale : 1.0f;
        item.Lightmapped = c.GIC->Lightmapped;
        item.Mesh = ResolveMeshData(c.MR->Ref);
        if (item.Mesh.Empty()) continue;
        in.Items.push_back(std::move(item));
    }
    in.GeometryHash = ComputeGeometryHash(scene, settings);
    return in;
}

void Transplant(Scene& from, Scene& to) {
    if (!from.GI || !from.GI->Baked) return;
    // Переносим только на геометрически идентичную сцену: иначе UV/страницы
    // не соответствуют сущностям и рендер получил бы мусор.
    uint64_t hash = ComputeGeometryHash(to, from.GI->Settings);
    if (hash != from.GI->GeometryHash) return;
    to.GI = from.GI;
}

} // namespace sage::gi
