#include "sage/vars/Refs.h"

#include "sage/vars/VarsComponent.h"

namespace sage::vars {

void VisitEntityRefs(entt::registry& reg, entt::entity e,
                     const std::function<void(EntityRef&)>& visit) {
    if (!visit) return;

    // 1. Публичные переменные объекта.
    if (VarsComponent* vc = reg.try_get<VarsComponent>(e)) {
        for (Var& var : vc->Values.All()) {
            if (var.Data.Type() != Kind::Entity) continue;
            EntityRef ref = var.Data.AsEntity();
            visit(ref);
            var.Data = Value(ref);
        }
    }
    // Остальные носители ссылок (связи сигналов, компоненты игры) — слой
    // сцены: sage::scene::VisitEntityRefs (sage/scene/ComponentRegistry.h).
    // vars ниже сцены и знать её компоненты не может.
}

} // namespace sage::vars
