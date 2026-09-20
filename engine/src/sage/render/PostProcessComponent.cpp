#include "sage/render/PostProcessComponent.h"

#include "sage/scene/Scene.h"

namespace sage::render {

bool ResolvePostChain(Scene& scene, entt::entity camera, PostChain& out) {
    if (camera == entt::null || !scene.Registry().valid(camera)) return false;
    const PostProcessComponent* component = scene.Registry().try_get<PostProcessComponent>(camera);
    if (!component || !component->Enabled) return false;
    // Пустой тракт при этом тоже решение автора: он означает «только перевести
    // кадр в готовый вид» (см. PostChain::Completed), а не «забыл настроить».
    out = component->Chain;
    return true;
}

} // namespace sage::render
