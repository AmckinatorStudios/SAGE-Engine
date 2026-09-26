#include "sage/render/PostProcessComponent.h"

#include "sage/ecs/CameraLightComponents.h"
#include "sage/scene/Scene.h"

namespace sage::render {

PostProcessComponent* ActivePostComponent(Scene& scene, entt::entity camera) {
    entt::registry& reg = scene.Registry();
    // Свой компонент камеры — её решение, в том числе выключенный: «эта камера
    // показывает кадр как есть» (схема, отладочный вид).
    if (camera != entt::null && reg.valid(camera)) {
        if (PostProcessComponent* own = reg.try_get<PostProcessComponent>(camera)) return own;
    }
    // Иначе — компонент сцены: на объекте без камеры. Из нескольких включённых
    // побеждает больший приоритет, при равенстве — первый.
    PostProcessComponent* best = nullptr;
    for (auto [e, pp] : reg.view<PostProcessComponent>().each()) {
        if (!pp.Enabled || reg.all_of<CameraComponent>(e)) continue;
        if (!best || pp.Priority > best->Priority) best = &pp;
    }
    return best;
}

bool ResolvePostChain(Scene& scene, entt::entity camera, PostChain& out) {
    const PostProcessComponent* pp = ActivePostComponent(scene, camera);
    if (!pp || !pp->Enabled) return false;
    // Пустой тракт — тоже решение автора: он означает «только перевести кадр
    // в готовый вид» (см. PostChain::Completed), а не «забыл настроить».
    out = pp->Chain;
    return true;
}

} // namespace sage::render
