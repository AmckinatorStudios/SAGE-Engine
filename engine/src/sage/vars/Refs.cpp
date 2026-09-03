#include "sage/vars/Refs.h"

#include "sage/events/Events.h"
#include "sage/ui/UIPart.h"
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

    // 2. Адресаты связей событий — ПО РЕЕСТРУ ЧАСТЕЙ, а не по списку известных
    // компонентов: связи может носить не только Interactable, и своя часть игры
    // со списком связей обязана попасть сюда сама.
    for (const sage::ui::PartType& part : sage::ui::Parts()) {
        if (!part.Fields || !part.Has || !part.GetMutable || !part.Has(reg, e)) continue;
        void* data = part.GetMutable(reg, e);
        if (!data) continue;
        for (const sage::ui::PartField& f : *part.Fields) {
            if (f.Type != sage::ui::PartField::Kind::Bindings) continue;
            for (sage::events::Binding& b : sage::ui::FieldAs<sage::events::Bindings>(data, f))
                visit(b.Target);
        }
    }
}

} // namespace sage::vars
