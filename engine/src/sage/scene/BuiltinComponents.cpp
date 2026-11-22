#include "sage/scene/BuiltinComponents.h"

#include <stdexcept>

#include "sage/scene/SceneSerializerDetail.h"

namespace sage::scene {

namespace {

using namespace scene_serial;

// Одна строка на компонент: тип, ключ в объекте, запись и разбор сериализатора.
template <typename T, typename SaveFn, typename ParseFn>
ComponentType Make(const char* key, SaveFn save, ParseFn parse) {
    return MakeTopLevelComponentType<T>(key, save, parse);
}

template <typename T, typename SaveFn, typename ParseFn>
void Builtin(const char* key, SaveFn save, ParseFn parse) {
    RegisterComponent(Make<T>(key, save, parse));
}

// Наклейка: сохраняются ПАРАМЕТРЫ проекции, а не её результат — геометрия
// наклейки производна от сцены, и хранить её значило бы держать в файле копию
// соседних мешей, устаревающую от любой их правки.
void SaveDecal(json& j, const DecalComponent& dc) {
    j["decal"]["angleLimit"] = dc.AngleLimitDeg;
    j["decal"]["offset"] = dc.Offset;
}

DecalComponent ParseDecal(const json& d) {
    DecalComponent dc;
    dc.AngleLimitDeg = d.value("angleLimit", dc.AngleLimitDeg);
    dc.Offset = d.value("offset", dc.Offset);
    dc.Dirty = true;   // геометрию строит система после загрузки сцены
    return dc;
}

// Съёмка в текстуру: только настройки, не картинка — её снимают заново.
void SaveRenderTexture(json& j, const RenderTextureComponent& rt) {
    json& o = j["renderTexture"];
    o["target"] = rt.Target;
    o["width"] = rt.Width;
    o["height"] = rt.Height;
    o["clearColor"] = Vec4ToJson(rt.ClearColor);
    o["ortho"] = rt.Ortho;
    o["orthoSize"] = rt.OrthoSize;
    o["fov"] = rt.Fov;
    o["near"] = rt.Near;
    o["far"] = rt.Far;
    o["lookAt"] = Vec3ToJson(rt.LookAt);
    o["continuous"] = rt.Continuous;
    o["update"] = (int)rt.Mode;
    if (rt.Aspect > 0.0f) o["aspect"] = rt.Aspect;
    o["studioLight"] = rt.StudioLight;
    o["lightDir"] = Vec3ToJson(rt.LightDir);
    o["lightIntensity"] = rt.LightIntensity;
    o["ambient"] = rt.Ambient;
}

RenderTextureComponent ParseRenderTexture(const json& o) {
    if (!o.is_object()) throw std::runtime_error("renderTexture — не объект");
    RenderTextureComponent rt;
    rt.Target = o.value("target", rt.Target);
    rt.Width = o.value("width", rt.Width);
    rt.Height = o.value("height", rt.Height);
    if (o.contains("clearColor")) rt.ClearColor = Vec4FromJson(o["clearColor"], rt.ClearColor);
    rt.Ortho = o.value("ortho", rt.Ortho);
    rt.OrthoSize = o.value("orthoSize", rt.OrthoSize);
    rt.Fov = o.value("fov", rt.Fov);
    rt.Near = o.value("near", rt.Near);
    rt.Far = o.value("far", rt.Far);
    if (o.contains("lookAt")) rt.LookAt = Vec3FromJson(o["lookAt"]);
    rt.Continuous = o.value("continuous", rt.Continuous);
    rt.Mode = (RenderTextureComponent::Update)std::clamp(o.value("update", (int)rt.Mode), 0, 2);
    // Старая сцена: галка «каждый кадр» и есть режим Continuous.
    if (rt.Continuous && !o.contains("update")) rt.Mode = RenderTextureComponent::Update::Continuous;
    rt.Aspect = o.value("aspect", rt.Aspect);
    rt.StudioLight = o.value("studioLight", rt.StudioLight);
    if (o.contains("lightDir")) rt.LightDir = Vec3FromJson(o["lightDir"]);
    rt.LightIntensity = o.value("lightIntensity", rt.LightIntensity);
    rt.Ambient = o.value("ambient", rt.Ambient);
    rt.Dirty = true;
    return rt;
}

// Папка списка: сама метка и её цвет. Без неё «Декорации» после перезагрузки
// становились обычным пустым объектом и снова таскали за собой содержимое.
void SaveFolder(json& j, const FolderComponent& fc) { j["folder"]["color"] = Vec3ToJson(fc.Color); }

FolderComponent ParseFolder(const json& f) {
    FolderComponent fc;
    if (f.contains("color")) fc.Color = Vec3FromJson(f["color"]);
    return fc;
}

void SaveTag(json& j, const TagComponent& t) {
    if (!t.Tag.empty()) j["tag"] = t.Tag;
}

TagComponent ParseTag(const json& t) {
    if (!t.is_string()) throw std::runtime_error("tag — не строка");
    return TagComponent{t.get<std::string>()};
}

void SaveNetReplicated(json& j, const NetReplicatedComponent&) { j["netReplicated"] = true; }

NetReplicatedComponent ParseNetReplicated(const json& v) {
    if (!v.is_boolean()) throw std::runtime_error("netReplicated — не логическое значение");
    return NetReplicatedComponent{};
}

// "netReplicated": false — метки нет (так файл и читался всегда).
void LoadNetReplicated(entt::registry& r, entt::entity e, const json& v) {
    ParseNetReplicated(v);
    if (v.get<bool>()) r.emplace_or_replace<NetReplicatedComponent>(e);
}

// ИНТЕРФЕЙС — не часть элемента, а его ГРАНИЦА (см.
// sage/ui/components/InterfaceComponent.h): отдельным ключом, а не внутри "ui".
void SaveInterface(json& j, const sage::ui::InterfaceComponent& ic) {
    json ij;
    SaveAssetRef(ij, "source", ic.Source);
    ij["visible"] = ic.Visible;
    ij["sortOrder"] = ic.SortOrder;
    ij["receivesInput"] = ic.ReceivesInput;
    ij["canvasMode"] = (int)ic.Canvas.Mode;
    ij["reference"] = Vec2ToJson(ic.Canvas.Reference);
    ij["matchWidthOrHeight"] = ic.Canvas.MatchWidthOrHeight;
    j["interface"] = std::move(ij);
}

sage::ui::InterfaceComponent ParseInterface(const json& ij) {
    sage::ui::InterfaceComponent ic;
    ic.Source = LoadAssetRef(ij, "source");
    ic.Visible = ij.value("visible", true);
    ic.SortOrder = ij.value("sortOrder", 0);
    ic.ReceivesInput = ij.value("receivesInput", true);
    ic.Canvas.Mode = (sage::ui::Canvas::Scale)ij.value("canvasMode", 0);
    if (ij.contains("reference")) ic.Canvas.Reference = Vec2FromJson(ij["reference"], ic.Canvas.Reference);
    ic.Canvas.MatchWidthOrHeight = ij.value("matchWidthOrHeight", 0.5f);
    return ic;
}

} // namespace

void RegisterBuiltinSceneComponents() {
    Builtin<GIStaticComponent>("giStatic", &SaveGIStatic, &ParseGIStatic);
    Builtin<JointComponent>("joint", &SaveJoint, &ParseJoint);
    Builtin<PropertyAnimatorComponent>("propertyAnimator", &SavePropertyAnimator, &ParsePropertyAnimator);
    Builtin<sage::anim::TweenComponent>("tweens", &SaveTweens, &ParseTweens);
    Builtin<IKComponent>("ik", &SaveIK, &ParseIK);
    Builtin<CharacterControllerComponent>("character", &SaveCharacter, &ParseCharacter);
    Builtin<ReflectionProbeComponent>("reflectionProbe", &SaveReflectionProbe, &ParseReflectionProbe);
    Builtin<sage::render::LodComponent>("lod", &SaveLod, &ParseLod);
    Builtin<ShaderParamsComponent>("shaderParams", &SaveShaderParams, &ParseShaderParams);
    Builtin<ParticleEmitterComponent>("particles", &SaveParticles, &ParseParticles);
    Builtin<AudioSourceComponent>("audio", &SaveAudio, &ParseAudio);
    Builtin<CameraComponent>("camera", &SaveCamera, &ParseCamera);
    Builtin<LightComponent>("light", &SaveLight, &ParseLight);
    Builtin<RigidBodyComponent>("rigidBody", &SaveRigidBody, &ParseRigidBody);
    Builtin<ColliderComponent>("collider", &SaveCollider, &ParseCollider);
    Builtin<AnimationComponent>("animation", &SaveAnimation, &ParseAnimation);
    Builtin<DecalComponent>("decal", &SaveDecal, &ParseDecal);
    Builtin<RenderTextureComponent>("renderTexture", &SaveRenderTexture, &ParseRenderTexture);
    Builtin<sage::ui::InterfaceComponent>("interface", &SaveInterface, &ParseInterface);

    // "postChain" — имя пост-обработки ДО того, как она стала «Пост-обработкой»:
    // сцена, сохранённая раньше, обязана открыться (миграция v11->v12 переводит
    // остальное).
    ComponentType post = Make<sage::render::PostProcessComponent>("postProcess", &SavePostProcess, &ParsePostProcess);
    post.OldKeys = {"postChain"};
    RegisterComponent(std::move(post));

    // Тег: пустая строка в файле — «тега нет», как и раньше (поиск по тегу
    // не должен находить объект с пустым тегом).
    ComponentType tag = Make<TagComponent>("tag", &SaveTag, &ParseTag);
    tag.Load = [](entt::registry& r, entt::entity e, const json& j) {
        TagComponent t = ParseTag(j);
        if (!t.Tag.empty()) r.emplace_or_replace<TagComponent>(e, std::move(t));
    };
    RegisterComponent(std::move(tag));

    // Папка: у неё нет положения — сдвиг, оставшийся в старом файле, ни на
    // что не влиял, а в инспекторе и гизмо выглядел бы рабочим. Transform к
    // этому моменту уже прочитан сериализатором.
    ComponentType folder = Make<FolderComponent>("folder", &SaveFolder, &ParseFolder);
    folder.Load = [](entt::registry& r, entt::entity e, const json& j) {
        r.emplace_or_replace<FolderComponent>(e, ParseFolder(j));
        if (Transform* t = r.try_get<Transform>(e)) *t = Transform{};
    };
    RegisterComponent(std::move(folder));

    ComponentType net = Make<NetReplicatedComponent>("netReplicated", &SaveNetReplicated, &ParseNetReplicated);
    net.Load = &LoadNetReplicated;
    RegisterComponent(std::move(net));
}

} // namespace sage::scene
