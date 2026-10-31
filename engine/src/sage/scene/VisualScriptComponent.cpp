#include "sage/scene/VisualScriptComponent.h"

#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/SceneValueJson.h"

namespace sage::scene {

void RegisterVisualScriptComponent() {
    RegisterComponent(MakeComponentType<VisualScriptComponent>(
        "visual_script",
        [](const VisualScriptComponent& c) {
            nlohmann::json j;
            j["graph"] = c.Graph;
            if (!c.Vars.Empty()) j["vars"] = VarsToJson(c.Vars);
            return j;
        },
        [](const nlohmann::json& j, VisualScriptComponent& c) {
            c.Graph = j.value("graph", std::string());
            if (j.contains("vars")) VarsFromJson(j["vars"], c.Vars);
        }));
}

} // namespace sage::scene
