#include "sage/scene/SceneSerializerDetail.h"

// Окружение сцены в JSON и обратно (см. SceneSerializerDetail.h).
namespace scene_serial {

json LightingToJson(const LightingEnvironment& lighting) {
    json j;
    j["ambientMode"] = (int)lighting.AmbientMode;
    j["ambientSky"] = Vec3ToJson(lighting.SkyColor);
    j["ambientGround"] = Vec3ToJson(lighting.GroundColor);
    j["ambientStrength"] = lighting.AmbientStrength;
    // Устаревшее поле — дублируем средним цветом для обратного чтения
    // старыми версиями движка/внешними тулами, которые ждут ambientColor.
    j["ambientColor"] = Vec3ToJson(lighting.AmbientColorApprox());

    j["sun"]["direction"] = Vec3ToJson(lighting.Sun.Direction);
    j["sun"]["color"] = Vec3ToJson(lighting.Sun.Color);
    j["sun"]["intensity"] = lighting.Sun.Intensity;

    json pointsJson = json::array();
    for (const PointLight& light : lighting.PointLights) {
        json pj;
        pj["position"] = Vec3ToJson(light.Position);
        pj["color"] = Vec3ToJson(light.Color);
        pj["intensity"] = light.Intensity;
        pj["range"] = light.Range;
        pointsJson.push_back(pj);
    }
    j["pointLights"] = pointsJson;

    // Прожекторы scene-level (обычно света-сущности, но окружение тоже может
    // нести свои — сериализуем для полноты).
    json spotsJson = json::array();
    for (const SpotLight& s : lighting.SpotLights) {
        json sj;
        sj["position"] = Vec3ToJson(s.Position);
        sj["direction"] = Vec3ToJson(s.Direction);
        sj["color"] = Vec3ToJson(s.Color);
        sj["intensity"] = s.Intensity;
        sj["range"] = s.Range;
        sj["innerCone"] = s.InnerAngleDeg;
        sj["outerCone"] = s.OuterAngleDeg;
        spotsJson.push_back(sj);
    }
    j["spotLights"] = spotsJson;

    // Атмосфера: туман + скайбокс.
    j["fog"]["enabled"] = lighting.Fog.Enabled;
    j["fog"]["color"] = Vec3ToJson(lighting.Fog.Color);
    j["fog"]["start"] = lighting.Fog.Start;
    j["fog"]["end"] = lighting.Fog.End;
    j["fog"]["mode"] = lighting.Fog.Kind == FogSettings::Mode::ExponentialHeight ? "height" : "linear";
    j["fog"]["density"] = lighting.Fog.Density;
    j["fog"]["heightFalloff"] = lighting.Fog.HeightFalloff;
    j["fog"]["baseHeight"] = lighting.Fog.BaseHeight;
    j["fog"]["maxOpacity"] = lighting.Fog.MaxOpacity;
    j["fog"]["sunScatter"] = lighting.Fog.SunScatter;
    j["fog"]["sunExponent"] = lighting.Fog.SunExponent;

    j["skybox"]["enabled"] = lighting.Skybox.Enabled;
    // РЕЖИМ НЕБА — явным полем. Раньше он выводился из того, пуст ли путь к
    // каталогу, и «процедурное небо + сохранённый путь к набору» описать было
    // нечем: путь приходилось стирать, то есть терять.
    j["skybox"]["mode"] = (int)lighting.Skybox.Kind;
    j["skybox"]["top"] = Vec3ToJson(lighting.Skybox.TopColor);
    j["skybox"]["horizon"] = Vec3ToJson(lighting.Skybox.HorizonColor);
    j["skybox"]["dayNight"] = lighting.Skybox.DayNight;
    j["skybox"]["nightTop"] = Vec3ToJson(lighting.Skybox.NightTopColor);
    j["skybox"]["nightHorizon"] = Vec3ToJson(lighting.Skybox.NightHorizonColor);
    j["skybox"]["dusk"] = Vec3ToJson(lighting.Skybox.DuskColor);
    j["skybox"]["moonlightColor"] = Vec3ToJson(lighting.Skybox.MoonlightColor);
    j["skybox"]["moonlightIntensity"] = lighting.Skybox.MoonlightIntensity;
    j["skybox"]["cubemapDir"] = lighting.Skybox.CubemapDir;
    j["skybox"]["image"] = lighting.Skybox.ImagePath;
    j["skybox"]["imageLayout"] = lighting.Skybox.ImageLayout;
    {
        json faces = json::array();
        for (int i = 0; i < 6; ++i) faces.push_back(lighting.Skybox.FacePaths[i]);
        j["skybox"]["faces"] = faces;
    }
    j["skybox"]["intensity"] = lighting.Skybox.Intensity;
    j["skybox"]["rotation"] = lighting.Skybox.RotationDeg;
    j["skybox"]["celestials"] = lighting.Skybox.Celestials;
    j["skybox"]["sunColor"] = Vec3ToJson(lighting.Skybox.SunColor);
    j["skybox"]["sunSize"] = lighting.Skybox.SunSize;
    j["skybox"]["moon"] = lighting.Skybox.Moon;
    j["skybox"]["moonColor"] = Vec3ToJson(lighting.Skybox.MoonColor);
    j["skybox"]["moonSize"] = lighting.Skybox.MoonSize;
    j["skybox"]["stars"] = lighting.Skybox.StarIntensity;
    {
        // Форма процедурного неба — отдельным объектом: полей много, и в
        // общей куче ключей неба их не найти глазами.
        const SkyboxSettings& s = lighting.Skybox;
        json& sh = j["skybox"]["shape"];
        sh["gradientExponent"] = s.GradientExponent;
        sh["horizonSoftness"] = s.HorizonSoftness;
        sh["horizonOffset"] = s.HorizonOffset;
        sh["ground"] = s.Ground;
        sh["groundColor"] = Vec3ToJson(s.GroundColor);
        sh["nightGroundColor"] = Vec3ToJson(s.NightGroundColor);
        sh["groundBlend"] = s.GroundBlend;
        sh["sunBrightness"] = s.SunBrightness;
        sh["sunGlow"] = s.SunGlow;
        sh["moonPhase"] = s.MoonPhase;
        sh["sunTexture"] = s.SunTexture;
        sh["moonTexture"] = s.MoonTexture;
        sh["filtering"] = sage::TextureFilteringKey(s.Filtering);
        sh["starDensity"] = s.StarDensity;
        sh["starSize"] = s.StarSize;
        json& cl = j["skybox"]["clouds"];
        cl["enabled"] = s.Clouds;
        cl["color"] = Vec3ToJson(s.CloudColor);
        cl["nightColor"] = Vec3ToJson(s.NightCloudColor);
        cl["height"] = s.CloudHeight;
        cl["scale"] = s.CloudScale;
        cl["coverage"] = s.CloudCoverage;
        cl["opacity"] = s.CloudOpacity;
        cl["wind"] = json::array({s.CloudWind.x, s.CloudWind.y});
        cl["fade"] = s.CloudFade;
    }

    // Дальность теней сцены. Ноль (по умолчанию) — «взять из настроек движка»,
    // и такие сцены ведут себя ровно как до появления поля.
    j["shadows"]["distance"] = lighting.Shadows.Distance;
    j["shadows"]["sunAngle"] = lighting.Shadows.SunAngle;

    // Цикл дня и ночи — своим объектом: это отдельная система, а не свойство
    // неба (работает при любом его типе).
    {
        const DayNightCycle& c = lighting.Cycle;
        json& cj = j["dayNightCycle"];
        cj["enabled"] = c.Enabled;
        cj["time"] = c.Time;
        cj["dayLengthMinutes"] = c.DayLengthMinutes;
        cj["speed"] = c.Speed;
        cj["runInPlay"] = c.RunInPlay;
        cj["sunAzimuth"] = c.SunAzimuth;
        cj["noonElevation"] = c.NoonElevation;
    }
    return j;
}

// Старые файлы сцен (сохранённые до появления освещения) не содержат
// "lighting" — тогда просто оставляем значения по умолчанию (см. Light.h).
LightingEnvironment LightingFromJson(const json& root) {
    LightingEnvironment lighting;
    if (!root.contains("lighting")) return lighting;

    const json& j = root["lighting"];
    if (j.contains("ambientSky") && j.contains("ambientGround")) {
        // Новый формат (после перехода на hemisphere ambient)
        lighting.SkyColor = Vec3FromJson(j["ambientSky"]);
        lighting.GroundColor = Vec3FromJson(j["ambientGround"]);
    } else if (j.contains("ambientColor")) {
        // Старый файл сцены (плоский ambient) — раскладываем один цвет
        // на sky/ground поровну, это даёт визуально то же самое, что было
        lighting.SetFlatAmbient(Vec3FromJson(j["ambientColor"]), lighting.AmbientStrength);
    }
    lighting.AmbientStrength = j.value("ambientStrength", lighting.AmbientStrength);
    {
        const int mode = j.value("ambientMode", (int)lighting.AmbientMode);
        lighting.AmbientMode = (mode == 1) ? LightingEnvironment::AmbientSource::Custom
                                           : LightingEnvironment::AmbientSource::FromSky;
    }

    if (j.contains("sun")) {
        const json& sj = j["sun"];
        if (sj.contains("direction")) lighting.Sun.Direction = Vec3FromJson(sj["direction"]);
        if (sj.contains("color")) lighting.Sun.Color = Vec3FromJson(sj["color"]);
        lighting.Sun.Intensity = sj.value("intensity", lighting.Sun.Intensity);
    }

    for (const auto& pj : j.value("pointLights", json::array())) {
        PointLight light;
        if (pj.contains("position")) light.Position = Vec3FromJson(pj["position"]);
        if (pj.contains("color")) light.Color = Vec3FromJson(pj["color"]);
        light.Intensity = pj.value("intensity", light.Intensity);
        light.Range = pj.value("range", light.Range);
        lighting.PointLights.push_back(light);
    }

    if (j.contains("shadows")) {
        lighting.Shadows.Distance = j["shadows"].value("distance", lighting.Shadows.Distance);
        lighting.Shadows.SunAngle = j["shadows"].value("sunAngle", lighting.Shadows.SunAngle);
    }
    // Нет ключа — сцена старше цикла: он выключен, время задаёт поворот солнца.
    if (j.contains("dayNightCycle") && j["dayNightCycle"].is_object()) {
        const json& cj = j["dayNightCycle"];
        DayNightCycle& c = lighting.Cycle;
        c.Enabled = cj.value("enabled", c.Enabled);
        c.Time = cj.value("time", c.Time);
        c.DayLengthMinutes = cj.value("dayLengthMinutes", c.DayLengthMinutes);
        c.Speed = cj.value("speed", c.Speed);
        c.RunInPlay = cj.value("runInPlay", c.RunInPlay);
        c.SunAzimuth = cj.value("sunAzimuth", c.SunAzimuth);
        c.NoonElevation = cj.value("noonElevation", c.NoonElevation);
    }

    for (const auto& sj : j.value("spotLights", json::array())) {
        SpotLight s;
        if (sj.contains("position")) s.Position = Vec3FromJson(sj["position"]);
        if (sj.contains("direction")) s.Direction = Vec3FromJson(sj["direction"]);
        if (sj.contains("color")) s.Color = Vec3FromJson(sj["color"]);
        s.Intensity = sj.value("intensity", s.Intensity);
        s.Range = sj.value("range", s.Range);
        s.InnerAngleDeg = sj.value("innerCone", s.InnerAngleDeg);
        s.OuterAngleDeg = sj.value("outerCone", s.OuterAngleDeg);
        lighting.SpotLights.push_back(s);
    }

    if (j.contains("fog")) {
        const json& fj = j["fog"];
        lighting.Fog.Enabled = fj.value("enabled", lighting.Fog.Enabled);
        if (fj.contains("color")) lighting.Fog.Color = Vec3FromJson(fj["color"]);
        lighting.Fog.Start = fj.value("start", lighting.Fog.Start);
        lighting.Fog.End = fj.value("end", lighting.Fog.End);
        lighting.Fog.Kind = fj.value("mode", std::string("linear")) == "height"
                                ? FogSettings::Mode::ExponentialHeight
                                : FogSettings::Mode::Linear;
        lighting.Fog.Density = fj.value("density", lighting.Fog.Density);
        lighting.Fog.HeightFalloff = fj.value("heightFalloff", lighting.Fog.HeightFalloff);
        lighting.Fog.BaseHeight = fj.value("baseHeight", lighting.Fog.BaseHeight);
        lighting.Fog.MaxOpacity = fj.value("maxOpacity", lighting.Fog.MaxOpacity);
        lighting.Fog.SunScatter = fj.value("sunScatter", lighting.Fog.SunScatter);
        lighting.Fog.SunExponent = fj.value("sunExponent", lighting.Fog.SunExponent);
    }
    if (j.contains("skybox")) {
        const json& sj = j["skybox"];
        lighting.Skybox.Enabled = sj.value("enabled", lighting.Skybox.Enabled);
        if (sj.contains("top")) lighting.Skybox.TopColor = Vec3FromJson(sj["top"]);
        if (sj.contains("horizon")) lighting.Skybox.HorizonColor = Vec3FromJson(sj["horizon"]);
        // НЕТРОНУТЫЕ ПРЕЖНИЕ УМОЛЧАНИЯ — ЭТО УМОЛЧАНИЯ, а не выбор человека.
        // Сцена сохраняет цвета неба всегда, и старая палитра (почти серая, в
        // кадре — «пасмурно») осталась бы во всех сценах навсегда, хотя её
        // никто не выбирал. Совпадение до бита с прежним умолчанием значит
        // «не трогали» — такие цвета переходят на нынешние умолчания.
        if (lighting.Skybox.TopColor == glm::vec3(0.30f, 0.45f, 0.75f) &&
            lighting.Skybox.HorizonColor == glm::vec3(0.70f, 0.80f, 0.92f)) {
            lighting.Skybox.TopColor = SkyboxSettings{}.TopColor;
            lighting.Skybox.HorizonColor = SkyboxSettings{}.HorizonColor;
        }
        lighting.Skybox.CubemapDir = sj.value("cubemapDir", lighting.Skybox.CubemapDir);
        lighting.Skybox.ImagePath = sj.value("image", lighting.Skybox.ImagePath);
        lighting.Skybox.ImageLayout = sj.value("imageLayout", lighting.Skybox.ImageLayout);
        if (sj.contains("faces") && sj["faces"].is_array()) {
            const json& fa = sj["faces"];
            for (int i = 0; i < 6 && i < (int)fa.size(); ++i)
                lighting.Skybox.FacePaths[i] = fa[(size_t)i].get<std::string>();
        }
        lighting.Skybox.DayNight = sj.value("dayNight", lighting.Skybox.DayNight);
        if (sj.contains("nightTop")) lighting.Skybox.NightTopColor = Vec3FromJson(sj["nightTop"]);
        if (sj.contains("nightHorizon"))
            lighting.Skybox.NightHorizonColor = Vec3FromJson(sj["nightHorizon"]);
        if (sj.contains("dusk")) lighting.Skybox.DuskColor = Vec3FromJson(sj["dusk"]);
        if (sj.contains("moonlightColor"))
            lighting.Skybox.MoonlightColor = Vec3FromJson(sj["moonlightColor"]);
        lighting.Skybox.MoonlightIntensity =
            sj.value("moonlightIntensity", lighting.Skybox.MoonlightIntensity);
        // РЕЖИМ. Явный — из файла; иначе выводим из того, что есть, ровно по
        // старому правилу: был путь к каталогу — значит небо было текстурным.
        // Так сцены, сохранённые до появления режимов, открываются с тем же
        // небом, что и раньше, а не с внезапным градиентом.
        if (sj.contains("mode")) {
            const int mode = sj.value("mode", 0);
            lighting.Skybox.Kind = (mode == 1)   ? SkyboxSettings::Source::Cubemap
                                   : (mode == 2) ? SkyboxSettings::Source::Faces
                                   : (mode == 3) ? SkyboxSettings::Source::Image
                                   : (mode == 4) ? SkyboxSettings::Source::Solid
                                                 : SkyboxSettings::Source::Procedural;
        } else if (!lighting.Skybox.CubemapDir.empty()) {
            lighting.Skybox.Kind = SkyboxSettings::Source::Cubemap;
        }
        lighting.Skybox.Intensity = sj.value("intensity", lighting.Skybox.Intensity);
        lighting.Skybox.RotationDeg = sj.value("rotation", lighting.Skybox.RotationDeg);
        lighting.Skybox.Celestials = sj.value("celestials", lighting.Skybox.Celestials);
        if (sj.contains("sunColor")) lighting.Skybox.SunColor = Vec3FromJson(sj["sunColor"]);
        lighting.Skybox.SunSize = sj.value("sunSize", lighting.Skybox.SunSize);
        lighting.Skybox.Moon = sj.value("moon", lighting.Skybox.Moon);
        if (sj.contains("moonColor")) lighting.Skybox.MoonColor = Vec3FromJson(sj["moonColor"]);
        lighting.Skybox.MoonSize = sj.value("moonSize", lighting.Skybox.MoonSize);
        lighting.Skybox.StarIntensity = sj.value("stars", lighting.Skybox.StarIntensity);
        SkyboxSettings& s = lighting.Skybox;
        if (sj.contains("shape") && sj["shape"].is_object()) {
            const json& sh = sj["shape"];
            s.GradientExponent = sh.value("gradientExponent", s.GradientExponent);
            s.HorizonSoftness = sh.value("horizonSoftness", s.HorizonSoftness);
            s.HorizonOffset = sh.value("horizonOffset", s.HorizonOffset);
            s.Ground = sh.value("ground", s.Ground);
            if (sh.contains("groundColor")) s.GroundColor = Vec3FromJson(sh["groundColor"]);
            if (sh.contains("nightGroundColor"))
                s.NightGroundColor = Vec3FromJson(sh["nightGroundColor"]);
            s.GroundBlend = sh.value("groundBlend", s.GroundBlend);
            s.SunBrightness = sh.value("sunBrightness", s.SunBrightness);
            s.SunGlow = sh.value("sunGlow", s.SunGlow);
            s.MoonPhase = sh.value("moonPhase", s.MoonPhase);
            s.SunTexture = sh.value("sunTexture", s.SunTexture);
            s.MoonTexture = sh.value("moonTexture", s.MoonTexture);
            // Фильтрация — словом. Старые сцены писали галку «pixelArt»:
            // читается как Nearest, чтобы небо не поменяло вид при загрузке.
            if (sh.contains("filtering") && sh["filtering"].is_string())
                sage::TextureFilteringFromKey(sh["filtering"].get<std::string>(), s.Filtering);
            else if (sh.value("pixelArt", false))
                s.Filtering = sage::TextureFiltering::Nearest;
            s.StarDensity = sh.value("starDensity", s.StarDensity);
            s.StarSize = sh.value("starSize", s.StarSize);
        }
        if (sj.contains("clouds") && sj["clouds"].is_object()) {
            const json& cl = sj["clouds"];
            s.Clouds = cl.value("enabled", s.Clouds);
            if (cl.contains("color")) s.CloudColor = Vec3FromJson(cl["color"]);
            if (cl.contains("nightColor")) s.NightCloudColor = Vec3FromJson(cl["nightColor"]);
            s.CloudHeight = cl.value("height", s.CloudHeight);
            s.CloudScale = cl.value("scale", s.CloudScale);
            s.CloudCoverage = cl.value("coverage", s.CloudCoverage);
            s.CloudOpacity = cl.value("opacity", s.CloudOpacity);
            if (cl.contains("wind") && cl["wind"].is_array() && cl["wind"].size() == 2)
                s.CloudWind = glm::vec2(cl["wind"][0].get<float>(), cl["wind"][1].get<float>());
            s.CloudFade = cl.value("fade", s.CloudFade);
        }
    }
    return lighting;
}

// ---------------------------------------------------------------------------
// Per-компонентная (де)сериализация. Раньше эти блоки жили ВНУТРИ двух гигантских
// циклов по сущностям (BuildSceneJson/BuildSceneFromJson); вынесены в парные
// Save*/Parse* функции, чтобы каждый компонент читался/правился независимо, а
// добавление нового компонента было локальной правкой (новая пара + одна строка
// в каждом цикле), а не вставкой в середину 100-строчного тела. Save* пишет свой
// под-объект в json сущности; Parse* собирает компонент из его под-json (вызывающий
// решает, навешивать ли его — по наличию ключа). Поведение идентично прежнему.
// ---------------------------------------------------------------------------


// Отражения сцены: сохраняется ЗАДАНИЕ, а не снятые кубы — они пересобираются
// при загрузке из того же неба и той же геометрии.
json ReflectionsToJson(const sage::render::ReflectionSettings& r) {
    json j;
    j["enabled"] = r.Enabled;
    j["intensity"] = r.Intensity;
    j["planarEnabled"] = r.PlanarEnabled;
    j["plane"] = Vec4ToJson(r.Plane);
    j["planarScale"] = r.PlanarScale;
    return j;
}

sage::render::ReflectionSettings ReflectionsFromJson(const json& root) {
    sage::render::ReflectionSettings r;
    if (!root.contains("reflections")) return r;
    const json& j = root["reflections"];
    r.Enabled = j.value("enabled", r.Enabled);
    r.Intensity = j.value("intensity", r.Intensity);
    r.PlanarEnabled = j.value("planarEnabled", r.PlanarEnabled);
    if (j.contains("plane")) r.Plane = Vec4FromJson(j["plane"], r.Plane);
    r.PlanarScale = j.value("planarScale", r.PlanarScale);
    return r;
}


// Общая сборка JSON-дерева сцены — используется и файловым Save, и SaveToString.
// Секция запечённого GI: настройки + отпечаток + (для файла) объём проб.
// Страницы атласа в JSON не кладутся — файловый Save пишет их рядом .hdr-файлами
// (gi::SavePages), а строковые снапшоты (undo/Play) переносят бейк указателем
// (gi::Transplant) — тащить мегабайты текселей в каждый снапшот незачем.
json GIToJson(const sage::gi::GIState& st, bool withProbes) {
    json g;
    g["settings"] = {
        {"texelsPerUnit", st.Settings.TexelsPerUnit},
        {"atlasSize", st.Settings.AtlasSize},
        {"sampleCount", st.Settings.SampleCount},
        {"bounces", st.Settings.Bounces},
        {"probeCellSize", st.Settings.ProbeCellSize},
        {"maxProbeAxis", st.Settings.MaxProbeAxis},
        {"seed", st.Settings.Seed},
    };
    g["baked"] = st.Baked;
    g["geometryHash"] = st.GeometryHash;
    g["pages"] = (int)st.Pages.size();
    if (withProbes && st.Probes.Valid()) {
        json pj;
        pj["dims"] = {st.Probes.Dims.x, st.Probes.Dims.y, st.Probes.Dims.z};
        pj["min"] = Vec3ToJson(st.Probes.Min);
        pj["cell"] = Vec3ToJson(st.Probes.CellSize);
        std::vector<float> data;
        data.reserve(st.Probes.Probes.size() * 12);
        for (const sage::gi::SH1& sh : st.Probes.Probes) {
            for (int k = 0; k < 4; ++k) data.push_back(sh.R[k]);
            for (int k = 0; k < 4; ++k) data.push_back(sh.G[k]);
            for (int k = 0; k < 4; ++k) data.push_back(sh.B[k]);
        }
        pj["sh"] = std::move(data);
        g["probes"] = std::move(pj);
    }
    return g;
}

std::shared_ptr<sage::gi::GIState> GIFromJson(const json& g) {
    auto st = std::make_shared<sage::gi::GIState>();
    if (g.contains("settings")) {
        const json& sj = g["settings"];
        st->Settings.TexelsPerUnit = sj.value("texelsPerUnit", st->Settings.TexelsPerUnit);
        st->Settings.AtlasSize = sj.value("atlasSize", st->Settings.AtlasSize);
        st->Settings.SampleCount = sj.value("sampleCount", st->Settings.SampleCount);
        st->Settings.Bounces = sj.value("bounces", st->Settings.Bounces);
        st->Settings.ProbeCellSize = sj.value("probeCellSize", st->Settings.ProbeCellSize);
        st->Settings.MaxProbeAxis = sj.value("maxProbeAxis", st->Settings.MaxProbeAxis);
        st->Settings.Seed = sj.value("seed", st->Settings.Seed);
    }
    st->Baked = g.value("baked", false);
    st->GeometryHash = g.value("geometryHash", (uint64_t)0);
    if (g.contains("probes")) {
        const json& pj = g["probes"];
        auto dims = pj.value("dims", std::vector<int>{0, 0, 0});
        if (dims.size() == 3 && dims[0] > 0 && dims[1] > 0 && dims[2] > 0) {
            st->Probes.Dims = {dims[0], dims[1], dims[2]};
            if (pj.contains("min")) st->Probes.Min = Vec3FromJson(pj["min"]);
            if (pj.contains("cell")) st->Probes.CellSize = Vec3FromJson(pj["cell"]);
            size_t count = (size_t)dims[0] * dims[1] * dims[2];
            auto data = pj.value("sh", std::vector<float>{});
            if (data.size() == count * 12) {
                st->Probes.Probes.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    sage::gi::SH1& sh = st->Probes.Probes[i];
                    for (int k = 0; k < 4; ++k) sh.R[k] = data[i * 12 + k];
                    for (int k = 0; k < 4; ++k) sh.G[k] = data[i * 12 + 4 + k];
                    for (int k = 0; k < 4; ++k) sh.B[k] = data[i * 12 + 8 + k];
                }
            } else {
                st->Probes.Dims = {0, 0, 0}; // битые данные — без объёма
            }
        }
    }
    return st;
}


} // namespace scene_serial
