#include "sage/render/PostChainComponent.h"

#include "sage/core/Log.h"
#include "sage/render/PostChainIO.h"
#include "sage/scene/Scene.h"

namespace sage::render {

PostChain ProjectPostChain(const sage::EngineConfig& cfg) {
    if (cfg.PostChain.empty()) return PostChain::FromConfig(cfg);
    try {
        return PostChainFromJson(nlohmann::json::parse(cfg.PostChain));
    } catch (const std::exception& error) {
        // Битый текст тракта не должен оставить проект БЕЗ обработки вовсе:
        // собираем из полей и говорим, что именно не разобрали.
        LOG_WARN("Post") << "тракт проекта в sage.cfg не разобран (" << error.what()
                         << ") — собираю из полей настроек";
        return PostChain::FromConfig(cfg);
    }
}

PostChain ResolvePostChain(Scene& scene, entt::entity camera, const sage::EngineConfig& cfg) {
    if (camera != entt::null && scene.Registry().valid(camera)) {
        const PostChainComponent* component = scene.Registry().try_get<PostChainComponent>(camera);
        // Компонент есть и не просит умолчание — берём его тракт КАК ЕСТЬ.
        // Пустой тракт при этом тоже решение автора: он означает «только
        // тон-маппинг» (см. PostChain::Completed), а не «забыл настроить».
        if (component && !component->UseProjectDefault) return component->Chain;
    }
    return ProjectPostChain(cfg);
}

} // namespace sage::render
