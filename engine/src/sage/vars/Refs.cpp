#include "sage/vars/Refs.h"

#include "sage/events/Events.h"
#include "sage/ui/scene/UIScene.h"
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

    // 2. Связи «команда интерфейса -> событие»: у них есть адресат, и он
    //    такая же ссылка на объект. Без этого копия заготовки открывала бы
    //    дверь ОРИГИНАЛА — молча и убедительно.
    if (sage::ui::UIDocumentComponent* uc = reg.try_get<sage::ui::UIDocumentComponent>(e)) {
        for (sage::events::Binding& b : uc->Commands) visit(b.Target);
    }
}

} // namespace sage::vars
