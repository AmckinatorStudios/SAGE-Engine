#include "SceneSerializer.h"
#include "sage/render/ResourceManager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <vector>

using json = nlohmann::json;

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

static void SaveCamera(json& j, const CameraComponent& cam) {
    j["camera"]["fov"] = cam.Fov;
    j["camera"]["near"] = cam.NearClip;
    j["camera"]["far"] = cam.FarClip;
    j["camera"]["primary"] = cam.Primary;
}

static CameraComponent ParseCamera(const json& cj) {
    CameraComponent cam;
    cam.Fov = cj.value("fov", cam.Fov);
    cam.NearClip = cj.value("near", cam.NearClip);
    cam.FarClip = cj.value("far", cam.FarClip);
    cam.Primary = cj.value("primary", cam.Primary);
    return cam;
}

static void SaveLight(json& j, const LightComponent& light) {
    j["light"]["type"] = (light.Kind == LightComponent::Type::Spot) ? "spot" : "point";
    j["light"]["color"] = Vec3ToJson(light.Color);
    j["light"]["intensity"] = light.Intensity;
    j["light"]["range"] = light.Range;
    j["light"]["innerCone"] = light.InnerConeDeg;
    j["light"]["outerCone"] = light.OuterConeDeg;
}

static LightComponent ParseLight(const json& lj) {
    LightComponent light;
    if (lj.value("type", std::string("point")) == "spot")
        light.Kind = LightComponent::Type::Spot;
    if (lj.contains("color")) light.Color = Vec3FromJson(lj["color"]);
    light.Intensity = lj.value("intensity", light.Intensity);
    light.Range = lj.value("range", light.Range);
    light.InnerConeDeg = lj.value("innerCone", light.InnerConeDeg);
    light.OuterConeDeg = lj.value("outerCone", light.OuterConeDeg);
    return light;
}

