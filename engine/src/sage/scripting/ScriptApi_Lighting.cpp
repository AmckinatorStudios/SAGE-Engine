#include "ScriptEngine.h"

#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// Освещение и отражения: sage.light.*, sage.reflect.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterLightingApi() {
    // --- Освещение сцены: солнце, ambient, туман — доступны после BindScene.
    // Скрипт-дирижёр катсцены/цикла дня меняет их прямо: GetLighting().Sun.
    // Intensity = 0.2, GetLighting().Fog.Enabled = true и т.п. ---
    m_lua.new_usertype<DirectionalLight>("DirectionalLight",
        "Direction", &DirectionalLight::Direction,
        "Color", &DirectionalLight::Color,
        "Intensity", &DirectionalLight::Intensity
    );
    m_lua.new_usertype<FogSettings>("FogSettings",
        "Enabled", &FogSettings::Enabled,
        "Color", &FogSettings::Color,
        "Start", &FogSettings::Start,
        "End", &FogSettings::End
    );
    // Само НЕБО, а не только засветка от него. Без этого цикл суток из скрипта
    // выходил половинчатым: свет, туман и вода темнели к ночи, а купол неба
    // оставался полуденно-синим — закат был виден везде, кроме собственно неба.
    m_lua.new_usertype<SkyboxSettings>("SkyboxSettings",
        "Enabled", &SkyboxSettings::Enabled,
        "TopColor", &SkyboxSettings::TopColor,
        "HorizonColor", &SkyboxSettings::HorizonColor,
        "Intensity", &SkyboxSettings::Intensity,
        "Rotation", &SkyboxSettings::RotationDeg,
        // Светила: направление берётся у солнца сцены, поэтому дублировать его
        // в скрипте не нужно — только включить и настроить вид.
        "Celestials", &SkyboxSettings::Celestials,
        "SunColor", &SkyboxSettings::SunColor,
        "SunSize", &SkyboxSettings::SunSize,
        "Moon", &SkyboxSettings::Moon,
        "MoonColor", &SkyboxSettings::MoonColor,
        "MoonSize", &SkyboxSettings::MoonSize,
        "StarIntensity", &SkyboxSettings::StarIntensity
    );
    // Дальность теней — свойство МИРА, а не настроек качества: масштаб сцены
    // знает игра. Ноль означает «взять из настроек движка» (см.
    // sage::ShadowSettings).
    m_lua.new_usertype<ShadowSettings>("ShadowSettings",
        "Distance", &ShadowSettings::Distance
    );
    m_lua.new_usertype<LightingEnvironment>("LightingEnvironment",
        "SkyColor", &LightingEnvironment::SkyColor,
        "GroundColor", &LightingEnvironment::GroundColor,
        "AmbientStrength", &LightingEnvironment::AmbientStrength,
        "Sun", &LightingEnvironment::Sun,
        "Fog", &LightingEnvironment::Fog,
        "Skybox", &LightingEnvironment::Skybox,
        "Shadows", &LightingEnvironment::Shadows
    );
    // --- Отражения ---------------------------------------------------------
    //
    // Игре тут решать две вещи: что отражать (небо само по себе или снятое
    // зондом окружение) и есть ли в сцене зеркальная плоскость. Всё остальное —
    // выбор мипа по шероховатости, Френель, параллакс — дело движка.
    Bind("reflect", "SetEnabled", "SetReflectionsEnabled", [this](bool on) {
        if (m_scene) m_scene->Reflections.Enabled = on;
    });
    Bind("reflect", "SetIntensity", "SetReflectionIntensity", [this](float v) {
        if (m_scene) m_scene->Reflections.Intensity = std::max(0.0f, v);
    });
    // Зеркальная плоскость: высота воды. Отражается ВСЯ сцена над ней.
    Bind("reflect", "SetWater", "SetWaterReflection", [this](float height) {
        if (!m_scene) return;
        m_scene->Reflections.PlanarEnabled = true;
        // dot((0,1,0), p) - height = 0 — плоскость y = height.
        m_scene->Reflections.Plane = glm::vec4(0.0f, 1.0f, 0.0f, -height);
    });
    // Произвольная плоскость (наклонное зеркало, витрина): нормаль + точка на ней.
    Bind("reflect", "SetPlanar", "SetPlanarReflection", [this](const glm::vec3& normal,
                                                     const glm::vec3& point) {
        if (!m_scene) return;
        const glm::vec3 n = glm::normalize(normal);
        m_scene->Reflections.PlanarEnabled = true;
        m_scene->Reflections.Plane = glm::vec4(n, -glm::dot(n, point));
    });
    Bind("reflect", "DisablePlanar", "DisablePlanarReflection", [this]() {
        if (m_scene) m_scene->Reflections.PlanarEnabled = false;
    });
    // Разрешение прохода отражения долей от кадра. Половина почти незаметна на
    // воде (её и так ломает рябь), а стоит вчетверо дешевле.
    Bind("reflect", "SetPlanarScale", "SetPlanarReflectionScale", [this](float scale) {
        if (m_scene) m_scene->Reflections.PlanarScale = glm::clamp(scale, 0.1f, 1.0f);
    });

    Bind("light", "Get", "GetLighting", [this]() -> LightingEnvironment& {
        if (!m_scene) throw std::runtime_error("GetLighting: сцена не привязана (BindScene не вызван)");
        return m_scene->Lighting;
    });
}

