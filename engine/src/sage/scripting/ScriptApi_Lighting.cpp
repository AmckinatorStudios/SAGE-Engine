#include "ScriptEngine.h"

#include "sage/core/Config.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"

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

    // --- Солнце: писать НАДО сюда, а не в GetLighting().Sun -----------------
    //
    // Направленный свет-СУЩНОСТЬ перекрывает солнце из настроек сцены (см.
    // ecs::CollectLighting), и в сценах, прошедших миграцию до пятой версии,
    // такая сущность есть всегда: её заводит сам движок. Скрипт, который
    // честно писал GetLighting().Sun.Direction каждый кадр, при этом не менял
    // НИЧЕГО — и понять это по картинке нельзя: цвет неба и туман он менял
    // теми же строками рядом, а солнце с тенями стояло на месте. Ровно так и
    // потерялся ход солнца в «Корабле»: сутки шли, небо краснело, тени не
    // двигались.
    //
    // Эта функция пишет ТУДА, ОТКУДА КАДР ЧИТАЕТ: есть направленный свет
    // сущностью — правим его (в том числе поворот, из которого берётся
    // направление), нет — правим настройки сцены.
    Bind("light", "SetSun", "SetSun", [this](sol::table t) {
        if (!m_scene) throw std::runtime_error("SetSun: сцена не привязана (BindScene не вызван)");

        const sol::optional<glm::vec3> dir = t["direction"];
        const sol::optional<glm::vec3> color = t["color"];
        const sol::optional<float> intensity = t["intensity"];

        // Тот же выбор солнца, что и у CollectLighting: наименьший id, то есть
        // первый направленный свет в иерархии. Иначе скрипт правил бы одно
        // солнце, а кадр рисовался бы по другому.
        entt::entity best = entt::null;
        int bestId = 0;
        auto view = m_scene->Registry().view<LightComponent, Transform>();
        for (auto e : view) {
            if (view.get<LightComponent>(e).Kind != LightComponent::Type::Directional) continue;
            const IdComponent* id = m_scene->Registry().try_get<IdComponent>(e);
            const int candidate = id ? id->Id : 0;
            if (best != entt::null && candidate >= bestId) continue;
            best = e;
            bestId = candidate;
        }

        if (best != entt::null) {
            LightComponent& lc = m_scene->Registry().get<LightComponent>(best);
            if (color) lc.Color = *color;
            if (intensity) lc.Intensity = *intensity;
            if (dir) {
                // Направление у сущности живёт в ПОВОРОТЕ: свет светит туда,
                // куда смотрит объект. Пишем поворот, а не воображаемое поле
                // направления, — иначе гизмо в редакторе показывало бы одно, а
                // кадр освещался бы по другому.
                m_scene->Registry().get<Transform>(best).Rotation =
                    sage::ecs::EulerFromForward(*dir);
            }
        }

        // Основание правим ВСЕГДА, даже когда сущность нашлась: удаление
        // солнца-объекта должно возвращать сцену к настройкам, а не к тем
        // значениям, что лежали в файле при загрузке.
        if (dir) m_scene->Lighting.Sun.Direction = *dir;
        if (color) m_scene->Lighting.Sun.Color = *color;
        if (intensity && best == entt::null) m_scene->Lighting.Sun.Intensity = *intensity;
    });

    // --- Объём: лучи и облака (см. render/Volumetrics.h) --------------------
    //
    // Из скрипта, а не только из файла настроек: включать самый дорогой проход
    // кадра решает игра, и решает по обстановке. У «Корабля» это, например,
    // время суток — на закате лучи и облака делают весь кадр, ночью их не
    // видно, а платить за них приходится одинаково.
    //
    // Таблицей с необязательными полями: игре почти всегда нужно поправить
    // одно-два числа, а не перечислять все двенадцать.
    Bind("volumetric", "Set", "SetVolumetrics", [](sol::table t) {
        sage::EngineConfig& cfg = sage::EngineConfig::Get();
        cfg.Volumetrics = t.get_or("enabled", cfg.Volumetrics);
        cfg.VolumetricShafts = t.get_or("shafts", cfg.VolumetricShafts);
        cfg.VolumetricClouds = t.get_or("clouds", cfg.VolumetricClouds);
        cfg.VolumetricDensity = t.get_or("density", cfg.VolumetricDensity);
        cfg.VolumetricIntensity = t.get_or("intensity", cfg.VolumetricIntensity);
        cfg.VolumetricSteps = t.get_or("steps", cfg.VolumetricSteps);
        cfg.CloudSteps = t.get_or("cloudSteps", cfg.CloudSteps);
        cfg.CloudCoverage = t.get_or("coverage", cfg.CloudCoverage);
        cfg.VolumetricScale = t.get_or("scale", cfg.VolumetricScale);
        // Дальность марша и убывание с высотой — главные ручки против
        // «молока»: у сцены обычно уже есть свой туман по глубине, и объём,
        // считающий дымку на всю сотню метров, складывается с ним и съедает
        // горизонт.
        cfg.VolumetricMaxDistance = t.get_or("maxDistance", cfg.VolumetricMaxDistance);
        cfg.VolumetricHeightFalloff = t.get_or("heightFalloff", cfg.VolumetricHeightFalloff);
        cfg.CloudBottom = t.get_or("cloudBottom", cfg.CloudBottom);
        cfg.CloudTop = t.get_or("cloudTop", cfg.CloudTop);
    });
    Bind("volumetric", "Enabled", "VolumetricsEnabled", []() -> bool {
        return sage::EngineConfig::Get().Volumetrics;
    });

    // --- Блик в объективе (см. render/LensFlare.h) --------------------------
    //
    // Из скрипта по той же причине, что и объём: блик уместен не всегда, и
    // решает это игра. У «Корабля» он живёт ровно на восходе и закате, когда
    // солнце низко и смотришь на него в упор, — днём в зените он был бы
    // грязью на весь кадр.
    Bind("lensflare", "Set", "SetLensFlare", [](sol::table t) {
        sage::EngineConfig& cfg = sage::EngineConfig::Get();
        cfg.LensFlare = t.get_or("enabled", cfg.LensFlare);
        cfg.LensFlareIntensity = t.get_or("intensity", cfg.LensFlareIntensity);
        cfg.LensFlareGhosts = t.get_or("ghosts", cfg.LensFlareGhosts);
        cfg.LensFlareGhostSpacing = t.get_or("ghostSpacing", cfg.LensFlareGhostSpacing);
        cfg.LensFlareGhostSize = t.get_or("ghostSize", cfg.LensFlareGhostSize);
        cfg.LensFlareBlades = t.get_or("blades", cfg.LensFlareBlades);
        cfg.LensFlareHalo = t.get_or("halo", cfg.LensFlareHalo);
        cfg.LensFlareHaloRadius = t.get_or("haloRadius", cfg.LensFlareHaloRadius);
        cfg.LensFlareStarburst = t.get_or("starburst", cfg.LensFlareStarburst);
        cfg.LensFlareGlare = t.get_or("glare", cfg.LensFlareGlare);
        cfg.LensFlareChroma = t.get_or("chroma", cfg.LensFlareChroma);
        cfg.LensFlareThreshold = t.get_or("threshold", cfg.LensFlareThreshold);
    });
    Bind("lensflare", "Enabled", "LensFlareEnabled", []() -> bool {
        return sage::EngineConfig::Get().LensFlare;
    });
}