static void SaveRigidBody(json& j, const RigidBodyComponent& rb) {
    const char* types[] = {"static", "dynamic", "kinematic"};
    j["rigidBody"]["type"] = types[(int)rb.Type];
    j["rigidBody"]["mass"] = rb.Mass;
    j["rigidBody"]["friction"] = rb.Friction;
    j["rigidBody"]["restitution"] = rb.Restitution;
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

static void SaveAnimatedModel(json& j, const AnimatedModelComponent& am) {
    // Только описательные поля — модель/палитра восстанавливаются загрузкой.
    j["animatedModel"]["path"] = am.Path;
    j["animatedModel"]["demoSegments"] = am.DemoSegments;
    j["animatedModel"]["clip"] = am.Clip;
    j["animatedModel"]["speed"] = am.Speed;
    j["animatedModel"]["loop"] = am.Loop;
    j["animatedModel"]["playing"] = am.Playing;
    j["animatedModel"]["blendTime"] = am.BlendTime;
}

static AnimatedModelComponent ParseAnimatedModel(const json& aj) {
    AnimatedModelComponent am;
    am.Path = aj.value("path", std::string());
    am.DemoSegments = aj.value("demoSegments", 6);
    am.Clip = aj.value("clip", 0);
    am.Speed = aj.value("speed", 1.0f);
    am.Loop = aj.value("loop", true);
    am.Playing = aj.value("playing", true);
    am.BlendTime = aj.value("blendTime", 0.25f);
    // Model/Anim восстановятся при первом UpdateAnimators (Ready=false).
    return am;
}

static std::string UIKindToString(UIElementComponent::Kind k) {
    switch (k) {
        case UIElementComponent::Kind::Label: return "label";
        case UIElementComponent::Kind::Image: return "image";
        case UIElementComponent::Kind::Bar:   return "bar";
        default: return "panel";
    }
}

static UIElementComponent::Kind UIKindFromString(const std::string& s) {
    if (s == "label") return UIElementComponent::Kind::Label;
    if (s == "image") return UIElementComponent::Kind::Image;
    if (s == "bar")   return UIElementComponent::Kind::Bar;
    return UIElementComponent::Kind::Panel;
}

static void SaveUIElement(json& j, const UIElementComponent& u) {
    json& uj = j["ui"];
    uj["kind"] = UIKindToString(u.Type);
    uj["anchor"] = (int)u.Anchor; // 0..8 (см. UIAnchor)
    uj["offset"] = json{{"x", u.Offset.x}, {"y", u.Offset.y}};
    uj["size"] = json{{"x", u.Size.x}, {"y", u.Size.y}};
    uj["layer"] = u.Layer;
    uj["visible"] = u.Visible;
    uj["clipChildren"] = u.ClipChildren;
    uj["color"] = Vec4ToJson(u.Color);
    uj["rounding"] = u.Rounding;
    uj["borderThickness"] = u.BorderThickness;
    uj["borderColor"] = Vec4ToJson(u.BorderColor);
    uj["text"] = u.Text;
    uj["textScale"] = u.TextScale;
    uj["textColor"] = Vec4ToJson(u.TextColor);
    uj["textCentered"] = u.TextCentered;
    uj["texture"] = u.TexturePath;
    uj["value"] = u.Value;
    uj["barFillColor"] = Vec4ToJson(u.BarFillColor);
}

static UIElementComponent ParseUIElement(const json& uj) {
    UIElementComponent u;
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
    // Текстура картинки — рантайм, из кэша (nullptr при ошибке — заглушка цветом).
    if (!u.TexturePath.empty())
        u.Tex = ResourceManager::Instance().GetTexture(u.TexturePath);
    return u;
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
static json BuildSceneJson(const Scene& scene) {
    json root;
    root["sage_scene_version"] = 1;
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
        j["mesh"]["type"] = MeshTypeToString(mr.Ref.type);
        j["mesh"]["path"] = mr.Ref.path;
        if (!mr.MaterialPath.empty()) j["material"] = mr.MaterialPath;
        if (const ScriptComponent* sc = reg.try_get<ScriptComponent>(e)) {
            j["script"] = sc->Path;
        }
        if (const CameraComponent* cam = reg.try_get<CameraComponent>(e)) SaveCamera(j, *cam);
        if (const LightComponent* light = reg.try_get<LightComponent>(e)) SaveLight(j, *light);
        if (const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e)) SaveRigidBody(j, *rb);
        if (const ColliderComponent* col = reg.try_get<ColliderComponent>(e)) SaveCollider(j, *col);
        if (const JointComponent* jc = reg.try_get<JointComponent>(e)) SaveJoint(j, *jc);
        if (const AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(e)) SaveAnimatedModel(j, *am);
        if (const ParticleEmitterComponent* pe = reg.try_get<ParticleEmitterComponent>(e)) SaveParticles(j, *pe);
        if (const UIElementComponent* uie = reg.try_get<UIElementComponent>(e)) SaveUIElement(j, *uie);
        objectsJson.push_back(j);
    }
    root["objects"] = objectsJson;
    root["lighting"] = LightingToJson(scene.Lighting);
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

        if (j.contains("mesh")) {
            mr.Ref.type = MeshTypeFromString(j["mesh"].value("type", "none"));
            mr.Ref.path = j["mesh"].value("path", "");
        }

        if (j.contains("script")) {
            obj.Registry()->emplace<ScriptComponent>(obj.Entity(), ScriptComponent{j.value("script", "")});
        }

        // Материал: путь сериализуется, разделяемый экземпляр — из кэша.
        mr.MaterialPath = j.value("material", "");
        if (!mr.MaterialPath.empty()) {
            mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
        }

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
        if (j.contains("particles"))
            obj.Registry()->emplace<ParticleEmitterComponent>(obj.Entity(), ParseParticles(j["particles"]));
        if (j.contains("ui"))
            obj.Registry()->emplace<UIElementComponent>(obj.Entity(), ParseUIElement(j["ui"]));

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

    return scene;
}

// --- Публичные обёртки: файл и строка используют одну и ту же сборку JSON ---

void Save(const Scene& scene, const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл для записи сцены: " + path);
    }
    file << BuildSceneJson(scene).dump(2);
}

std::unique_ptr<Scene> Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл сцены: " + path);
    }
    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка парсинга JSON сцены (" + path + "): " + e.what());
    }
    return BuildSceneFromJson(root);
}

std::string SaveToString(const Scene& scene) {
    // Без отступов (dump()) — снапшоты undo/Play держатся в памяти, компактность важнее читаемости.
    return BuildSceneJson(scene).dump();
}

std::unique_ptr<Scene> LoadFromString(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Ошибка парсинга JSON сцены (строка): ") + e.what());
    }
    return BuildSceneFromJson(root);
}

}
