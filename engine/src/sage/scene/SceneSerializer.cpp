#include "SceneSerializer.h"
#include "sage/audio/AudioComponents.h"
#include "sage/assets/Pack.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/LodGroup.h"
#include "sage/gi/GI.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ResourceManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/scene/SceneLegacyUI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UISerialize.h"
#include "sage/scene/SceneJson.h"
#include "sage/scene/SceneValueJson.h"
#include "sage/scene/SceneMigrations.h"

using json = nlohmann::json;

// Переменные и связи в JSON — общий словарь (см. SceneValueJson.h).
using sage::scene::BindingsFromJson;
using sage::scene::BindingsToJson;
using sage::scene::ValueFromJson;
using sage::scene::ValueToJson;
using sage::scene::VarsFromJson;
using sage::scene::VarsToJson;

// Векторы в JSON и обратно — общий словарь формата (см. SceneJson.h).
using sage::scene::Vec2FromJson;
using sage::scene::Vec2ToJson;
using sage::scene::Vec3FromJson;
using sage::scene::Vec3ToJson;
using sage::scene::Vec4FromJson;
using sage::scene::Vec4ToJson;

static std::string MeshTypeToString(MeshRef::Type t) {
    switch (t) {
        case MeshRef::Type::Cube:     return "cube";
        case MeshRef::Type::Sphere:   return "sphere";
        case MeshRef::Type::Plane:    return "plane";
        case MeshRef::Type::Cylinder: return "cylinder";
        case MeshRef::Type::Cone:     return "cone";
        case MeshRef::Type::Capsule:  return "capsule";
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
    if (s == "capsule")  return MeshRef::Type::Capsule;
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

// --- Тракт пост-обработки на камере -------------------------------------------
//
// Сам формат тракта живёт в движке (sage/render/PostChainIO.h) и ОДИН на всех,
// кто его хранит: сцену и конфиг проекта. Здесь только место компонента в файле
// сцены — так же, как у камеры, света и прочих компонентов.
static void SavePostProcess(json& j, const sage::render::PostProcessComponent& component) {
    j["postProcess"] = sage::render::PostChainToJson(component.Chain);
    j["postProcess"]["enabled"] = component.Enabled;
}

static sage::render::PostProcessComponent ParsePostProcess(const json& cj) {
    sage::render::PostProcessComponent component;
    component.Enabled = cj.value("enabled", true);
    component.Chain = sage::render::PostChainFromJson(cj);
    return component;
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

static void SaveAnimation(json& j, const AnimationComponent& am) {
    // Только описательные поля — модель, поза и палитра костей восстанавливаются
    // загрузкой. Пути к модели здесь БОЛЬШЕ НЕТ: она принадлежит Mesh, и второе
    // её имя в файле означало бы два источника правды, которые однажды
    // разъедутся (см. AnimationComponent).
    json& oj = j["animation"];
    // Клип — ПУТЬ к файлу; номер остался запасным путём для сцен, сделанных до
    // появления файлов клипов, и для моделей, клипы которых ещё не вынуты.
    oj["clipPath"] = am.ClipPath;
    oj["clip"] = am.Clip;
    oj["speed"] = am.Speed;
    oj["loop"] = am.Loop;
    oj["playing"] = am.Playing;
    oj["blendTime"] = am.BlendTime;
    oj["rootMotion"] = am.RootMotion;
}

static AnimationComponent ParseAnimation(const json& aj) {
    AnimationComponent am;
    am.ClipPath = aj.value("clipPath", std::string());
    am.Clip = aj.value("clip", 0);
    am.Speed = aj.value("speed", 1.0f);
    am.Loop = aj.value("loop", true);
    am.Playing = aj.value("playing", true);
    am.BlendTime = aj.value("blendTime", 0.25f);
    am.RootMotion = aj.value("rootMotion", false);
    // Model/Anim восстановятся при первом UpdateAnimators (Ready=false).
    return am;
}

// Проигрывание клипа по свойствам (sage/anim/PropertyAnimator.h). Клип —
// ОТДЕЛЬНЫЙ ФАЙЛ, и в сцену идёт только путь к нему с настройками воспроизведения:
// тот же «мигание» висит на десяти кнопках, и копия внутри каждой означала бы
// десять разъехавшихся копий.
static void SavePropertyAnimator(json& j, const PropertyAnimatorComponent& a) {
    json& oj = j["propertyAnimator"];
    oj["clipPath"] = a.ClipPath;
    oj["playing"] = a.Playing;
    oj["loop"] = a.Loop;
    oj["speed"] = a.Speed;
    // Время НЕ пишется: это положение бегунка сейчас, а не свойство сцены.
    // Сохранённое «сейчас середина» означало бы сцену, которая открывается с
    // движением, начатым неизвестно кем.
}

static PropertyAnimatorComponent ParsePropertyAnimator(const json& aj) {
    PropertyAnimatorComponent a;
    a.ClipPath = aj.value("clipPath", std::string());
    a.Playing = aj.value("playing", true);
    a.Loop = aj.value("loop", true);
    a.Speed = aj.value("speed", 1.0f);
    return a;   // Clip/Ready восстановит первый UpdatePropertyAnimators
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

static sage::scene::LegacyElement::Kind UIKindFromString(const std::string& s) {
    if (s == "label") return sage::scene::LegacyElement::Kind::Label;
    if (s == "image") return sage::scene::LegacyElement::Kind::Image;
    if (s == "bar")   return sage::scene::LegacyElement::Kind::Bar;
    if (s == "icon")  return sage::scene::LegacyElement::Kind::Icon;
    if (s == "input") return sage::scene::LegacyElement::Kind::Input;
    if (s == "checkbox") return sage::scene::LegacyElement::Kind::Checkbox;
    if (s == "slider") return sage::scene::LegacyElement::Kind::Slider;
    return sage::scene::LegacyElement::Kind::Panel;
}

static sage::scene::LegacyElement ParseUIElement(const json& uj) {
    sage::scene::LegacyElement u;
    u.Type = UIKindFromString(uj.value("kind", "panel"));
    int anchor = uj.value("anchor", 0);
    if (anchor >= 0 && anchor <= 8) u.Anchor = (UIAnchor)anchor;
    if (uj.contains("offset")) {
        u.Offset.x = uj["offset"].value("x", u.Offset.x);
        u.Offset.y = uj["offset"].value("y", u.Offset.y);
    }
    if (uj.contains("size")) {
        u.Size.x = uj["size"].value("x", u.Size.x);
        u.Size.y = uj["size"].value("y", u.Size.y);
    }
    u.Layer = uj.value("layer", u.Layer);
    u.Visible = uj.value("visible", u.Visible);
    u.ClipChildren = uj.value("clipChildren", u.ClipChildren);
    if (uj.contains("color")) u.Color = Vec4FromJson(uj["color"], u.Color);
    u.Rounding = uj.value("rounding", u.Rounding);
    u.BorderThickness = uj.value("borderThickness", u.BorderThickness);
    if (uj.contains("borderColor")) u.BorderColor = Vec4FromJson(uj["borderColor"], u.BorderColor);
    u.Text = uj.value("text", u.Text);
    u.TextScale = uj.value("textScale", u.TextScale);
    if (uj.contains("textColor")) u.TextColor = Vec4FromJson(uj["textColor"], u.TextColor);
    u.TextCentered = uj.value("textCentered", u.TextCentered);
    u.TexturePath = uj.value("texture", u.TexturePath);
    u.Value = uj.value("value", u.Value);
    if (uj.contains("barFillColor")) u.BarFillColor = Vec4FromJson(uj["barFillColor"], u.BarFillColor);
    u.Icon = uj.value("icon", u.Icon);
    if (uj.contains("iconColor")) u.IconColor = Vec4FromJson(uj["iconColor"], u.IconColor);
    if (uj.contains("gradientColor")) u.GradientColor = Vec4FromJson(uj["gradientColor"], u.GradientColor);
    u.ShadowSize = uj.value("shadowSize", u.ShadowSize);
    if (uj.contains("sprite")) u.Sprite = Vec4FromJson(uj["sprite"], u.Sprite);
    if (uj.contains("sliceBorder")) u.SliceBorder = Vec4FromJson(uj["sliceBorder"], u.SliceBorder);
    u.PixelScale = uj.value("pixelScale", u.PixelScale);
    u.PixelArt = uj.value("pixelArt", u.PixelArt);
    if (uj.contains("spriteHover")) u.SpriteHover = Vec4FromJson(uj["spriteHover"], u.SpriteHover);
    if (uj.contains("spritePressed"))
        u.SpritePressed = Vec4FromJson(uj["spritePressed"], u.SpritePressed);
    u.Interactive = uj.value("interactive", u.Interactive);
    u.Enabled = uj.value("enabled", u.Enabled);
    u.Placeholder = uj.value("placeholder", u.Placeholder);
    u.MaxLength = uj.value("maxLength", u.MaxLength);
    u.Password = uj.value("password", u.Password);
    u.MinValue = uj.value("minValue", u.MinValue);
    u.MaxValue = uj.value("maxValue", u.MaxValue);
    u.WrapText = uj.value("wrapText", u.WrapText);
    u.PadX = uj.value("padX", u.PadX);
    u.AutoWidth = uj.value("autoWidth", u.AutoWidth);
    // Текстура картинки — рантайм, из кэша (nullptr при ошибке — заглушка цветом).
    if (!u.TexturePath.empty()) {
        // Пиксель-арт грузится ближайшим соседом и без мипмапов — иначе набор
        // спрайтов размывается, а мипмапы ЛИСТА подмешивают в края соседний
        // спрайт.
        u.Tex = u.PixelArt
                    ? ResourceManager::Instance().GetTexture(u.TexturePath, TextureFilter::Nearest,
                                                             /*mipmaps=*/false)
                    : ResourceManager::Instance().GetTexture(u.TexturePath);
    }
    return u;
}

// --- Интерфейс: КОМПОНЕНТЫ ---------------------------------------------------
//
// Элемент интерфейса — это набор компонентов (см. sage/ui/UI.h), и в файле он
// выглядит так же: под "ui" лежат только те части, которые у элемента ЕСТЬ.
// Кнопка — это transform + fill + label + interactable, надпись — transform +
// label. Раньше писался один плоский блок на сорок полей, и у каждой надписи в
// файле честно хранились скругление, девятина, предел длины поля ввода и
// границы ползунка — поля, ничего для неё не значащие.
//
// СТАРЫЙ ФОРМАТ ЧИТАЕТСЯ. Признак — ключ "kind": он был видом элемента и в
// новой записи не встречается. Такой блок разбирается прежним разбором и
// раскладывается по компонентам (sage::scene::Decompose), поэтому сцены, сделанные до
// перехода, открываются без единой правки руками.

// --- Части элемента — ПО РЕЕСТРУ (sage/ui/UIPart.h) -------------------------
//
// Здесь было двести строк «поле за полем» на запись и столько же на чтение, и
// два этих списка обязаны были совпадать. Рано или поздно они расходятся:
// свойство сохраняется, но не читается, и молча сбрасывается при следующей
// загрузке — самая незаметная из поломок формата.
//
// Теперь оба идут по одной таблице полей, объявленной рядом с самой частью.
// Своя часть игры сериализуется без единой правки здесь: она есть в реестре —
// значит, она есть в файле.


// --- Значения, переменные и связи (sage/vars, sage/events) -------------------
//
// Один разбор на все три задачи: публичная переменная объекта, аргумент
// события и связь кнопки — это одно и то же значение под именем. Раньше каждая
// такая вещь заводила бы свою пару «запись/чтение», и рано или поздно они
// расходятся: поле сохраняется, но не читается, и молча сбрасывается.
static void SaveAudio(json& j, const AudioSourceComponent& a) {
    json& aj = j["audio"];
    aj["clip"] = a.Clip;
    aj["volume"] = a.Volume;
    aj["pitch"] = a.Pitch;
    aj["loop"] = a.Loop;
    aj["autoPlay"] = a.AutoPlay;
    aj["spatial"] = a.Spatial;
    aj["minDistance"] = a.MinDistance;
    aj["maxDistance"] = a.MaxDistance;
    aj["rolloff"] = a.Rolloff;
    aj["category"] = (int)a.Category;
}

static AudioSourceComponent ParseAudio(const json& aj) {
    AudioSourceComponent a;
    a.Clip = aj.value("clip", std::string());
    a.Volume = aj.value("volume", 1.0f);
    a.Pitch = aj.value("pitch", 1.0f);
    a.Loop = aj.value("loop", false);
    a.AutoPlay = aj.value("autoPlay", true);
    a.Spatial = aj.value("spatial", true);
    a.MinDistance = aj.value("minDistance", 1.0f);
    a.MaxDistance = aj.value("maxDistance", 40.0f);
    a.Rolloff = aj.value("rolloff", 1.0f);
    const int cat = aj.value("category", 0);
    a.Category = (cat >= 0 && cat <= 2) ? (AudioCategory)cat : AudioCategory::Sfx;
    return a;
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
    root["sage_scene_version"] = sage::scene::kSceneVersion;
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
        // Указатель, а не ссылка: MeshRenderer есть НЕ У ВСЕХ. Пустой объект
        // (узел иерархии, точка привязки, держатель скрипта) его не несёт, и
        // reg.get<> на такой сущности читал бы чужую память.
        const MeshRendererComponent* mrp = reg.try_get<MeshRendererComponent>(e);
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
        // ПРИЗНАК ОТСУТСТВИЯ, А НЕ МОЛЧАНИЕ. Старые сцены ключа "noMesh" не
        // имеют, и загрузчик по-прежнему даёт их объектам MeshRenderer — иначе
        // каждая сохранённая до этой правки сцена открылась бы невидимой.
        // Отсутствие компонента поэтому пишется явно.
        //
        // Пропускаются ТОЛЬКО поля меша: у пустого объекта могут быть скрипт,
        // свет, физика, звук и дети, и они сохраняются наравне со всеми.
        if (!mrp) j["noMesh"] = true;
        if (mrp) {
            const MeshRendererComponent& mr = *mrp;
            j["color"]    = Vec3ToJson(mr.Color);
            j["castShadows"] = mr.CastShadows;
            j["inReflections"] = mr.InReflections;
            // СВЕЧЕНИЯ И ПРОЗРАЧНОСТИ У ЭКЗЕМПЛЯРА БОЛЬШЕ НЕТ: и то и другое —
            // свойство материала (см. MeshRendererComponent). Ключи opacity,
            // emissive и emissiveStrength не пишутся и не читаются: сцена,
            // сохранённая старым редактором, открывается, но эти значения
            // теряет — хранить их было бы обещанием, что они на что-то влияют.
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
        } // if (mrp)
        // Папка списка: сама метка и её цвет. Без этого «Декорации» после
        // перезагрузки сцены становились обычным пустым объектом — и снова
        // начинали таскать за собой содержимое.
        // Выключенное остаётся выключенным и после перезагрузки, и в собранной
        // игре: иначе «выключил и забыл» означало бы сюрприз ровно в тот
        // момент, когда игру собрали и раздали.
        if (reg.all_of<HiddenComponent>(e)) j["hidden"] = true;
        if (const FolderComponent* fc = reg.try_get<FolderComponent>(e)) {
            j["folder"]["color"] = Vec3ToJson(fc->Color);
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
        if (const sage::render::PostProcessComponent* pc =
                reg.try_get<sage::render::PostProcessComponent>(e))
            SavePostProcess(j, *pc);
        if (const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e)) SaveRigidBody(j, *rb);
        if (const ColliderComponent* col = reg.try_get<ColliderComponent>(e)) SaveCollider(j, *col);
        if (const JointComponent* jc = reg.try_get<JointComponent>(e)) SaveJoint(j, *jc);
        if (const AnimationComponent* am = reg.try_get<AnimationComponent>(e)) SaveAnimation(j, *am);
        if (const PropertyAnimatorComponent* pa = reg.try_get<PropertyAnimatorComponent>(e))
            SavePropertyAnimator(j, *pa);
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
        if (const AudioSourceComponent* au = reg.try_get<AudioSourceComponent>(e)) SaveAudio(j, *au);
        if (const VarsComponent* vc = reg.try_get<VarsComponent>(e))
            if (!vc->Values.Empty()) j["vars"] = VarsToJson(vc->Values);
        // Элемент и его компоненты — общей записью (см. sage/ui/UISerialize.h):
        // тот же формат, каким интерфейс ложится в отдельный ресурс .sageui.
        if (json ui; sage::ui::SaveElement(ui, reg, e)) j["ui"] = std::move(ui);
        // ИНТЕРФЕЙС — не часть элемента, а его ГРАНИЦА (см.
        // sage/ui/components/InterfaceComponent.h): отдельным ключом, рядом с
        // камерой и светом, а не внутри "ui".
        if (const sage::ui::InterfaceComponent* ic =
                reg.try_get<sage::ui::InterfaceComponent>(e)) {
            json ij;
            ij["visible"] = ic->Visible;
            ij["sortOrder"] = ic->SortOrder;
            ij["receivesInput"] = ic->ReceivesInput;
            ij["canvasMode"] = (int)ic->Canvas.Mode;
            ij["reference"] = Vec2ToJson(ic->Canvas.Reference);
            ij["matchWidthOrHeight"] = ic->Canvas.MatchWidthOrHeight;
            j["interface"] = std::move(ij);
        }
        objectsJson.push_back(j);
    }
    root["objects"] = objectsJson;
    root["lighting"] = LightingToJson(scene.Lighting);
    root["reflections"] = ReflectionsToJson(scene.Reflections);
    if (scene.GI) root["gi"] = GIToJson(*scene.GI, withProbes);
    return root;
}

// Общее восстановление сцены из JSON-дерева — для файлового Load и LoadFromString.
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
        mr.CastShadows = j.value("castShadows", mr.CastShadows);
        mr.InReflections = j.value("inReflections", mr.InReflections);
        // opacity / emissive / emissiveStrength из старых сцен НЕ читаются:
        // полей под них больше нет. Прозрачность и свечение задаёт материал, и
        // объект, которому они были нужны, получает свой материал — иначе вид
        // снова задавался бы в двух местах.

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

        if (j.value("hidden", false)) obj.Registry()->emplace<HiddenComponent>(obj.Entity());
        if (j.contains("folder")) {
            FolderComponent fc;
            if (j["folder"].contains("color")) fc.Color = Vec3FromJson(j["folder"]["color"]);
            obj.Registry()->emplace<FolderComponent>(obj.Entity(), fc);
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
        // Ключ "postChain" — имя этого же компонента ДО того, как он стал
        // «Пост-обработкой». Читается по-прежнему: сцена, сохранённая раньше,
        // обязана открыться (миграция v11->v12 переводит остальное).
        if (j.contains("postProcess"))
            obj.Registry()->emplace<sage::render::PostProcessComponent>(
                obj.Entity(), ParsePostProcess(j["postProcess"]));
        else if (j.contains("postChain"))
            obj.Registry()->emplace<sage::render::PostProcessComponent>(
                obj.Entity(), ParsePostProcess(j["postChain"]));
        if (j.contains("rigidBody"))
            obj.Registry()->emplace<RigidBodyComponent>(obj.Entity(), ParseRigidBody(j["rigidBody"]));
        if (j.contains("collider"))
            obj.Registry()->emplace<ColliderComponent>(obj.Entity(), ParseCollider(j["collider"]));
        if (j.contains("joint"))
            obj.Registry()->emplace<JointComponent>(obj.Entity(), ParseJoint(j["joint"]));
        if (j.contains("animation"))
            obj.Registry()->emplace<AnimationComponent>(obj.Entity(), ParseAnimation(j["animation"]));
        if (j.contains("propertyAnimator"))
            obj.Registry()->emplace<PropertyAnimatorComponent>(
                obj.Entity(), ParsePropertyAnimator(j["propertyAnimator"]));
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
        if (j.contains("audio"))
            obj.Registry()->emplace<AudioSourceComponent>(obj.Entity(), ParseAudio(j["audio"]));
        if (j.contains("vars")) {
            VarsComponent vc;
            VarsFromJson(j["vars"], vc.Values);
            if (!vc.Values.Empty())
                obj.Registry()->emplace_or_replace<VarsComponent>(obj.Entity(), std::move(vc));
        }
        if (j.contains("interface")) {
            const json& ij = j["interface"];
            sage::ui::InterfaceComponent ic;
            ic.Visible = ij.value("visible", true);
            ic.SortOrder = ij.value("sortOrder", 0);
            ic.ReceivesInput = ij.value("receivesInput", true);
            ic.Canvas.Mode = (sage::ui::Canvas::Scale)ij.value("canvasMode", 0);
            if (ij.contains("reference"))
                ic.Canvas.Reference = Vec2FromJson(ij["reference"], ic.Canvas.Reference);
            ic.Canvas.MatchWidthOrHeight = ij.value("matchWidthOrHeight", 0.5f);
            obj.Registry()->emplace<sage::ui::InterfaceComponent>(obj.Entity(), ic);
        }
        if (j.contains("ui")) {
            const json& uj = j["ui"];
            // Старая запись (плоский элемент с "kind") — история формата СЦЕН,
            // и разбирается она здесь, а не в общем читателе элемента: это два
            // разных формата, и делать вид, что один плавно переходит в другой,
            // значит получить третий.
            if (uj.contains("kind")) {
                sage::scene::Decompose(ParseUIElement(uj), *obj.Registry(), obj.Entity());
                if (sage::ui::Image* im =
                        obj.Registry()->try_get<sage::ui::Image>(obj.Entity()))
                    sage::ui::ResolveImageTexture(*im);
            } else {
                sage::ui::LoadElement(uj, *obj.Registry(), obj.Entity());
            }
        }

        // Пересоздаём GPU-ресурс на основе описания
        if (mr.Ref.type == MeshRef::Type::Model) {
            // У АНИМИРОВАННОГО объекта статический меш не грузим вовсе.
            //
            // Один и тот же файл читают два загрузчика: статический (в MeshPtr)
            // и скелетный (для Animation). Рисует объект только второй — значит,
            // первый копировал бы в видеопамять вершины и индексы, которыми
            // никто ни разу не воспользуется. На сцене с десятком разных
            // персонажей это десяток лишних копий геометрии, и заметить их
            // нечем: картинка правильная, просто памяти вдвое больше.
            const bool animated = obj.Registry()->all_of<AnimationComponent>(obj.Entity());
            mr.MeshPtr = animated ? nullptr : ResourceManager::Instance().GetModel(mr.Ref.path);
        } else {
            // Примитивы (Cube/Sphere/Plane/Cylinder/Cone) — из кэша; None -> nullptr.
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type);
        }

        // Пустой объект: компонент снимается ПОСЛЕДНИМ, уже после разбора всего
        // остального. Сущность создаётся общим CreateObjectWithId (он даёт
        // MeshRenderer всем), а строк выше, читающих mr, полтора десятка —
        // проще снять лишнее в конце, чем проводить указатель через весь разбор.
        if (j.value("noMesh", false)) obj.Registry()->remove<MeshRendererComponent>(obj.Entity());
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
    sage::scene::MigrateJsonInPlace(root);
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

int CurrentVersion() { return sage::scene::kSceneVersion; }

std::string MigrateSceneJson(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Ошибка парсинга JSON сцены: ") + e.what());
    }
    sage::scene::MigrateJsonInPlace(root);
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
    sage::scene::MigrateJsonInPlace(root);
    return BuildSceneFromJson(root);
}

}
