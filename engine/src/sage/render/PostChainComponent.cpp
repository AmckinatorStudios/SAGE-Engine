#include "sage/render/PostChainComponent.h"

#include "sage/scene/Scene.h"

namespace sage::render {

PostChain ResolvePostChain(Scene& scene, entt::entity camera, const sage::EngineConfig& cfg) {
    if (camera != entt::null && scene.Registry().valid(camera)) {
        const PostChainComponent* component = scene.Registry().try_get<PostChainComponent>(camera);
        // Компонент есть и не просит умолчание — берём его тракт КАК ЕСТЬ.
        // Пустой тракт при этом тоже решение автора: он означает «только
        // тон-маппинг» (см. PostChain::Completed), а не «забыл настроить».
        if (component && !component->UseProjectDefault) return component->Chain;
    }
    return PostChain::FromConfig(cfg);
}

} // namespace sage::render
