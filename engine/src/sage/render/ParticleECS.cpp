#include "sage/render/ParticleECS.h"

#include "sage/render/ParticleSystem.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

namespace sage::fx {

void UpdateEmitters(Scene& scene, ParticleSystem& sys, float dt) {
    auto view = scene.Registry().view<ParticleEmitterComponent, Transform>();
    for (auto e : view) {
        ParticleEmitterComponent& em = view.get<ParticleEmitterComponent>(e);
        if (!em.Active) continue;
        glm::vec3 pos = glm::vec3(scene.WorldMatrix(e)[3]); // мировая позиция (иерархия)

        if (em.Continuous) {
            // Непрерывная струя: EmissionRate частиц/сек через дробный накопитель.
            em.Accumulator += em.Config.EmissionRate * dt;
            int n = (int)em.Accumulator;
            if (n > 0) {
                em.Accumulator -= (float)n;
                sys.Burst(em.Config, pos, n);
            }
        } else {
            // Периодические залпы: BurstCount частиц каждые BurstInterval секунд.
            em.Accumulator += dt;
            if (em.BurstInterval > 0.0f && em.Accumulator >= em.BurstInterval) {
                em.Accumulator = 0.0f;
                sys.Burst(em.Config, pos, em.BurstCount);
            }
        }
    }
    sys.Update(dt);
}

} // namespace sage::fx
