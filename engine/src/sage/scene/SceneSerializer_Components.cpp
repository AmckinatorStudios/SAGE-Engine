#include "sage/scene/SceneSerializerDetail.h"

// Компоненты сцены в JSON и обратно (см. SceneSerializerDetail.h).
namespace scene_serial {

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
void SaveAssetRef(json& j, const char* key, const std::string& path) {
    if (path.empty()) return;
    j[key] = path;
    const sage::AssetGuid guid = sage::AssetDatabase::Instance().GuidOf(path);
    if (guid.Valid()) j[std::string(key) + "Guid"] = guid.ToString();
}

// Возвращает АКТУАЛЬНЫЙ путь: по GUID, если он знаком (файл могли
// переименовать), иначе по сохранённому пути.
std::string LoadAssetRef(const json& j, const char* key) {
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

void SaveGIStatic(json& j, const GIStaticComponent& gs) {
    j["giStatic"]["lightmapped"] = gs.Lightmapped;
    j["giStatic"]["texelScale"] = gs.TexelScale;
}

GIStaticComponent ParseGIStatic(const json& gj) {
    GIStaticComponent gs;
    gs.Lightmapped = gj.value("lightmapped", gs.Lightmapped);
    gs.TexelScale = gj.value("texelScale", gs.TexelScale);
    return gs;
}

void SaveCamera(json& j, const CameraComponent& cam) {
    j["camera"]["projection"] =
        cam.Mode == CameraComponent::Projection::Orthographic ? "orthographic" : "perspective";
    j["camera"]["fov"] = cam.Fov;
    j["camera"]["orthoHeight"] = cam.OrthoHeight;
    j["camera"]["near"] = cam.NearClip;
    j["camera"]["far"] = cam.FarClip;
    j["camera"]["primary"] = cam.Primary;
}

CameraComponent ParseCamera(const json& cj) {
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

const char* LightTypeToString(LightComponent::Type kind) {
    switch (kind) {
        case LightComponent::Type::Spot: return "spot";
        case LightComponent::Type::Directional: return "directional";
        case LightComponent::Type::Area: return "area";
        default: return "point";
    }
}

LightComponent::Type LightTypeFromString(const std::string& text) {
    if (text == "spot") return LightComponent::Type::Spot;
    if (text == "directional") return LightComponent::Type::Directional;
    if (text == "area") return LightComponent::Type::Area;
    return LightComponent::Type::Point;
}

// --- Тракт пост-обработки на камере -------------------------------------------
//
// Сам формат тракта живёт в движке (sage/render/PostChainIO.h) и ОДИН на всех,
// кто его хранит: сцену и конфиг проекта. Здесь только место компонента в файле
// сцены — так же, как у камеры, света и прочих компонентов.
void SavePostProcess(json& j, const sage::render::PostProcessComponent& component) {
    j["postProcess"] = sage::render::PostChainToJson(component.Chain);
    j["postProcess"]["enabled"] = component.Enabled;
    j["postProcess"]["priority"] = component.Priority;
}

sage::render::PostProcessComponent ParsePostProcess(const json& cj) {
    sage::render::PostProcessComponent component;
    component.Enabled = cj.value("enabled", true);
    component.Priority = cj.value("priority", 0);
    component.Chain = sage::render::PostChainFromJson(cj);
    return component;
}

void SaveLight(json& j, const LightComponent& light) {
    j["light"]["type"] = LightTypeToString(light.Kind);
    j["light"]["color"] = Vec3ToJson(light.Color);
    j["light"]["intensity"] = light.Intensity;
    j["light"]["range"] = light.Range;
    j["light"]["innerCone"] = light.InnerConeDeg;
    j["light"]["outerCone"] = light.OuterConeDeg;
    j["light"]["castShadows"] = light.CastShadows;
    if (light.Kind == LightComponent::Type::Area) {
        j["light"]["width"] = light.AreaWidth;
        j["light"]["height"] = light.AreaHeight;
    }
}

LightComponent ParseLight(const json& lj) {
    LightComponent light;
    light.Kind = LightTypeFromString(lj.value("type", std::string("point")));
    if (lj.contains("color")) light.Color = Vec3FromJson(lj["color"]);
    light.Intensity = lj.value("intensity", light.Intensity);
    light.Range = lj.value("range", light.Range);
    light.InnerConeDeg = lj.value("innerCone", light.InnerConeDeg);
    light.OuterConeDeg = lj.value("outerCone", light.OuterConeDeg);
    light.CastShadows = lj.value("castShadows", light.CastShadows);
    light.AreaWidth = lj.value("width", light.AreaWidth);
    light.AreaHeight = lj.value("height", light.AreaHeight);
    return light;
}

void SaveRigidBody(json& j, const RigidBodyComponent& rb) {
    const char* types[] = {"static", "dynamic", "kinematic"};
    j["rigidBody"]["type"] = types[(int)rb.Type];
    j["rigidBody"]["mass"] = rb.Mass;
    j["rigidBody"]["friction"] = rb.Friction;
    j["rigidBody"]["restitution"] = rb.Restitution;
    j["rigidBody"]["layer"] = (unsigned)rb.Layer;
    j["rigidBody"]["sensor"] = rb.Sensor;
    j["rigidBody"]["triggerMask"] = (unsigned)rb.TriggerMask;
    j["rigidBody"]["linearDamping"] = rb.LinearDamping;
    j["rigidBody"]["angularDamping"] = rb.AngularDamping;
    j["rigidBody"]["gravityScale"] = rb.GravityScale;
    j["rigidBody"]["continuous"] = rb.Continuous;
    j["rigidBody"]["locks"] = (unsigned)rb.Locks;
}

RigidBodyComponent ParseRigidBody(const json& rj) {
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
    rb.TriggerMask = rj.value("triggerMask", (unsigned)rb.TriggerMask);
    // Сцены, сохранённые до этих полей, получают значения по умолчанию —
    // те же, с которыми тела и так строились.
    rb.LinearDamping = rj.value("linearDamping", rb.LinearDamping);
    rb.AngularDamping = rj.value("angularDamping", rb.AngularDamping);
    rb.GravityScale = rj.value("gravityScale", rb.GravityScale);
    rb.Continuous = rj.value("continuous", rb.Continuous);
    rb.Locks = (uint8_t)(rj.value("locks", 0u) & 0x3Fu);
    return rb;
}

static const char* kShapeNames[] = {"box", "sphere", "capsule", "convex", "mesh"};
sage::physics::ShapeType ShapeFromStr(const std::string& s) {
    return s == "sphere" ? sage::physics::ShapeType::Sphere
         : s == "capsule" ? sage::physics::ShapeType::Capsule
         : s == "convex" ? sage::physics::ShapeType::ConvexHull
         : s == "mesh" ? sage::physics::ShapeType::Mesh
         : sage::physics::ShapeType::Box;
}

void SaveCollider(json& j, const ColliderComponent& col) {
    j["collider"]["shape"] = kShapeNames[(int)col.Shape];
    j["collider"]["halfExtents"] = Vec3ToJson(col.HalfExtents);
    j["collider"]["radius"] = col.Radius;
    j["collider"]["halfHeight"] = col.HalfHeight;
    // Ссылкой на ассет (GUID + путь), как меш и материал: переименованная
    // модель коллайдера не должна молча превращать его в коробку.
    if (!col.MeshPath.empty()) SaveAssetRef(j["collider"], "mesh", col.MeshPath);
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

ColliderComponent ParseCollider(const json& cj) {
    ColliderComponent col;
    col.Shape = ShapeFromStr(cj.value("shape", "box"));
    if (cj.contains("halfExtents")) col.HalfExtents = Vec3FromJson(cj["halfExtents"]);
    col.Radius = cj.value("radius", col.Radius);
    col.HalfHeight = cj.value("halfHeight", col.HalfHeight);
    if (cj.contains("mesh")) col.MeshPath = LoadAssetRef(cj, "mesh");
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
void SaveJoint(json& j, const JointComponent& jc) {
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

JointComponent ParseJoint(const json& jj) {
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

// Зонд отражений: сохраняется ЗАДАНИЕ (где, какой охват, какое разрешение), но
// не снятая карта — она пересобирается из той же геометрии и того же неба, а
// шесть картинок в проекте устаревали бы от любой правки уровня.
// Контроллер персонажа: сохраняются размеры и правила ходьбы, но не сам
// контроллер — он живёт в физическом мире и создаётся заново при запуске.
void SaveCharacter(json& j, const CharacterControllerComponent& c) {
    j["character"]["radius"] = c.Radius;
    j["character"]["height"] = c.Height;
    j["character"]["stepOffset"] = c.StepOffset;
    j["character"]["slopeLimit"] = c.SlopeLimit;
    j["character"]["center"] = Vec3ToJson(c.Center);
    j["character"]["skinWidth"] = c.SkinWidth;
    j["character"]["gravity"] = c.Gravity;
    j["character"]["mass"] = c.Mass;
    j["character"]["layer"] = (unsigned)c.Layer;
    j["character"]["collidesWith"] = (unsigned)c.CollidesWith;
    j["character"]["collisionEvents"] = c.CollisionEvents;
}

CharacterControllerComponent ParseCharacter(const json& cj) {
    CharacterControllerComponent c;
    c.Radius = cj.value("radius", c.Radius);
    c.Height = cj.value("height", c.Height);
    // Прежние имена читаются по-прежнему: сцены, сделанные до переименования
    // параметров, обязаны открываться, а не терять настройку персонажа молча.
    c.StepOffset = cj.value("stepOffset", cj.value("stepHeight", c.StepOffset));
    c.SlopeLimit = cj.value("slopeLimit", cj.value("maxSlopeDeg", c.SlopeLimit));
    if (cj.contains("center")) c.Center = Vec3FromJson(cj["center"]);
    c.SkinWidth = cj.value("skinWidth", c.SkinWidth);
    c.Gravity = cj.value("gravity", c.Gravity);
    c.Mass = cj.value("mass", c.Mass);
    c.Layer = cj.value("layer", (unsigned)c.Layer);
    c.CollidesWith = cj.value("collidesWith", (unsigned)c.CollidesWith);
    c.CollisionEvents = cj.value("collisionEvents", c.CollisionEvents);
    return c;
}

void SaveReflectionProbe(json& j, const ReflectionProbeComponent& p) {
    j["reflectionProbe"]["resolution"] = p.Resolution;
    j["reflectionProbe"]["boxHalfExtents"] = Vec3ToJson(p.BoxHalfExtents);
    j["reflectionProbe"]["intensity"] = p.Intensity;
    j["reflectionProbe"]["boxParallax"] = p.BoxParallax;
    j["reflectionProbe"]["farClip"] = p.FarClip;
    j["reflectionProbe"]["realtime"] = p.Realtime;
}

ReflectionProbeComponent ParseReflectionProbe(const json& pj) {
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
void SaveLod(json& j, const sage::render::LodComponent& lod) {
    j["lod"]["auto"] = lod.Auto;
    j["lod"]["autoCount"] = lod.AutoCount;
    j["lod"]["cullBelow"] = lod.Levels.CullBelow;
    j["lod"]["screenHeights"] = lod.Levels.ScreenHeights;
}

sage::render::LodComponent ParseLod(const json& lj) {
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
void SaveShaderParams(json& j, const ShaderParamsComponent& sp) {
    json params = json::object();
    for (const auto& [name, value] : sp.Params) {
        params[name] = {{"kind", (int)value.Kind},
                        {"value", {value.Value.x, value.Value.y, value.Value.z, value.Value.w}}};
    }
    j["shaderParams"] = params;
}

ShaderParamsComponent ParseShaderParams(const json& sj) {
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

void SaveAnimation(json& j, const AnimationComponent& am) {
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

AnimationComponent ParseAnimation(const json& aj) {
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
void SavePropertyAnimator(json& j, const PropertyAnimatorComponent& a) {
    json& oj = j["propertyAnimator"];
    oj["clipPath"] = a.ClipPath;
    oj["playing"] = a.Playing;
    oj["loop"] = a.Loop;
    oj["speed"] = a.Speed;
    // Время НЕ пишется: это положение бегунка сейчас, а не свойство сцены.
    // Сохранённое «сейчас середина» означало бы сцену, которая открывается с
    // движением, начатым неизвестно кем.
}

PropertyAnimatorComponent ParsePropertyAnimator(const json& aj) {
    PropertyAnimatorComponent a;
    a.ClipPath = aj.value("clipPath", std::string());
    a.Playing = aj.value("playing", true);
    a.Loop = aj.value("loop", true);
    a.Speed = aj.value("speed", 1.0f);
    return a;   // Clip/Ready восстановит первый UpdatePropertyAnimators
}

// Твины объекта — прямо в сцене, а не отдельным файлом, как клип: твин — это
// «кнопка вздувается при наведении» на ЭТОЙ кнопке, два числа и кривая, и
// заводить ради него файл — ровно та тяжесть, от которой твин избавляет.
// Кривая пишется словом («back-out», «curve(…)») — файл читается глазами.
void SaveTweens(json& j, const sage::anim::TweenComponent& tc) {
    json list = json::array();
    for (const sage::anim::TweenClip& c : tc.Tweens) {
        json cj;
        cj["name"] = c.Name;
        if (c.Target != 0) cj["target"] = c.Target;
        if (c.Delay != 0.0f) cj["delay"] = c.Delay;
        if (c.Loop != sage::anim::TweenLoop::Once) cj["loop"] = sage::anim::LoopName(c.Loop);
        if (c.Speed != 1.0f) cj["speed"] = c.Speed;
        if (c.Reverse) cj["reverse"] = true;
        if (c.PlayOnStart) cj["playOnStart"] = true;
        if (!c.Then.empty()) cj["then"] = c.Then;
        json tracks = json::array();
        for (const sage::anim::TweenTrack& t : c.Tracks) {
            json tj;
            tj["property"] = t.Property;   // пусто — пауза в последовательности
            tj["start"] = t.Start;
            tj["duration"] = t.Duration;
            if (!t.Property.empty()) {
                tj["to"] = Vec4ToJson(t.To);
                if (!t.FromCurrent) tj["from"] = Vec4ToJson(t.From);
                if (t.ToCurrent) tj["toCurrent"] = true;
                tj["ease"] = sage::anim::ToString(t.Curve);
            }
            tracks.push_back(tj);
        }
        cj["tracks"] = tracks;
        list.push_back(cj);
    }
    j["tweens"] = list;
}

sage::anim::TweenComponent ParseTweens(const json& list) {
    sage::anim::TweenComponent tc;
    if (!list.is_array()) return tc;
    for (const json& cj : list) {
        sage::anim::TweenClip c;
        c.Name = cj.value("name", std::string("Tween"));
        c.Target = cj.value("target", 0);
        c.Delay = cj.value("delay", 0.0f);
        sage::anim::ParseLoop(cj.value("loop", std::string("once")), c.Loop);
        c.Speed = cj.value("speed", 1.0f);
        c.Reverse = cj.value("reverse", false);
        c.PlayOnStart = cj.value("playOnStart", false);
        c.Then = cj.value("then", std::string());
        if (cj.contains("tracks") && cj["tracks"].is_array()) {
            for (const json& tj : cj["tracks"]) {
                sage::anim::TweenTrack t;
                t.Property = tj.value("property", std::string());
                t.Start = tj.value("start", 0.0f);
                t.Duration = tj.value("duration", 1.0f);
                if (tj.contains("to")) t.To = Vec4FromJson(tj["to"], t.To);
                if (tj.contains("from")) {
                    t.From = Vec4FromJson(tj["from"], t.From);
                    t.FromCurrent = false;
                }
                t.ToCurrent = tj.value("toCurrent", false);
                sage::anim::Parse(tj.value("ease", std::string("quad-out")), t.Curve);
                c.Tracks.push_back(t);
            }
        }
        tc.Tweens.push_back(std::move(c));
    }
    return tc;
}

// IK: сохраняем только ЗАДАНИЕ (какая кость, куда тянем, как), но не результат.
// EndJoint/MidJoint/RootJoint — это индексы в конкретном скелете, они
// разрешаются заново после загрузки модели, а Locked/LockedAt — состояние
// текущего шага, которое переживать перезагрузку сцены не должно.
void SaveIK(json& j, const IKComponent& ik) {
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

IKComponent ParseIK(const json& ij) {
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

sage::scene::LegacyElement::Kind UIKindFromString(const std::string& s) {
    if (s == "label") return sage::scene::LegacyElement::Kind::Label;
    if (s == "image") return sage::scene::LegacyElement::Kind::Image;
    if (s == "bar")   return sage::scene::LegacyElement::Kind::Bar;
    if (s == "icon")  return sage::scene::LegacyElement::Kind::Icon;
    if (s == "input") return sage::scene::LegacyElement::Kind::Input;
    if (s == "checkbox") return sage::scene::LegacyElement::Kind::Checkbox;
    if (s == "slider") return sage::scene::LegacyElement::Kind::Slider;
    return sage::scene::LegacyElement::Kind::Panel;
}

sage::scene::LegacyElement ParseUIElement(const json& uj) {
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
    u.Nearest = uj.value("pixelArt", u.Nearest);   // ключ старого формата
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
        // Фильтрация «ближайший пиксель» — без мипмапов: иначе набор спрайтов
        // размывается, а мипмапы ЛИСТА подмешивают в края соседний спрайт.
        u.Tex = u.Nearest
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
void SaveAudio(json& j, const AudioSourceComponent& a) {
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

AudioSourceComponent ParseAudio(const json& aj) {
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

void SaveParticles(json& j, const ParticleEmitterComponent& pe) {
    json& pj = j["particles"];
    pj["playing"] = pe.Playing;
    // Эффект — тем же форматом, что и файл .sagefx: эмиттер, сохранённый в
    // файл из инспектора, и эмиттер в сцене — одно и то же.
    pj["effect"] = json::parse(sage::fx::EffectToJson(pe.Effect));
}

// СТАРЫЙ ЭМИТТЕР (две точки «начало/конец», направление коробкой, гравитация
// по Y) — переводится в новый эффект так, чтобы выглядеть как раньше. Сцены,
// сохранённые до переделки частиц, открываются без потерь.
sage::fx::ParticleEffect EffectFromLegacy(const json& pj) {
    using namespace sage::fx;
    ParticleEffect f;
    const glm::vec3 dmin = pj.contains("directionMin") ? Vec3FromJson(pj["directionMin"]) : glm::vec3(-0.5f, 0.5f, -0.5f);
    const glm::vec3 dmax = pj.contains("directionMax") ? Vec3FromJson(pj["directionMax"]) : glm::vec3(0.5f, 1.5f, 0.5f);
    const glm::vec3 mid = (dmin + dmax) * 0.5f;
    const float spread = std::max({std::abs(dmin.x), std::abs(dmax.x), std::abs(dmin.z), std::abs(dmax.z)});
    f.Shape = EmitShape::Cone;
    f.Radius = 0.0f;
    f.ConeAngle = glm::degrees(std::atan2(spread, std::max(std::abs(mid.y), 1e-3f)));
    if (mid.y < 0.0f) f.ShapeRotation = glm::vec3(180.0f, 0.0f, 0.0f);
    f.StartSpeed = {pj.value("speedMin", 1.0f), pj.value("speedMax", 2.0f)};
    f.GravityScale = -pj.value("gravity", -2.0f) / 9.81f;
    f.StartLifetime = {pj.value("lifetimeMin", 0.5f), pj.value("lifetimeMax", 1.0f)};
    f.StartSize = {pj.value("startSizeMin", 0.08f), pj.value("startSizeMax", 0.15f)};
    const float startAvg = std::max((f.StartSize.Min + f.StartSize.Max) * 0.5f, 1e-4f);
    const float endAvg = (pj.value("endSizeMin", 0.0f) + pj.value("endSizeMax", 0.02f)) * 0.5f;
    f.UseSizeOverLifetime = true;
    f.SizeOverLifetime = Curve::Linear(1.0f, endAvg / startAvg);
    const glm::vec4 c0 = pj.contains("startColor") ? Vec4FromJson(pj["startColor"]) : glm::vec4(1.0f);
    const glm::vec4 c1 = pj.contains("endColor") ? Vec4FromJson(pj["endColor"], glm::vec4(1, 1, 1, 0)) : glm::vec4(1, 1, 1, 0);
    f.StartColorA = f.StartColorB = glm::vec4(1.0f);
    f.UseColorOverLifetime = true;
    f.ColorOverLifetime = Gradient::Fade(c0, c1);
    const float spin = pj.value("angularVelocityMax", 0.0f);
    if (spin > 0.0f) {
        f.UseRotation = true;
        f.AngularVelocity = {-glm::degrees(spin), glm::degrees(spin)};
    }
    f.Sprite = pj.value("shape", std::string("circle")) == "quad" ? ParticleSprite::Square : ParticleSprite::SoftCircle;
    if (pj.value("continuous", true)) {
        f.RateOverTime = pj.value("emissionRate", 20.0f);
    } else {
        f.RateOverTime = 0.0f;
        const float interval = std::max(pj.value("burstInterval", 1.5f), 0.05f);
        const int count = pj.value("burstCount", 24);
        f.Duration = interval;
        f.Bursts.push_back({0.0f, count, count, 1, interval});
    }
    return f;
}

ParticleEmitterComponent ParseParticles(const json& pj) {
    ParticleEmitterComponent pe;
    if (pj.contains("effect") && pj["effect"].is_object()) {
        sage::fx::EffectFromJson(pj["effect"].dump(), pe.Effect);
        pe.Playing = pj.value("playing", true);
    } else {
        pe.Effect = EffectFromLegacy(pj);
        pe.Playing = pj.value("active", true);
    }
    return pe;
}


} // namespace scene_serial
