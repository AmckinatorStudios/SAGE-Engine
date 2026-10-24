#include "sage/scene/ScaleLock.h"

#include "sage/scene/Components.h"
#include "sage/ui/UI.h"

namespace sage::scene {

ScaleLock ScaleLockOf(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return ScaleLock::None;
    // Есть форма — размер настоящий (см. заголовок).
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e))
        if (mr->Ref.type != MeshRef::Type::None) return ScaleLock::None;
    if (reg.any_of<ColliderComponent, DecalComponent, ParticleEmitterComponent, sage::ui::Element>(e))
        return ScaleLock::None;

    if (reg.all_of<FolderComponent>(e)) return ScaleLock::Folder;
    if (reg.all_of<CameraComponent>(e)) return ScaleLock::Camera;
    if (reg.all_of<LightComponent>(e)) return ScaleLock::Light;
    if (reg.all_of<AudioSourceComponent>(e)) return ScaleLock::Audio;
    if (reg.all_of<ReflectionProbeComponent>(e)) return ScaleLock::Probe;
    return ScaleLock::None;
}

} // namespace sage::scene
