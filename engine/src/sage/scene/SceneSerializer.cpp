#include "SceneSerializer.h"
#include "sage/assets/Pack.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/LodGroup.h"
#include "sage/gi/GI.h"
#include "sage/render/ResourceManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"

using json = nlohmann::json;

// Текущая версия формата сцены. Растёт при ЛОМАЮЩЕМ изменении: добавление
// необязательного поля версию не двигает, потому что старые файлы читаются без
// него как раньше.
constexpr int kSceneVersion = 7;

static json Vec3ToJson(const glm::vec3& v) {
    return json{ {"x", v.x}, {"y", v.y}, {"z", v.z} };
}

static glm::vec3 Vec3FromJson(const json& j) {
    return glm::vec3(j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f));
}

static json Vec4ToJson(const glm::vec4& v) {
    return json{ {"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w} };
}

static glm::vec4 Vec4FromJson(const json& j, const glm::vec4& def = glm::vec4(1.0f)) {
    return glm::vec4(j.value("x", def.x), j.value("y", def.y),
                     j.value("z", def.z), j.value("w", def.w));
}

static std::string MeshTypeToString(MeshRef::Type t) {
    switch (t) {
        case MeshRef::Type::Cube:     return "cube";
        case MeshRef::Type::Sphere:   return "sphere";
        case MeshRef::Type::Plane:    return "plane";
        case MeshRef::Type::Cylinder: return "cylinder";
        case MeshRef::Type::Cone:     return "cone";
        case MeshRef::Type::Model:    return "model";
        default: return "none";
    }
}

static MeshRef::Type MeshTypeFromString(const std::string& s) {
    if (s == "cube")     return MeshRef::Type::Cube;
    if (s == "sphere")   return MeshRef::Type::Sphere;
    if (s == "plane")    return MeshRef::Type::Plane;
    if (s == "cylinder") return MeshRef::Type::Cylinder;
    if (s == "cone")     return MeshRef::Type::Cone;
    if (s == "model")    return MeshRef::Type::Model;
    return MeshRef::Type::None;
}

// --- Ссылки на ассеты ---------------------------------------------------------
//
// Ассет опознаётся GUID'ом, а путь сохраняется РЯДОМ — как подсказка человеку в
// diff'е и как запасной вариант для проектов, сделанных до появления базы.
// Личность у файла одна, и это GUID: путь меняется от переименования, от
// переноса в другую папку, от наведения порядка, и каждое такое движение
// раньше молча ломало сцену.
//
// Формат: помимо "path"/"material"/"script" в объекте появляется парное поле
// с суффиксом "Guid". Старые сцены его не имеют — миграция v2->v3 проставляет
// его по путям, а Resolve всё равно умеет работать по одному только пути.
static void SaveAssetRef(json& j, const char* key, const std::string& path) {
    if (path.empty()) return;
    j[key] = path;
    const sage::AssetGuid guid = sage::AssetDatabase::Instance().GuidOf(path);
    if (guid.Valid()) j[std::string(key) + "Guid"] = guid.ToString();
}

// Возвращает АКТУАЛЬНЫЙ путь: по GUID, если он знаком (файл могли
// переименовать), иначе по сохранённому пути.
static std::string LoadAssetRef(const json& j, const char* key) {
    const std::string path = j.value(key, std::string());
    const std::string guidText = j.value(std::string(key) + "Guid", std::string());
    if (path.empty() && guidText.empty()) return {};
    const sage::AssetGuid guid = sage::AssetGuid::FromString(guidText);
    const std::string resolved = sage::AssetDatabase::Instance().Resolve(guid, path);
    // Пусто означает «ассета нет» — и об этом уже сказано в лог внутри Resolve.
    // Возвращаем исходный путь: пусть загрузчик попробует и сообщит по-своему,
    // это лучше, чем подсунуть ему пустую строку и получить «нет модели» без
    // упоминания, какой именно.
    return resolved.empty() ? path : resolved;
}

static json LightingToJson(const LightingEnvironment& lighting) {
    json j;
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

    j["skybox"]["enabled"] = lighting.Skybox.Enabled;
    j["skybox"]["top"] = Vec3ToJson(lighting.Skybox.TopColor);
    j["skybox"]["horizon"] = Vec3ToJson(lighting.Skybox.HorizonColor);
    j["skybox"]["cubemapDir"] = lighting.Skybox.CubemapDir;
    j["skybox"]["intensity"] = lighting.Skybox.Intensity;
    j["skybox"]["rotation"] = lighting.Skybox.RotationDeg;
    j["skybox"]["celestials"] = lighting.Skybox.Celestials;
    j["skybox"]["sunColor"] = Vec3ToJson(lighting.Skybox.SunColor);
    j["skybox"]["sunSize"] = lighting.Skybox.SunSize;
    j["skybox"]["moon"] = lighting.Skybox.Moon;
    j["skybox"]["moonColor"] = Vec3ToJson(lighting.Skybox.MoonColor);
    j["skybox"]["moonSize"] = lighting.Skybox.MoonSize;
    j["skybox"]["stars"] = lighting.Skybox.StarIntensity;

    // Дальность теней сцены. Ноль (по умолчанию) — «взять из настроек движка»,
    // и такие сцены ведут себя ровно как до появления поля.
    j["shadows"]["distance"] = lighting.Shadows.Distance;
    return j;
}

// Старые файлы сцен (сохранённые до появления освещения) не содержат
// "lighting" — тогда просто оставляем значения по умолчанию (см. Light.h).
static LightingEnvironment LightingFromJson(const json& root) {
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
    }
    if (j.contains("skybox")) {
        const json& sj = j["skybox"];
        lighting.Skybox.Enabled = sj.value("enabled", lighting.Skybox.Enabled);
        if (sj.contains("top")) lighting.Skybox.TopColor = Vec3FromJson(sj["top"]);
        if (sj.contains("horizon")) lighting.Skybox.HorizonColor = Vec3FromJson(sj["horizon"]);
        lighting.Skybox.CubemapDir = sj.value("cubemapDir", lighting.Skybox.CubemapDir);
        lighting.Skybox.Intensity = sj.value("intensity", lighting.Skybox.Intensity);
        lighting.Skybox.RotationDeg = sj.value("rotation", lighting.Skybox.RotationDeg);
        lighting.Skybox.Celestials = sj.value("celestials", lighting.Skybox.Celestials);
        if (sj.contains("sunColor")) lighting.Skybox.SunColor = Vec3FromJson(sj["sunColor"]);
        lighting.Skybox.SunSize = sj.value("sunSize", lighting.Skybox.SunSize);
        lighting.Skybox.Moon = sj.value("moon", lighting.Skybox.Moon);
        if (sj.contains("moonColor")) lighting.Skybox.MoonColor = Vec3FromJson(sj["moonColor"]);
        lighting.Skybox.MoonSize = sj.value("moonSize", lighting.Skybox.MoonSize);
        lighting.Skybox.StarIntensity = sj.value("stars", lighting.Skybox.StarIntensity);
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

static void SaveGIStatic(json& j, const GIStaticComponent& gs) {
    j["giStatic"]["lightmapped"] = gs.Lightmapped;
    j["giStatic"]["texelScale"] = gs.TexelScale;
}

static GIStaticComponent ParseGIStatic(const json& gj) {
    GIStaticComponent gs;
    gs.Lightmapped = gj.value("lightmapped", gs.Lightmapped);
    gs.TexelScale = gj.value("texelScale", gs.TexelScale);
    return gs;
}

static void SaveCamera(json& j, const CameraComponent& cam) {
    j["camera"]["projection"] =
        cam.Mode == CameraComponent::Projection::Orthographic ? "orthographic" : "perspective";
    j["camera"]["fov"] = cam.Fov;
    j["camera"]["orthoHeight"] = cam.OrthoHeight;
    j["camera"]["near"] = cam.NearClip;
    j["camera"]["far"] = cam.FarClip;
    j["camera"]["primary"] = cam.Primary;
}

static CameraComponent ParseCamera(const json& cj) {
    CameraComponent cam;
    // Сцена без поля projection — от версии до орто-камер: перспектива.
    cam.Mode = cj.value("projection", std::string("perspective")) == "orthographic"
                   ? CameraComponent::Projection::Orthographic
                   : CameraComponent::Projection::Perspective;
    cam.Fov = cj.value("fov", cam.Fov);
    cam.OrthoHeight = cj.value("orthoHeight", cam.OrthoHeight);
    cam.NearClip = cj.value("near", cam.NearClip);
    cam.FarClip = cj.value("far", cam.FarClip);
    cam.Primary = cj.value("primary", cam.Primary);
    return cam;
}

static const char* LightTypeToString(LightComponent::Type kind) {
    switch (kind) {
        case LightComponent::Type::Spot: return "spot";
        case LightComponent::Type::Directional: return "directional";
        default: return "point";
    }
}

static LightComponent::Type LightTypeFromString(const std::string& text) {
    if (text == "spot") return LightComponent::Type::Spot;
    if (text == "directional") return LightComponent::Type::Directional;
    return LightComponent::Type::Point;
}

static void SaveLight(json& j, const LightComponent& light) {
    j["light"]["type"] = LightTypeToString(light.Kind);
    j["light"]["color"] = Vec3ToJson(light.Color);
    j["light"]["intensity"] = light.Intensity;
    j["light"]["range"] = light.Range;
    j["light"]["innerCone"] = light.InnerConeDeg;
    j["light"]["outerCone"] = light.OuterConeDeg;
    j["light"]["castShadows"] = light.CastShadows;
}

static LightComponent ParseLight(const json& lj) {
    LightComponent light;
    light.Kind = LightTypeFromString(lj.value("type", std::string("point")));
    if (lj.contains("color")) light.Color = Vec3FromJson(lj["color"]);
    light.Intensity = lj.value("intensity", light.Intensity);
    light.Range = lj.value("range", light.Range);
    light.InnerConeDeg = lj.value("innerCone", light.InnerConeDeg);
    light.OuterConeDeg = lj.value("outerCone", light.OuterConeDeg);
    light.CastShadows = lj.value("castShadows", light.CastShadows);
    return light;
}

static void SaveRigidBody(json& j, const RigidBodyComponent& rb) {
    const char* types[] = {"static", "dynamic", "kinematic"};
    j["rigidBody"]["type"] = types[(int)rb.Type];
    j["rigidBody"]["mass"] = rb.Mass;
    j["rigidBody"]["friction"] = rb.Friction;
    j["rigidBody"]["restitution"] = rb.Restitution;
    j["rigidBody"]["layer"] = (unsigned)rb.Layer;
    j["rigidBody"]["sensor"] = rb.Sensor;
}

static RigidBodyComponent ParseRigidBody(const json& rj) {
    RigidBodyComponent rb;
    std::string type = rj.value("type", "dynamic");
    rb.Type = type == "static" ? sage::physics::BodyType::Static
            : type == "kinematic" ? sage::physics::BodyType::Kinematic
            : sage::physics::BodyType::Dynamic;
    rb.Mass = rj.value("mass", rb.Mass);
    rb.Friction = rj.value("friction", rb.Friction);
    rb.Restitution = rj.value("restitution", rb.Restitution);
    rb.Layer = rj.value("layer", (unsigned)rb.Layer);
    rb.Sensor = rj.value("sensor", rb.Sensor);
    return rb;
}

static const char* kShapeNames[] = {"box", "sphere", "capsule"};
static sage::physics::ShapeType ShapeFromStr(const std::string& s) {
    return s == "sphere" ? sage::physics::ShapeType::Sphere
         : s == "capsule" ? sage::physics::ShapeType::Capsule
         : sage::physics::ShapeType::Box;
}

static void SaveCollider(json& j, const ColliderComponent& col) {
    j["collider"]["shape"] = kShapeNames[(int)col.Shape];
    j["collider"]["halfExtents"] = Vec3ToJson(col.HalfExtents);
    j["collider"]["radius"] = col.Radius;
    j["collider"]["halfHeight"] = col.HalfHeight;
    if (!col.Parts.empty()) {
        json parts = json::array();
        for (const ColliderComponent::Part& p : col.Parts) {
            parts.push_back({
                {"shape", kShapeNames[(int)p.Shape]},
                {"halfExtents", Vec3ToJson(p.HalfExtents)},
                {"radius", p.Radius},
                {"halfHeight", p.HalfHeight},
                {"offset", Vec3ToJson(p.Offset)},
                {"euler", Vec3ToJson(p.EulerDeg)},
            });
        }
        j["collider"]["parts"] = std::move(parts);
    }
}

static ColliderComponent ParseCollider(const json& cj) {
    ColliderComponent col;
    col.Shape = ShapeFromStr(cj.value("shape", "box"));
    if (cj.contains("halfExtents")) col.HalfExtents = Vec3FromJson(cj["halfExtents"]);
    col.Radius = cj.value("radius", col.Radius);
    col.HalfHeight = cj.value("halfHeight", col.HalfHeight);
    if (cj.contains("parts") && cj["parts"].is_array()) {
        for (const json& pj : cj["parts"]) {
            ColliderComponent::Part p;
            p.Shape = ShapeFromStr(pj.value("shape", "box"));
            if (pj.contains("halfExtents")) p.HalfExtents = Vec3FromJson(pj["halfExtents"]);
            p.Radius = pj.value("radius", p.Radius);
            p.HalfHeight = pj.value("halfHeight", p.HalfHeight);
            if (pj.contains("offset")) p.Offset = Vec3FromJson(pj["offset"]);
            if (pj.contains("euler")) p.EulerDeg = Vec3FromJson(pj["euler"]);
            col.Parts.push_back(p);
        }
    }
    return col;
}

static const char* kJointNames[] = {"fixed", "point", "hinge", "slider", "distance", "cone"};
static void SaveJoint(json& j, const JointComponent& jc) {
    j["joint"]["type"] = kJointNames[(int)jc.Type];
    j["joint"]["targetId"] = jc.TargetId;
    j["joint"]["anchor"] = Vec3ToJson(jc.Anchor);
    j["joint"]["axis"] = Vec3ToJson(jc.Axis);
    j["joint"]["useLimits"] = jc.UseLimits;
    j["joint"]["minLimit"] = jc.MinLimit;
    j["joint"]["maxLimit"] = jc.MaxLimit;
    j["joint"]["minDistance"] = jc.MinDistance;
    j["joint"]["maxDistance"] = jc.MaxDistance;
    j["joint"]["coneHalfAngle"] = jc.ConeHalfAngle;
}

static JointComponent ParseJoint(const json& jj) {
    JointComponent jc;
    std::string t = jj.value("type", "point");
    using JT = sage::physics::JointType;
    jc.Type = t == "fixed" ? JT::Fixed : t == "hinge" ? JT::Hinge : t == "slider" ? JT::Slider
            : t == "distance" ? JT::Distance : t == "cone" ? JT::Cone : JT::Point;
    jc.TargetId = jj.value("targetId", -1);
    if (jj.contains("anchor")) jc.Anchor = Vec3FromJson(jj["anchor"]);
    if (jj.contains("axis")) jc.Axis = Vec3FromJson(jj["axis"]);
    jc.UseLimits = jj.value("useLimits", false);
    jc.MinLimit = jj.value("minLimit", jc.MinLimit);
    jc.MaxLimit = jj.value("maxLimit", jc.MaxLimit);
    jc.MinDistance = jj.value("minDistance", jc.MinDistance);
    jc.MaxDistance = jj.value("maxDistance", jc.MaxDistance);
    jc.ConeHalfAngle = jj.value("coneHalfAngle", jc.ConeHalfAngle);
    return jc;
}

// Отражения сцены: сохраняется ЗАДАНИЕ, а не снятые кубы — они пересобираются
// при загрузке из того же неба и той же геометрии.
static json ReflectionsToJson(const sage::render::ReflectionSettings& r) {
    json j;
    j["enabled"] = r.Enabled;
    j["intensity"] = r.Intensity;
    j["planarEnabled"] = r.PlanarEnabled;
    j["plane"] = Vec4ToJson(r.Plane);
    j["planarScale"] = r.PlanarScale;
    return j;
}

static sage::render::ReflectionSettings ReflectionsFromJson(const json& root) {
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

// Зонд отражений: сохраняется ЗАДАНИЕ (где, какой охват, какое разрешение), но
// не снятая карта — она пересобирается из той же геометрии и того же неба, а
// шесть картинок в проекте устаревали бы от любой правки уровня.
// Контроллер персонажа: сохраняются размеры и правила ходьбы, но не сам
// контроллер — он живёт в физическом мире и создаётся заново при запуске.
static void SaveCharacter(json& j, const CharacterControllerComponent& c) {
    j["character"]["radius"] = c.Radius;
    j["character"]["height"] = c.Height;
    j["character"]["stepHeight"] = c.StepHeight;
    j["character"]["maxSlopeDeg"] = c.MaxSlopeDeg;
    j["character"]["mass"] = c.Mass;
    j["character"]["layer"] = (unsigned)c.Layer;
}

static CharacterControllerComponent ParseCharacter(const json& cj) {
    CharacterControllerComponent c;
    c.Radius = cj.value("radius", c.Radius);
    c.Height = cj.value("height", c.Height);
    c.StepHeight = cj.value("stepHeight", c.StepHeight);
    c.MaxSlopeDeg = cj.value("maxSlopeDeg", c.MaxSlopeDeg);
    c.Mass = cj.value("mass", c.Mass);
    c.Layer = cj.value("layer", (unsigned)c.Layer);
    return c;
}

static void SaveReflectionProbe(json& j, const ReflectionProbeComponent& p) {
    j["reflectionProbe"]["resolution"] = p.Resolution;
    j["reflectionProbe"]["boxHalfExtents"] = Vec3ToJson(p.BoxHalfExtents);
    j["reflectionProbe"]["intensity"] = p.Intensity;
    j["reflectionProbe"]["boxParallax"] = p.BoxParallax;
    j["reflectionProbe"]["farClip"] = p.FarClip;
    j["reflectionProbe"]["realtime"] = p.Realtime;
}

static ReflectionProbeComponent ParseReflectionProbe(const json& pj) {
    ReflectionProbeComponent p;
    p.Resolution = pj.value("resolution", p.Resolution);
    if (pj.contains("boxHalfExtents")) p.BoxHalfExtents = Vec3FromJson(pj["boxHalfExtents"]);
    p.Intensity = pj.value("intensity", p.Intensity);
    p.BoxParallax = pj.value("boxParallax", p.BoxParallax);
    p.FarClip = pj.value("farClip", p.FarClip);
    p.Realtime = pj.value("realtime", p.Realtime);
    p.Dirty = true;   // после загрузки снять заново — сцена могла измениться
    return p;
}

// --- Уровни детализации ------------------------------------------------------
//
// Сериализуются НАСТРОЙКИ, а не сами упрощённые меши: геометрия уровней
// строится из исходного меша (BuildAutoLods) и хранить её в сцене — значит
// хранить копию модели, которая устареет от первой же её замены.
//
// До этого LodComponent не сохранялся ВООБЩЕ: пороги, выставленные для объекта,
// жили ровно до закрытия сцены. Ни сборка, ни тесты этого не замечали — рендер
// просто каждый раз строил уровни по умолчанию.
static void SaveLod(json& j, const sage::render::LodComponent& lod) {
    j["lod"]["auto"] = lod.Auto;
    j["lod"]["autoCount"] = lod.AutoCount;
    j["lod"]["cullBelow"] = lod.Levels.CullBelow;
    j["lod"]["screenHeights"] = lod.Levels.ScreenHeights;
}

static sage::render::LodComponent ParseLod(const json& lj) {
    sage::render::LodComponent lod;
    lod.Auto = lj.value("auto", lod.Auto);
    lod.AutoCount = lj.value("autoCount", lod.AutoCount);
    lod.Levels.CullBelow = lj.value("cullBelow", lod.Levels.CullBelow);
    if (lj.contains("screenHeights") && lj["screenHeights"].is_array()) {
        lod.Levels.ScreenHeights = lj["screenHeights"].get<std::vector<float>>();
    }
    return lod;
}

// --- Поправки параметров шейдера на ЭКЗЕМПЛЯР --------------------------------
//
// Тоже не сохранялись вовсе. Скрипт выставлял «мигает именно эта кнопка», это
// работало до сохранения сцены — и исчезало после. Хуже того, дефект выглядел
// как «параметр не применился», хотя применялся он исправно.
static void SaveShaderParams(json& j, const ShaderParamsComponent& sp) {
    json params = json::object();
    for (const auto& [name, value] : sp.Params) {
        params[name] = {{"kind", (int)value.Kind},
                        {"value", {value.Value.x, value.Value.y, value.Value.z, value.Value.w}}};
    }
    j["shaderParams"] = params;
}

static ShaderParamsComponent ParseShaderParams(const json& sj) {
    ShaderParamsComponent sp;
    if (!sj.is_object()) return sp;
    for (auto it = sj.begin(); it != sj.end(); ++it) {
        const json& v = it.value();
        if (!v.is_object() || !v.contains("value")) continue;
        ShaderParam param;
        param.Kind = (ShaderParam::Type)v.value("kind", 0);
        const json& arr = v["value"];
        if (arr.is_array() && arr.size() == 4) {
            param.Value = glm::vec4(arr[0].get<float>(), arr[1].get<float>(),
                                    arr[2].get<float>(), arr[3].get<float>());
        }
        sp.Params[it.key()] = param;
    }
    return sp;
}

static void SaveAnimatedModel(json& j, const AnimatedModelComponent& am) {
    // Только описательные поля — модель/палитра восстанавливаются загрузкой.
    SaveAssetRef(j["animatedModel"], "path", am.Path);
    j["animatedModel"]["demoSegments"] = am.DemoSegments;
    j["animatedModel"]["clip"] = am.Clip;
    j["animatedModel"]["speed"] = am.Speed;
    j["animatedModel"]["loop"] = am.Loop;
    j["animatedModel"]["playing"] = am.Playing;
    j["animatedModel"]["blendTime"] = am.BlendTime;
    j["animatedModel"]["rootMotion"] = am.RootMotion;
}

static AnimatedModelComponent ParseAnimatedModel(const json& aj) {
    AnimatedModelComponent am;
    am.Path = LoadAssetRef(aj, "path");
    am.DemoSegments = aj.value("demoSegments", 6);
    am.Clip = aj.value("clip", 0);
    am.Speed = aj.value("speed", 1.0f);
    am.Loop = aj.value("loop", true);
    am.Playing = aj.value("playing", true);
    am.BlendTime = aj.value("blendTime", 0.25f);
    am.RootMotion = aj.value("rootMotion", false);
    // Model/Anim восстановятся при первом UpdateAnimators (Ready=false).
    return am;
}

// IK: сохраняем только ЗАДАНИЕ (какая кость, куда тянем, как), но не результат.
// EndJoint/MidJoint/RootJoint — это индексы в конкретном скелете, они
// разрешаются заново после загрузки модели, а Locked/LockedAt — состояние
// текущего шага, которое переживать перезагрузку сцены не должно.
static void SaveIK(json& j, const IKComponent& ik) {
    j["ik"]["enabled"] = ik.Enabled;
    json goals = json::array();
    for (const IKGoal& g : ik.Goals) {
        json gj;
        gj["bone"] = g.Bone;
        gj["chainLength"] = g.ChainLength;
        gj["target"] = Vec3ToJson(g.Target);
        gj["usePole"] = g.UsePole;
        gj["pole"] = Vec3ToJson(g.Pole);
        gj["weight"] = g.Weight;
        gj["enabled"] = g.Enabled;
        gj["alignNormal"] = Vec3ToJson(g.AlignNormal);
        gj["aim"] = g.Aim;
        gj["aimAxis"] = Vec3ToJson(g.AimAxis);
        gj["aimMaxAngle"] = g.AimMaxAngle;
        gj["lock"] = g.Lock;
        gj["plantHeight"] = g.PlantHeight;
        gj["releaseTime"] = g.ReleaseTime;
        goals.push_back(std::move(gj));
    }
    j["ik"]["goals"] = std::move(goals);
}

static IKComponent ParseIK(const json& ij) {
    IKComponent ik;
    ik.Enabled = ij.value("enabled", true);
    if (ij.contains("goals") && ij["goals"].is_array()) {
        for (const json& gj : ij["goals"]) {
            IKGoal g;
            g.Bone = gj.value("bone", std::string());
            g.ChainLength = gj.value("chainLength", 2);
            if (gj.contains("target")) g.Target = Vec3FromJson(gj["target"]);
            g.UsePole = gj.value("usePole", false);
            if (gj.contains("pole")) g.Pole = Vec3FromJson(gj["pole"]);
            g.Weight = gj.value("weight", 1.0f);
            g.Enabled = gj.value("enabled", true);
            if (gj.contains("alignNormal")) g.AlignNormal = Vec3FromJson(gj["alignNormal"]);
            g.Aim = gj.value("aim", false);
            if (gj.contains("aimAxis")) g.AimAxis = Vec3FromJson(gj["aimAxis"]);
            g.AimMaxAngle = gj.value("aimMaxAngle", 80.0f);
            g.Lock = gj.value("lock", false);
            g.PlantHeight = gj.value("plantHeight", 0.12f);
            g.ReleaseTime = gj.value("releaseTime", 0.12f);
            ik.Goals.push_back(std::move(g));
        }
    }
    return ik;
}

// --- Интерфейс: ЧТЕНИЕ СТАРОГО ФОРМАТА ---------------------------------------
//
// Разбор блока "ui" с ключом "kind" — так элемент выглядел в файлах до перехода
// на компоненты. Пишется всё уже компонентами (см. ниже), поэтому здесь только
// чтение: однажды открытая и сохранённая сцена сюда больше не возвращается.

// --- Интерфейс: ССЫЛКА НА ДОКУМЕНТ -------------------------------------------
//
// Раньше здесь лежал САМ интерфейс: у каждого объекта сцены блок "ui" на сорок
// полей, позже — набор частей. Интерфейса в сцене больше нет: он живёт
// отдельным документом (.uidoc), а в сцене остаётся ССЫЛКА на него.
//
// Старые сцены этот загрузчик как интерфейс уже не читает — и не должен: делать
// вид, что старая модель плавно переходит в новую, значит получить третью. Для
// них есть отдельный инструмент переезда, работающий с ФАЙЛОМ сцены
// (sage/ui/serialization/UIMigration.h). Он намеренно не зависит от системы, из
// которой переезжают, — иначе умер бы вместе с ней ровно тогда, когда нужен.


static json Vec2ToJson(const glm::vec2& v) { return json{{"x", v.x}, {"y", v.y}}; }

static glm::vec2 Vec2FromJson(const json& j, const glm::vec2& fallback) {
    if (!j.is_object()) return fallback;
    return {j.value("x", fallback.x), j.value("y", fallback.y)};
}

// --- Значения, переменные и связи (sage/vars, sage/events) -------------------
//
// Один разбор на все три задачи: публичная переменная объекта, аргумент
// события и связь кнопки — это одно и то же значение под именем. Раньше каждая
// такая вещь заводила бы свою пару «запись/чтение», и рано или поздно они
// расходятся: поле сохраняется, но не читается, и молча сбрасывается.
static json ValueToJson(const sage::vars::Value& v) {
    using K = sage::vars::Kind;
    json j;
    // ВИД ПИШЕТСЯ ИМЕНЕМ, а не номером: номер сломался бы от вставки нового
    // вида в середину перечисления, и старые сцены прочитались бы наизнанку.
    j["kind"] = sage::vars::KindId(v.Type());
    switch (v.Type()) {
        case K::Bool: j["value"] = v.AsBool(); break;
        case K::Int: j["value"] = v.AsInt(); break;
        case K::Float: j["value"] = v.AsFloat(); break;
        case K::String: j["value"] = v.AsString(); break;
        case K::Vec2: j["value"] = Vec2ToJson(v.AsVec2()); break;
        case K::Vec3: j["value"] = Vec3ToJson(v.AsVec3()); break;
        case K::Color: j["value"] = Vec4ToJson(v.AsVec4()); break;
        case K::Entity: j["value"] = v.AsEntity().Id; break;
        case K::Asset: j["value"] = v.AsAsset().Path; break;
    }
    return j;
}

static sage::vars::Value ValueFromJson(const json& j) {
    using K = sage::vars::Kind;
    K kind = K::Bool;
    if (!j.is_object() || !j.contains("kind") || !j["kind"].is_string() ||
        !sage::vars::ParseKind(j["kind"].get<std::string>(), kind)) {
        return sage::vars::Value();
    }
    const json v = j.value("value", json());
    switch (kind) {
        case K::Bool: return sage::vars::Value(v.is_boolean() ? v.get<bool>() : false);
        case K::Int: return sage::vars::Value(v.is_number() ? v.get<int>() : 0);
        case K::Float: return sage::vars::Value(v.is_number() ? v.get<float>() : 0.0f);
        case K::String:
            return sage::vars::Value(v.is_string() ? v.get<std::string>() : std::string());
        case K::Vec2: return sage::vars::Value(Vec2FromJson(v, glm::vec2(0.0f)));
        case K::Vec3: return sage::vars::Value(v.is_object() ? Vec3FromJson(v) : glm::vec3(0.0f));
        case K::Color: return sage::vars::Value(Vec4FromJson(v, glm::vec4(1.0f)));
        case K::Entity:
            return sage::vars::Value(sage::vars::EntityRef{v.is_number() ? v.get<int>() : 0});
        case K::Asset:
            return sage::vars::Value(
                sage::vars::AssetRef{v.is_string() ? v.get<std::string>() : std::string()});
    }
    return sage::vars::Value();
}

static json VarsToJson(const sage::vars::Table& table) {
    // МАССИВ, а не объект: порядок переменных — это порядок в инспекторе, и
    // объект JSON его не обещает.
    json out = json::array();
    for (const sage::vars::Var& var : table.All()) {
        json j = ValueToJson(var.Data);
        j["name"] = var.Name;
        // Описание пишется, только если оно есть: у переменной, заведённой
        // руками, его нет, и пустые ключи в файле — это шум.
        if (!var.Label.empty()) j["label"] = var.Label;
        if (!var.Tooltip.empty()) j["tooltip"] = var.Tooltip;
        if (var.Min != var.Max) { j["min"] = var.Min; j["max"] = var.Max; }
        if (var.Declared) j["declared"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

static void VarsFromJson(const json& in, sage::vars::Table& table) {
    table.Clear();
    if (!in.is_array()) return;
    for (const json& j : in) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) continue;
        sage::vars::Var var;
        var.Name = j["name"].get<std::string>();
        var.Data = ValueFromJson(j);
        var.Label = j.value("label", std::string());
        var.Tooltip = j.value("tooltip", std::string());
        var.Min = j.value("min", 0.0f);
        var.Max = j.value("max", 0.0f);
        var.Declared = j.value("declared", false);
        table.Put(var);
    }
}

static json BindingsToJson(const sage::events::Bindings& bindings) {
    json out = json::array();
    for (const sage::events::Binding& b : bindings) {
        json j;
        j["trigger"] = b.Trigger;
        if (!b.Event.empty()) j["event"] = b.Event;
        if (b.Target.Valid()) j["target"] = b.Target.Id;
        if (!b.Method.empty()) j["method"] = b.Method;
        j["arg"] = ValueToJson(b.Arg);
        if (!b.Enabled) j["enabled"] = false;
        out.push_back(std::move(j));
    }
    return out;
}

static void BindingsFromJson(const json& in, sage::events::Bindings& out) {
    out.clear();
    if (!in.is_array()) return;
    for (const json& j : in) {
        if (!j.is_object()) continue;
        sage::events::Binding b;
        b.Trigger = j.value("trigger", std::string());
        b.Event = j.value("event", std::string());
        b.Target.Id = j.value("target", 0);
        b.Method = j.value("method", std::string());
        if (j.contains("arg")) b.Arg = ValueFromJson(j["arg"]);
        b.Enabled = j.value("enabled", true);
        out.push_back(std::move(b));
    }
}

// Одно поле части -> json.
// Ссылка сцены на документ интерфейса.
static void SaveUIDocument(json& j, const sage::ui::UIDocumentComponent& c) {
    json& uj = j["ui"];
    uj["document"] = c.Path;
    uj["sortOrder"] = c.SortOrder;
    uj["interactive"] = c.Interactive;
    uj["visible"] = c.Visible;
    // Связи «команда -> событие» пишутся, только если они есть: пустой массив в
    // каждом объекте с интерфейсом — шум в файле, который потом читают глазами.
    if (!c.Commands.empty()) uj["commands"] = BindingsToJson(c.Commands);
}

static void LoadUIDocument(const json& uj, sage::ui::UIDocumentComponent& c) {
    c.Path = uj.value("document", std::string());
    c.SortOrder = uj.value("sortOrder", 0);
    c.Interactive = uj.value("interactive", true);
    c.Visible = uj.value("visible", true);
    c.Commands.clear();
    if (uj.contains("commands")) BindingsFromJson(uj["commands"], c.Commands);
}


static void SaveParticles(json& j, const ParticleEmitterComponent& pe) {
    json& pj = j["particles"];
    pj["preset"] = pe.Preset;
    pj["active"] = pe.Active;
    pj["continuous"] = pe.Continuous;
    pj["burstCount"] = pe.BurstCount;
    pj["burstInterval"] = pe.BurstInterval;
    const ParticleEmitterConfig& c = pe.Config;
    pj["directionMin"] = Vec3ToJson(c.DirectionMin);
    pj["directionMax"] = Vec3ToJson(c.DirectionMax);
    pj["speedMin"] = c.SpeedMin; pj["speedMax"] = c.SpeedMax;
    pj["gravity"] = c.Gravity;
    pj["lifetimeMin"] = c.LifetimeMin; pj["lifetimeMax"] = c.LifetimeMax;
    pj["startSizeMin"] = c.StartSizeMin; pj["startSizeMax"] = c.StartSizeMax;
    pj["endSizeMin"] = c.EndSizeMin; pj["endSizeMax"] = c.EndSizeMax;
    pj["startColor"] = Vec4ToJson(c.StartColor);
    pj["endColor"] = Vec4ToJson(c.EndColor);
    pj["angularVelocityMax"] = c.AngularVelocityMax;
    pj["shape"] = (c.Shape == ParticleShape::Quad) ? "quad" : "circle";
    pj["emissionRate"] = c.EmissionRate;
}

static ParticleEmitterComponent ParseParticles(const json& pj) {
    ParticleEmitterComponent pe;
    pe.Preset = pj.value("preset", 0);
    pe.Active = pj.value("active", true);
    pe.Continuous = pj.value("continuous", true);
    pe.BurstCount = pj.value("burstCount", 24);
    pe.BurstInterval = pj.value("burstInterval", 1.5f);
    ParticleEmitterConfig& c = pe.Config;
    if (pj.contains("directionMin")) c.DirectionMin = Vec3FromJson(pj["directionMin"]);
    if (pj.contains("directionMax")) c.DirectionMax = Vec3FromJson(pj["directionMax"]);
    c.SpeedMin = pj.value("speedMin", c.SpeedMin);
    c.SpeedMax = pj.value("speedMax", c.SpeedMax);
    c.Gravity = pj.value("gravity", c.Gravity);
    c.LifetimeMin = pj.value("lifetimeMin", c.LifetimeMin);
    c.LifetimeMax = pj.value("lifetimeMax", c.LifetimeMax);
    c.StartSizeMin = pj.value("startSizeMin", c.StartSizeMin);
    c.StartSizeMax = pj.value("startSizeMax", c.StartSizeMax);
    c.EndSizeMin = pj.value("endSizeMin", c.EndSizeMin);
    c.EndSizeMax = pj.value("endSizeMax", c.EndSizeMax);
    if (pj.contains("startColor")) c.StartColor = Vec4FromJson(pj["startColor"]);
    if (pj.contains("endColor")) c.EndColor = Vec4FromJson(pj["endColor"], glm::vec4(1, 1, 1, 0));
    c.AngularVelocityMax = pj.value("angularVelocityMax", c.AngularVelocityMax);
    c.Shape = (pj.value("shape", std::string("circle")) == "quad")
                  ? ParticleShape::Quad : ParticleShape::SoftCircle;
    c.EmissionRate = pj.value("emissionRate", c.EmissionRate);
    return pe;
}

namespace SceneSerializer {

// Общая сборка JSON-дерева сцены — используется и файловым Save, и SaveToString.
// Секция запечённого GI: настройки + отпечаток + (для файла) объём проб.
// Страницы атласа в JSON не кладутся — файловый Save пишет их рядом .hdr-файлами
// (gi::SavePages), а строковые снапшоты (undo/Play) переносят бейк указателем
// (gi::Transplant) — тащить мегабайты текселей в каждый снапшот незачем.
static json GIToJson(const sage::gi::GIState& st, bool withProbes) {
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

static std::shared_ptr<sage::gi::GIState> GIFromJson(const json& g) {
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

static json BuildSceneJson(const Scene& scene, bool withProbes = true) {
    json root;
    root["sage_scene_version"] = kSceneVersion;
    root["name"] = scene.Name();

    json objectsJson = json::array();
    // Обходим сущности через ECS-view. Собираем в вектор и сортируем по id,
    // чтобы вывод был детерминированным (порядок обхода entt не гарантирован).
    entt::registry& reg = const_cast<Scene&>(scene).Registry();
    std::vector<entt::entity> entities;
    for (auto e : reg.view<IdComponent>()) entities.push_back(e);
    std::sort(entities.begin(), entities.end(), [&reg](entt::entity a, entt::entity b) {
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    for (entt::entity e : entities) {
        const Transform& tr = reg.get<Transform>(e);
        const MeshRendererComponent& mr = reg.get<MeshRendererComponent>(e);
        json j;
        j["id"] = reg.get<IdComponent>(e).Id;
        j["name"] = reg.get<NameComponent>(e).Name;
        // Родитель в иерархии — по стабильному id (восстанавливается после загрузки всех).
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
            if (h->Parent != entt::null && reg.valid(h->Parent))
                if (const IdComponent* pid = reg.try_get<IdComponent>(h->Parent))
                    j["parent"] = pid->Id;
        }
        j["position"] = Vec3ToJson(tr.Position);
        j["rotation"] = Vec3ToJson(tr.Rotation);
        j["scale"]    = Vec3ToJson(tr.Scale);
        j["color"]    = Vec3ToJson(mr.Color);
        j["opacity"]  = mr.Opacity;
        j["castShadows"] = mr.CastShadows;
        j["inReflections"] = mr.InReflections;
        j["emissive"] = Vec3ToJson(mr.Emissive);
        j["emissiveStrength"] = mr.EmissiveStrength;
        j["mesh"]["type"] = MeshTypeToString(mr.Ref.type);
        SaveAssetRef(j["mesh"], "path", mr.Ref.path);
        SaveAssetRef(j, "material", mr.MaterialPath);
        // Материалы подмешей — массивом, по слоту на элемент разметки меша.
        // Пишется, только если слоты есть: у одноматериального объекта пустой
        // массив в каждом узле сцены был бы шумом в файле, который люди читают
        // и сравнивают в системе контроля версий.
        //
        // Пустой слот сохраняется как пустой объект, а не пропускается: слоты
        // адресуются НОМЕРОМ подмеша, и сжать дырки значило бы сдвинуть все
        // последующие материалы на одну часть модели.
        if (!mr.Slots.empty()) {
            json slots = json::array();
            for (const MaterialSlot& slot : mr.Slots) {
                json sj = json::object();
                SaveAssetRef(sj, "path", slot.Path);
                slots.push_back(std::move(sj));
            }
            j["materialSlots"] = std::move(slots);
        }
        if (const ScriptComponent* sc = reg.try_get<ScriptComponent>(e)) {
            SaveAssetRef(j, "script", sc->Path);
        }
        if (const DecalComponent* dc = reg.try_get<DecalComponent>(e)) {
            // Сохраняются ПАРАМЕТРЫ проекции, а не её результат: геометрия
            // наклейки — производная от сцены, и хранить её значило бы держать
            // в файле копию соседних мешей, устаревающую от любой их правки.
            j["decal"]["angleLimit"] = dc->AngleLimitDeg;
            j["decal"]["offset"] = dc->Offset;
        }
        if (const GIStaticComponent* gs = reg.try_get<GIStaticComponent>(e)) SaveGIStatic(j, *gs);
        if (reg.all_of<NetReplicatedComponent>(e)) j["netReplicated"] = true;
        if (const CameraComponent* cam = reg.try_get<CameraComponent>(e)) SaveCamera(j, *cam);
        if (const LightComponent* light = reg.try_get<LightComponent>(e)) SaveLight(j, *light);
        if (const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e)) SaveRigidBody(j, *rb);
        if (const ColliderComponent* col = reg.try_get<ColliderComponent>(e)) SaveCollider(j, *col);
        if (const JointComponent* jc = reg.try_get<JointComponent>(e)) SaveJoint(j, *jc);
        if (const AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(e)) SaveAnimatedModel(j, *am);
        if (const IKComponent* ik = reg.try_get<IKComponent>(e)) SaveIK(j, *ik);
        if (const ReflectionProbeComponent* rp = reg.try_get<ReflectionProbeComponent>(e))
            SaveReflectionProbe(j, *rp);
        if (const CharacterControllerComponent* cc = reg.try_get<CharacterControllerComponent>(e))
            SaveCharacter(j, *cc);
        if (const sage::render::LodComponent* lod = reg.try_get<sage::render::LodComponent>(e))
            SaveLod(j, *lod);
        if (const ShaderParamsComponent* sp = reg.try_get<ShaderParamsComponent>(e))
            SaveShaderParams(j, *sp);
        if (const ParticleEmitterComponent* pe = reg.try_get<ParticleEmitterComponent>(e)) SaveParticles(j, *pe);
        if (const VarsComponent* vc = reg.try_get<VarsComponent>(e))
            if (!vc->Values.Empty()) j["vars"] = VarsToJson(vc->Values);
        if (const sage::ui::UIDocumentComponent* uc =
                reg.try_get<sage::ui::UIDocumentComponent>(e))
            SaveUIDocument(j, *uc);
        objectsJson.push_back(j);
    }
    root["objects"] = objectsJson;
    root["lighting"] = LightingToJson(scene.Lighting);
    root["reflections"] = ReflectionsToJson(scene.Reflections);
    if (scene.GI) root["gi"] = GIToJson(*scene.GI, withProbes);
    return root;
}

// Общее восстановление сцены из JSON-дерева — для файлового Load и LoadFromString.
// ---------------------------------------------------------------------------
//  Версия формата сцены и миграции
// ---------------------------------------------------------------------------
//
// Номер писался в файл с самого начала и НИКОГДА не читался. Это худший из
// вариантов: он создаёт впечатление, что о совместимости позаботились, а на
// деле первое же ломающее изменение формата тихо испортило бы все старые
// сцены — без ошибки, без предупреждения, просто «объекты почему-то не там».
//
// Как это работает теперь. Файл несёт номер версии; загрузчик прогоняет его
// через цепочку миграций до текущей. Каждая миграция — маленькая функция
// «из N в N+1», которая правит JSON, а не сцену: правка на уровне JSON не
// зависит от того, как сегодня выглядят компоненты, и потому не устаревает
// вместе с ними. Иначе миграцию пришлось бы переписывать каждый раз, когда
// меняется структура, ради которой она и была написана.
//
// Отсутствие номера означает версию 1 — сцены, сохранённые до появления
// проверки. Их не за что винить, и загружаться они обязаны.
namespace {

// v1 -> v2. Первая настоящая миграция: в v1 у тел не было ни слоя
// столкновений, ни признака сенсора, и «нет поля» надо превратить в «слой по
// умолчанию, не сенсор» ЯВНО. Само по себе это сделали бы и значения по
// умолчанию при разборе — но именно на таких «и так сработает» миграции и
// перестают писать, а потом однажды не срабатывает.
void MigrateV1toV2(json& root) {
    for (json& obj : root["objects"]) {
        if (!obj.contains("rigidBody")) continue;
        json& rb = obj["rigidBody"];
        if (!rb.contains("layer")) rb["layer"] = 1u;
        if (!rb.contains("sensor")) rb["sensor"] = false;
    }
}

// v2 -> v3. Ссылки на ассеты обзаводятся GUID'ами. Раньше личностью файла был
// ПУТЬ, и переименование молча ломало сцену: она грузилась, объект оставался на
// месте, просто без модели, и в логе не было ни строчки.
//
// Миграция проставляет GUID по текущим путям — то есть фиксирует связь ровно в
// том виде, в каком она сейчас верна. Дальше файл можно переименовывать: GUID
// живёт в сайдкаре рядом с ним и переезжает вместе.
//
// Путь, у которого ассета в базе нет, остаётся БЕЗ GUID'а, а не получает
// выдуманный: несуществующая ссылка должна остаться видимо сломанной, а не
// притвориться целой.
void MigrateV2toV3(json& root) {
    sage::AssetDatabase& db = sage::AssetDatabase::Instance();
    auto stamp = [&](json& holder, const char* key) {
        if (!holder.contains(key) || !holder[key].is_string()) return;
        const std::string path = holder[key].get<std::string>();
        if (path.empty()) return;
        const sage::AssetGuid guid = db.GuidOf(path);
        if (guid.Valid()) holder[std::string(key) + "Guid"] = guid.ToString();
    };
    for (json& obj : root["objects"]) {
        if (obj.contains("mesh") && obj["mesh"].is_object()) stamp(obj["mesh"], "path");
        stamp(obj, "material");
        stamp(obj, "script");
        if (obj.contains("animatedModel") && obj["animatedModel"].is_object())
            stamp(obj["animatedModel"], "path");
    }
}

// v3 -> v4. Цвет экземпляра стал МНОЖИТЕЛЕМ albedo материала, а не заменой ему.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. У MeshRendererComponent три поправки поверх материала —
// цвет, свечение, прозрачность — и правила наложения у них были три разных:
// цвет материал ЗАМЕЩАЛ, прозрачность множил, свечение складывал. Замещение
// хуже прочих тем, что молчит: назначил материал — и поле Color в инспекторе
// перестало влиять на что-либо, никак об этом не сообщив. Теперь правило одно:
// поправка модулирует материал.
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ. У сущности с материалом старый color НЕ участвовал в
// картинке вообще, а по новому правилу он домножил бы albedo — и сцена, которую
// никто не трогал, перекрасилась бы при первом же открытии. Поэтому мёртвое
// значение заменяется на нейтральное: белый множитель даёт ровно тот вид,
// который был. Сущности БЕЗ материала не трогаем — у них color и был цветом.
void MigrateV3toV4(json& root) {
    for (json& obj : root["objects"]) {
        const bool hasMaterial = obj.contains("material") && obj["material"].is_string() &&
                                 !obj["material"].get<std::string>().empty();
        if (!hasMaterial) continue;
        obj["color"] = json::array({1.0f, 1.0f, 1.0f});
    }
}

// v4 -> v5. Солнце переехало из НАСТРОЕК СЦЕНЫ в обычную сущность.
//
// ПОЧЕМУ ПОМЕНЯЛОСЬ. Направленный свет был единственным источником, который не
// являлся объектом: его нельзя было выбрать в иерархии, повернуть гизмо,
// анимировать, привязать к родителю или положить в префаб, и второго такого
// света в сцене быть не могло. Всё это движок умеет делать с сущностями — и не
// умел с солнцем ровно потому, что солнце сущностью не было. Разбор — в
// комментарии к LightComponent (ecs/CameraLightComponents.h).
//
// ЧТО ДЕЛАЕТ МИГРАЦИЯ. Заводит объект «Солнце» с направленным светом, переносит
// в него цвет, яркость и направление (направление превращается в ПОВОРОТ: у
// сущности нет поля «куда светит», у неё есть ориентация), и обнуляет яркость
// старого поля.
//
// Обнуление обязательно, и это не уборка. Настройка сцены осталась основанием
// кадра — её перекрывает направленный свет-сущность, но если такую сущность
// удалить, основание проступит обратно. Сцена, где человек удалил солнце и
// продолжает видеть солнечный свет, объяснима только чтением исходников.
void MigrateV4toV5(json& root) {
    json sun = json::object();
    if (root.contains("lighting") && root["lighting"].contains("sun")) sun = root["lighting"]["sun"];

    const DirectionalLight defaults;
    glm::vec3 direction = defaults.Direction;
    glm::vec3 color = defaults.Color;
    float intensity = defaults.Intensity;
    if (sun.contains("direction")) direction = Vec3FromJson(sun["direction"]);
    if (sun.contains("color")) color = Vec3FromJson(sun["color"]);
    intensity = sun.value("intensity", intensity);

    // Свободный id: сущность добавляется к уже существующим, и совпадение
    // сломало бы связи иерархии (родитель ищется по id).
    int maxId = 0;
    for (const json& obj : root["objects"]) maxId = std::max(maxId, obj.value("id", 0));

    json entity;
    entity["id"] = maxId + 1;
    // Имя латиницей и БЕЗ перевода: это имя объекта в сцене, то есть ДАННЫЕ.
    // Сцена, открытая на другом языке, не должна оказаться со «Sun» вместо
    // «Солнце» — иначе скрипт, ищущий объект по имени, перестал бы его
    // находить от смены языка интерфейса. Остальные объекты демо-сцены
    // называются так же, по-английски.
    entity["name"] = "Sun";
    // Позиция ни на что не влияет (свет из бесконечности), но объект без
    // позиции неудобно найти в сцене — ставим его над началом мира.
    entity["position"] = Vec3ToJson(glm::vec3(0.0f, 10.0f, 0.0f));
    entity["rotation"] = Vec3ToJson(sage::ecs::EulerFromForward(direction));
    entity["scale"] = Vec3ToJson(glm::vec3(1.0f));
    entity["mesh"]["type"] = "none";
    entity["light"]["type"] = "directional";
    entity["light"]["color"] = Vec3ToJson(color);
    entity["light"]["intensity"] = intensity;
    entity["light"]["castShadows"] = true;
    root["objects"].push_back(entity);

    root["lighting"]["sun"]["intensity"] = 0.0f;
}

// v5 -> v6: НИЧЕГО НЕ ДЕЛАЕТ.
//
// Этот шаг разбирал внутреннее устройство старого элемента интерфейса: значок,
// сдвигавший подпись, галку, красившую соседа, ползунок, забиравший цвета у
// подложки. Интерфейса в сцене больше нет — разбирать нечего, и следующий шаг
// (v6 -> v7) блок "ui" всё равно убирает.
//
// Шаг оставлен пустым, а не выкинут: номера версий — это цепочка, и выдёргивать
// из неё звено значит сломать чтение всех сцен, записанных после него.
void MigrateV5toV6(json& root) { (void)root; }

// v6 -> v7: интерфейс уехал из сцены в документ.
//
// Старый блок "ui" описывал САМ интерфейс — сорок полей на объект. Теперь в
// сцене остаётся только ССЫЛКА на документ (.uidoc), а старый блок здесь
// убирается: молча тащить его дальше значило бы держать в файле данные, которых
// никто не читает, и однажды перепутать их со ссылкой.
//
// Сам интерфейс при этом НЕ ТЕРЯЕТСЯ: он переносится в документ отдельным
// инструментом, который читает исходный файл сцены
// (sage/ui/serialization/UIMigration.h). Поэтому здесь — предупреждение с
// именем объекта, а не тихое удаление.
void MigrateV6toV7(json& root) {
    int dropped = 0;
    for (json& obj : root["objects"]) {
        if (!obj.contains("ui") || !obj["ui"].is_object()) continue;
        const json& uj = obj["ui"];
        // Ссылку на документ не трогаем: это уже новый формат.
        if (uj.contains("document")) continue;
        obj.erase("ui");
        ++dropped;
    }
    if (dropped > 0) {
        LOG_WARN("Scene") << "интерфейс старого образца убран из сцены (" << dropped
                          << " объектов). Перенесите его в документ: "
                             "UIMigrateSceneFile(<исходный .sage>, doc)";
    }
}

using MigrationFn = void (*)(json&);

// Цепочка миграций: индекс i переводит версию (i+1) в (i+2).
const MigrationFn kMigrations[] = {
    &MigrateV1toV2,
    &MigrateV2toV3,
    &MigrateV3toV4,
    &MigrateV4toV5,
    &MigrateV5toV6,
    &MigrateV6toV7,
};

} // namespace

static int MigrateJsonInPlace(json& root) {
    const int from = root.value("sage_scene_version", 1);
    if (from > kSceneVersion) {
        throw std::runtime_error(
            "Сцена сохранена более новой версией движка (формат " + std::to_string(from) +
            ", движок понимает " + std::to_string(kSceneVersion) +
            "). Обновите движок — открыть её сейчас значит потерять часть данных.");
    }
    if (from < 1) throw std::runtime_error("Повреждённый номер версии сцены");
    if (!root.contains("objects") || !root["objects"].is_array()) root["objects"] = json::array();

    for (int v = from; v < kSceneVersion; ++v) {
        kMigrations[v - 1](root);
    }
    if (from < kSceneVersion) {
        LOG_INFO("Scene") << "Сцена обновлена с формата " << from << " до " << kSceneVersion;
    }
    root["sage_scene_version"] = kSceneVersion;
    return from;
}

static std::unique_ptr<Scene> BuildSceneFromJson(const json& root) {
    auto scene = std::make_unique<Scene>(root.value("name", "Untitled"));

    int maxId = 0;
    int fallbackId = 1;
    std::vector<std::pair<int, int>> parentLinks; // {childId, parentId} — применяем после загрузки всех
    for (const auto& j : root.value("objects", json::array())) {
        int id = j.value("id", fallbackId++);
        GameObject obj = scene->CreateObjectWithId(j.value("name", "Object"), id);
        // ФАКТИЧЕСКИЙ id: при дубликате в файле Scene выдаёт ближайший свободный
        // (см. CreateObjectWithId) — maxId и связи иерархии считаем по нему.
        id = obj.Id();
        maxId = std::max(maxId, id);
        if (j.contains("parent")) parentLinks.push_back({id, j.value("parent", -1)});

        Transform& tr = obj.GetTransform();
        MeshRendererComponent& mr = obj.Renderer();
        if (j.contains("position")) tr.Position = Vec3FromJson(j["position"]);
        if (j.contains("rotation")) tr.Rotation = Vec3FromJson(j["rotation"]);
        if (j.contains("scale"))    tr.Scale    = Vec3FromJson(j["scale"]);
        if (j.contains("color"))    mr.Color    = Vec3FromJson(j["color"]);
        mr.Opacity = j.value("opacity", mr.Opacity);
        mr.CastShadows = j.value("castShadows", mr.CastShadows);
        mr.InReflections = j.value("inReflections", mr.InReflections);
        if (j.contains("emissive")) mr.Emissive = Vec3FromJson(j["emissive"]);
        mr.EmissiveStrength = j.value("emissiveStrength", mr.EmissiveStrength);

        if (j.contains("mesh")) {
            mr.Ref.type = MeshTypeFromString(j["mesh"].value("type", "none"));
            mr.Ref.path = LoadAssetRef(j["mesh"], "path");
        }

        if (j.contains("decal")) {
            DecalComponent dc;
            dc.AngleLimitDeg = j["decal"].value("angleLimit", dc.AngleLimitDeg);
            dc.Offset = j["decal"].value("offset", dc.Offset);
            dc.Dirty = true; // геометрию строит система после загрузки сцены
            obj.Registry()->emplace<DecalComponent>(obj.Entity(), dc);
        }

        if (j.contains("script")) {
            obj.Registry()->emplace<ScriptComponent>(obj.Entity(),
                                                     ScriptComponent{LoadAssetRef(j, "script")});
        }

        // Материал: путь сериализуется, разделяемый экземпляр — из кэша.
        mr.MaterialPath = LoadAssetRef(j, "material");
        if (!mr.MaterialPath.empty()) {
            mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
        }
        if (j.contains("materialSlots") && j["materialSlots"].is_array()) {
            mr.Slots.clear();
            for (const json& sj : j["materialSlots"]) {
                MaterialSlot slot;
                slot.Path = sj.is_object() ? LoadAssetRef(sj, "path") : std::string();
                if (!slot.Path.empty())
                    slot.Ptr = ResourceManager::Instance().GetMaterial(slot.Path);
                mr.Slots.push_back(std::move(slot));
            }
        }

        if (j.contains("giStatic"))
            obj.Registry()->emplace<GIStaticComponent>(obj.Entity(), ParseGIStatic(j["giStatic"]));
        if (j.value("netReplicated", false))
            obj.Registry()->emplace<NetReplicatedComponent>(obj.Entity());
        if (j.contains("camera"))
            obj.Registry()->emplace<CameraComponent>(obj.Entity(), ParseCamera(j["camera"]));
        if (j.contains("light"))
            obj.Registry()->emplace<LightComponent>(obj.Entity(), ParseLight(j["light"]));
        if (j.contains("rigidBody"))
            obj.Registry()->emplace<RigidBodyComponent>(obj.Entity(), ParseRigidBody(j["rigidBody"]));
        if (j.contains("collider"))
            obj.Registry()->emplace<ColliderComponent>(obj.Entity(), ParseCollider(j["collider"]));
        if (j.contains("joint"))
            obj.Registry()->emplace<JointComponent>(obj.Entity(), ParseJoint(j["joint"]));
        if (j.contains("animatedModel"))
            obj.Registry()->emplace<AnimatedModelComponent>(obj.Entity(), ParseAnimatedModel(j["animatedModel"]));
        if (j.contains("ik"))
            obj.Registry()->emplace<IKComponent>(obj.Entity(), ParseIK(j["ik"]));
        if (j.contains("character"))
            obj.Registry()->emplace<CharacterControllerComponent>(obj.Entity(),
                                                                  ParseCharacter(j["character"]));
        if (j.contains("reflectionProbe"))
            obj.Registry()->emplace<ReflectionProbeComponent>(
                obj.Entity(), ParseReflectionProbe(j["reflectionProbe"]));
        if (j.contains("lod"))
            obj.Registry()->emplace<sage::render::LodComponent>(obj.Entity(), ParseLod(j["lod"]));
        if (j.contains("shaderParams"))
            obj.Registry()->emplace<ShaderParamsComponent>(obj.Entity(),
                                                           ParseShaderParams(j["shaderParams"]));
        if (j.contains("particles"))
            obj.Registry()->emplace<ParticleEmitterComponent>(obj.Entity(), ParseParticles(j["particles"]));
        if (j.contains("vars")) {
            VarsComponent vc;
            VarsFromJson(j["vars"], vc.Values);
            if (!vc.Values.Empty())
                obj.Registry()->emplace_or_replace<VarsComponent>(obj.Entity(), std::move(vc));
        }
        if (j.contains("ui") && j["ui"].contains("document")) {
            sage::ui::UIDocumentComponent uc;
            LoadUIDocument(j["ui"], uc);
            obj.Registry()->emplace_or_replace<sage::ui::UIDocumentComponent>(obj.Entity(),
                                                                             std::move(uc));
        }

        // Пересоздаём GPU-ресурс на основе описания
        if (mr.Ref.type == MeshRef::Type::Model) {
            mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
        } else {
            // Примитивы (Cube/Sphere/Plane/Cylinder/Cone) — из кэша; None -> nullptr.
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type);
        }
    }
    // Восстанавливаем иерархию, когда ВСЕ сущности уже созданы (родитель мог
    // идти в файле позже ребёнка).
    for (const auto& [childId, parentId] : parentLinks) scene->SetParentById(childId, parentId);

    scene->SetNextId(maxId + 1);
    scene->Lighting = LightingFromJson(root);
    scene->Reflections = ReflectionsFromJson(root);
    if (root.contains("gi")) scene->GI = GIFromJson(root["gi"]);

    return scene;
}

// --- Публичные обёртки: файл и строка используют одну и ту же сборку JSON ---

void Save(const Scene& scene, const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл для записи сцены: " + path);
    }
    file << BuildSceneJson(scene).dump(2);
    // Страницы атласа лайтмап — HDR-файлами рядом со сценой (в JSON им не место).
    if (scene.GI && scene.GI->Baked) sage::gi::SavePages(*scene.GI, path);
}

std::unique_ptr<Scene> Load(const std::string& path) {
    // Через vfs, а не напрямую: в собранной игре сцены лежат в пакете, в
    // редакторе — на диске, и загрузчик обязан быть ОДИН. Два пути загрузки
    // означали бы, что половина кода проверяется не в том виде, в каком её
    // увидит игрок.
    std::string text;
    if (!sage::assets::vfs::ReadText(path, text)) {
        throw std::runtime_error("Не удалось открыть файл сцены: " + path);
    }
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка парсинга JSON сцены (" + path + "): " + e.what());
    }
    // Обновление формата — ДО разбора: дальше код читает уже текущую версию и
    // ничего не знает про старые. Иначе каждая функция разбора обрастала бы
    // ветками «а если файл старый», и через три версии их стало бы не сосчитать.
    MigrateJsonInPlace(root);
    std::unique_ptr<Scene> scene = BuildSceneFromJson(root);
    // Восстановление лайтмап: пересчёт развёртки + чтение страниц с диска
    // (при несовпадении отпечатка геометрии бейк помечается устаревшим).
    sage::gi::RebuildAfterLoad(*scene, path);
    return scene;
}

std::string SaveToString(const Scene& scene) {
    // Без отступов (dump()) — снапшоты undo/Play держатся в памяти, компактность важнее читаемости.
    // Без объёма проб: снапшоты undo/Play переносят бейк через gi::Transplant.
    return BuildSceneJson(scene, /*withProbes=*/false).dump();
}

int CurrentVersion() { return kSceneVersion; }

std::string MigrateSceneJson(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Ошибка парсинга JSON сцены: ") + e.what());
    }
    MigrateJsonInPlace(root);
    return root.dump();
}

std::unique_ptr<Scene> LoadFromString(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Ошибка парсинга JSON сцены (строка): ") + e.what());
    }
    // Тем же путём, что и файл: снапшоты undo/Play — это тот же формат, и
    // пропустить их мимо миграции значит завести второй, необновляемый вход.
    MigrateJsonInPlace(root);
    return BuildSceneFromJson(root);
}

}
