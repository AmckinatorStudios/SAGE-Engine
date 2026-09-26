#include "sage/render/ParticleECS.h"

#include <functional>

#include "sage/render/ParticleSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::fx {

uint64_t EmitterKey(const Scene& scene, entt::entity e) {
    const uint64_t s = (uint64_t)std::hash<const void*>{}(&scene);
    return (s * 0x9E3779B97F4A7C15ull) ^ (uint64_t)entt::to_integral(e);
}

void UpdateEmitters(Scene& scene, ParticleSystem& sys, float dt) {
    auto view = scene.Registry().view<ParticleEmitterComponent, Transform>();
    for (auto e : view) {
        ParticleEmitterComponent& em = view.get<ParticleEmitterComponent>(e);
        const uint64_t key = EmitterKey(scene, e);
        // Выключенный объект не рождает частиц, но уже рождённые доживают.
        sys.SyncEmitter(key, em.Effect, scene.WorldMatrix(e), em.Playing && !scene.IsHidden(e));
        if (em.RestartRequested) sys.Restart(key);
        if (em.ClearRequested) sys.ClearEmitter(key);
        if (em.PendingEmit > 0) sys.EmitNow(key, em.PendingEmit);
        em.RestartRequested = em.ClearRequested = false;
        em.PendingEmit = 0;
    }
    sys.Update(dt);
}

} // namespace sage::fx
