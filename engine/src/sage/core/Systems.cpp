#include "sage/core/Systems.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"

namespace sage {

const std::vector<SystemVersion>& EngineSystems() {
    // Мажорная версия подсистемы поднимается, когда её контракт ломается
    // несовместимо. Порядок — от фундамента к прикладным системам (как их
    // видит инженер движка).
    //
    // Input v2: ввод перестал быть «картой действий поверх опроса GLFW» и стал
    // лестницей «устройства -> раскладка -> действия -> события» с контекстами,
    // геймпадом и переназначением. Прежний InputMap/InputAction/InputSystem
    // удалён целиком, поэтому это именно смена контракта, а не дополнение.
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
        {"Input",          2, "устройства -> действия -> события; контексты, "
                              "геймпад, ремаппинг"},
        {"Events",         1, "шина событий: именная и типизированная, "
                              "правила «когда-если-то»"},
        {"Config",         1, "EngineConfig + пресеты качества (Low..Ultra)"},
    };
    return systems;
}

void LogEngineSystems() {
    const auto& systems = EngineSystems();
    LOG_INFO("Engine") << "SAGE Engine " << kSageEngineVersion << " — подсистем: "
                       << systems.size();
    for (const SystemVersion& s : systems) {
        LOG_DEBUG("Engine") << "  " << s.Tag() << "  " << s.Name << " — " << s.Summary;
    }
}

} // namespace sage
