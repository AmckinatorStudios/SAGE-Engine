#include "sage/scene/AiAgentComponent.h"

#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/SceneValueJson.h"

namespace sage::scene {

void RegisterAiAgentComponent() {
    RegisterComponent(MakeComponentType<AiAgentComponent>(
        "ai_agent",
        [](const AiAgentComponent& c) {
            nlohmann::json j;
            j["graph"] = c.Graph;
            j["shared"] = c.Shared;
            j["auto_start"] = c.AutoStart;
            if (!c.Memory.Empty()) j["memory"] = VarsToJson(c.Memory);
            return j;
        },
        [](const nlohmann::json& j, AiAgentComponent& c) {
            c.Graph = j.value("graph", std::string());
            c.Shared = j.value("shared", std::string("world"));
            c.AutoStart = j.value("auto_start", true);
            if (j.contains("memory")) VarsFromJson(j["memory"], c.Memory);
        }));
}

} // namespace sage::scene
