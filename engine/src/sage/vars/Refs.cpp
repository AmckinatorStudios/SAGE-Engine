#include "sage/vars/Refs.h"

#include "sage/scene/Signals.h"
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

    // 2. Цели связей сигналов (см. sage/scene/Signals.h): копия кнопки из
    // префаба обязана звать СВОЮ дверь, а не дверь оригинала.
    if (sage::signals::SignalLinksComponent* sl = reg.try_get<sage::signals::SignalLinksComponent>(e))
        for (sage::signals::Link& link : sl->Links) visit(link.Target);
}

} // namespace sage::vars
