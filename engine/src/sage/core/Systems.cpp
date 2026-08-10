#include "sage/core/Systems.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"

namespace sage {

const std::vector<SystemVersion>& EngineSystems() {
    // Все подсистемы пока на v1 — первая стабильная ревизия контракта. Порядок
    // — от фундамента к прикладным системам (как их видит инженер движка).
    static const std::vector<SystemVersion> systems = {
        {"Core",           1, "Application/Layer/Window, главный цикл и тайминг"},
        {"RHI",            1, "абстракция графики (бэкенды: OpenGL 3.3, Vulkan, Null)"},
        {"ECS / Scene",    1, "сущности+компоненты (entt), иерархия, мировые матрицы"},
        {"Serialization",  1, "сцены/материалы/конфиг в JSON (.sage/.sagemat/sage.cfg)"},
        {"Resources",      1, "кэш мешей/моделей/материалов/текстур"},
        {"Rendering",      1, "инстансный батч, фрустум-отсечение, PBR (Cook-Torrance)"},
        {"Shadows",        1, "карта теней направленного солнца"},
        {"Post-processing",1, "SSAO + Bloom + тон-маппинг ACES + виньетка"},
        {"Lighting",       1, "полусферический ambient, солнце, точечные, прожекторы"},
        {"Materials",      1, "metallic-roughness, normal/AO-карты (.sagemat)"},
        {"Skybox",         1, "процедурный градиентный скайбокс + туман"},
        {"Particles",      1, "инстансные billboard-частицы (залпы и струи)"},
        {"Animation",      1, "скелетная анимация (glTF-скины, палитра костей)"},
        {"Physics",        1, "PhysicsWorld: встроенный движок / Jolt / Null"},
        {"Audio",          1, "miniaudio: 2D/3D звук, музыка, группы громкости"},
        {"Scripting",      1, "Lua (sol2): доступ к системам/компонентам/сообщениям"},
        {"UI",             1, "SDF-виджеты, ECS UIElement, редактор интерфейса"},
        {"Fonts / Text",   1, "TrueType (stb_truetype), Unicode/кириллица"},
        {"Input",          1, "именованные действия и раскладки (InputMap)"},
        {"Config",         1, "EngineConfig + пресеты качества (Low..Ultra)"},
    };
    return systems;
}

void LogEngineSystems() {
    const auto& systems = EngineSystems();
    LOG_INFO("Engine") << "SAGE Engine " << kSageEngineVersion << " — подсистем: "
                       << systems.size() << " (все v1)";
    for (const SystemVersion& s : systems) {
        LOG_DEBUG("Engine") << "  " << s.Tag() << "  " << s.Name << " — " << s.Summary;
    }
}

} // namespace sage
