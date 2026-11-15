#include "sage/scene/SceneLayer.h"

#include <algorithm>

#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneComponents.h"

namespace sage::scene {

void RegisterSceneLayerComponent() {
    RegisterComponent(MakeComponentType<SceneLayerComponent>(
        "scene_layer",
        [](const SceneLayerComponent& c) {
            nlohmann::json j;
            j["layer"] = c.Layer;
            return j;
        },
        [](const nlohmann::json& j, SceneLayerComponent& c) { c.Layer = j.value("layer", std::string()); }));
}

std::vector<std::string> SceneLayerNames(Scene& scene) {
    std::vector<std::string> out;
    for (const std::string& n : scene.Layers)
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    for (auto [e, c] : scene.Registry().view<SceneLayerComponent>().each())
        if (!c.Layer.empty() && std::find(out.begin(), out.end(), c.Layer) == out.end()) out.push_back(c.Layer);
    return out;
}

bool AddSceneLayer(Scene& scene, const std::string& name) {
    if (name.empty()) return false;
    const std::vector<std::string> all = SceneLayerNames(scene);
    if (std::find(all.begin(), all.end(), name) != all.end()) return false;
    scene.Layers.push_back(name);
    return true;
}

bool RenameSceneLayer(Scene& scene, const std::string& from, const std::string& to) {
    if (to.empty() || from == to) return false;
    const std::vector<std::string> all = SceneLayerNames(scene);
    if (std::find(all.begin(), all.end(), to) != all.end()) return false;
    bool found = false;
    for (std::string& n : scene.Layers)
        if (n == from) { n = to; found = true; }
    for (auto [e, c] : scene.Registry().view<SceneLayerComponent>().each())
        if (c.Layer == from) { c.Layer = to; found = true; }
    if (found && std::find(scene.Layers.begin(), scene.Layers.end(), to) == scene.Layers.end())
        scene.Layers.push_back(to);
    return found;
}

void RemoveSceneLayer(Scene& scene, const std::string& name) {
    scene.Layers.erase(std::remove(scene.Layers.begin(), scene.Layers.end(), name), scene.Layers.end());
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> clear;
    for (auto [e, c] : reg.view<SceneLayerComponent>().each())
        if (c.Layer == name) clear.push_back(e);
    for (entt::entity e : clear) reg.remove<SceneLayerComponent>(e);
}

std::string LayerOf(Scene& scene, int objectId) {
    GameObject o = scene.Get(objectId);
    if (!o.Valid()) return {};
    const SceneLayerComponent* c = scene.Registry().try_get<SceneLayerComponent>(o.Entity());
    return c ? c->Layer : std::string();
}

void SetLayerOf(Scene& scene, int objectId, const std::string& layer) {
    GameObject o = scene.Get(objectId);
    if (!o.Valid()) return;
    entt::registry& reg = scene.Registry();
    if (layer.empty()) {
        reg.remove<SceneLayerComponent>(o.Entity());
        return;
    }
    reg.emplace_or_replace<SceneLayerComponent>(o.Entity(), SceneLayerComponent{layer});
    // Назначенный слой — и в списке сцены: иначе он исчез бы из него, как
    // только объект из слоя убрали.
    if (std::find(scene.Layers.begin(), scene.Layers.end(), layer) == scene.Layers.end())
        scene.Layers.push_back(layer);
}

std::vector<std::string> SceneTags(Scene& scene) {
    std::vector<std::string> out;
    for (auto [e, t] : scene.Registry().view<TagComponent>().each())
        if (!t.Tag.empty() && std::find(out.begin(), out.end(), t.Tag) == out.end()) out.push_back(t.Tag);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace sage::scene
