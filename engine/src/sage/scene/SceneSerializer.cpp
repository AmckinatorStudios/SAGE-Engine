#include "SceneSerializer.h"
#include "sage/scene/SceneSerializerDetail.h"
#include "sage/audio/AudioComponents.h"
#include "sage/assets/Pack.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/LodGroup.h"
#include "sage/gi/GI.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ResourceManager.h"
#include <nlohmann/json.hpp>
#include "sage/render/ParticleEffectIO.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <vector>
#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/scene/SceneLegacyUI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"
#include "sage/scene/SceneJson.h"
#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/SceneValueJson.h"
#include "sage/scene/SceneMigrations.h"

using namespace scene_serial;


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


namespace SceneSerializer {

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
            // Место в списке — только если его меняли: у сцен, где порядок «по
            // номеру», файл не меняется ни на байт.
            if (h->Order != 0) j["order"] = h->Order;
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
            if (mr.Ref.part >= 0) j["mesh"]["part"] = mr.Ref.part;
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
        if (reg.all_of<KeepOnLoadComponent>(e)) j["keepOnLoad"] = true;
        // Компоненты игры (реестр) и непонятые этим кодом — как есть.
        sage::scene::SaveRegisteredComponents(reg, e, j);
        // Съёмка в текстуру: только настройки, не картинка — её снимают заново.
        // Раньше компонент не писался вовсе, и иконка предмета или экран
        // камеры наблюдения пропадали после сохранения сцены.
        if (const RenderTextureComponent* rt = reg.try_get<RenderTextureComponent>(e)) {
            nlohmann::json& o = j["renderTexture"];
            o["target"] = rt->Target;
            o["width"] = rt->Width;
            o["height"] = rt->Height;
            o["clearColor"] = Vec4ToJson(rt->ClearColor);
            o["ortho"] = rt->Ortho;
            o["orthoSize"] = rt->OrthoSize;
            o["fov"] = rt->Fov;
            o["near"] = rt->Near;
            o["far"] = rt->Far;
            o["lookAt"] = Vec3ToJson(rt->LookAt);
            o["continuous"] = rt->Continuous;
            o["studioLight"] = rt->StudioLight;
            o["lightDir"] = Vec3ToJson(rt->LightDir);
            o["lightIntensity"] = rt->LightIntensity;
            o["ambient"] = rt->Ambient;
        }
        if (const FolderComponent* fc = reg.try_get<FolderComponent>(e)) {
            j["folder"]["color"] = Vec3ToJson(fc->Color);
        }
        if (const TagComponent* tag = reg.try_get<TagComponent>(e))
            if (!tag->Tag.empty()) j["tag"] = tag->Tag;
        if (const ScriptComponent* sc = reg.try_get<ScriptComponent>(e)) {
            SaveAssetRef(j, "script", sc->Path);
            // Значения публичных переменных скрипта — рядом с самим скриптом:
            // они принадлежат ЭТОМУ объекту, а не файлу, и без них скрипт при
            // следующей загрузке получил бы умолчания вместо настройки.
            if (!sc->Fields.Empty()) j["scriptVars"] = VarsToJson(sc->Fields);
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
        if (const sage::anim::TweenComponent* tw = reg.try_get<sage::anim::TweenComponent>(e))
            SaveTweens(j, *tw);
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
        if (const sage::signals::SignalLinksComponent* sl =
                reg.try_get<sage::signals::SignalLinksComponent>(e))
            if (!sl->Links.empty()) j["signalLinks"] = LinksToJson(sl->Links);
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
        if (j.contains("order"))
            scene->Registry().get_or_emplace<HierarchyComponent>(obj.Entity()).Order = j.value("order", 0);

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
            mr.Ref.part = j["mesh"].value("part", -1);
        }

        if (j.contains("decal")) {
            DecalComponent dc;
            dc.AngleLimitDeg = j["decal"].value("angleLimit", dc.AngleLimitDeg);
            dc.Offset = j["decal"].value("offset", dc.Offset);
            dc.Dirty = true; // геометрию строит система после загрузки сцены
            obj.Registry()->emplace<DecalComponent>(obj.Entity(), dc);
        }

        if (j.value("hidden", false)) obj.Registry()->emplace<HiddenComponent>(obj.Entity());
        if (j.value("keepOnLoad", false)) obj.Registry()->emplace<KeepOnLoadComponent>(obj.Entity());
        sage::scene::LoadRegisteredComponents(*obj.Registry(), obj.Entity(), j);
        if (j.contains("renderTexture") && j["renderTexture"].is_object()) {
            const nlohmann::json& o = j["renderTexture"];
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
            rt.StudioLight = o.value("studioLight", rt.StudioLight);
            if (o.contains("lightDir")) rt.LightDir = Vec3FromJson(o["lightDir"]);
            rt.LightIntensity = o.value("lightIntensity", rt.LightIntensity);
            rt.Ambient = o.value("ambient", rt.Ambient);
            rt.Dirty = true;
            obj.Registry()->emplace<RenderTextureComponent>(obj.Entity(), rt);
        }
        if (j.contains("folder")) {
            FolderComponent fc;
            if (j["folder"].contains("color")) fc.Color = Vec3FromJson(j["folder"]["color"]);
            obj.Registry()->emplace<FolderComponent>(obj.Entity(), fc);
            // У папки нет положения: сдвиг, оставшийся в старом файле, ни на
            // что не влиял, а в инспекторе и гизмо выглядел бы рабочим.
            if (Transform* ft = obj.Registry()->try_get<Transform>(obj.Entity())) *ft = Transform{};
        }
        if (j.contains("tag") && j["tag"].is_string()) {
            const std::string tag = j["tag"].get<std::string>();
            if (!tag.empty()) obj.Registry()->emplace<TagComponent>(obj.Entity(), TagComponent{tag});
        }
        if (j.contains("script")) {
            ScriptComponent sc;
            sc.Path = LoadAssetRef(j, "script");
            if (j.contains("scriptVars")) VarsFromJson(j["scriptVars"], sc.Fields);
            obj.Registry()->emplace<ScriptComponent>(obj.Entity(), std::move(sc));
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
        if (j.contains("tweens"))
            obj.Registry()->emplace<sage::anim::TweenComponent>(obj.Entity(), ParseTweens(j["tweens"]));
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
        if (j.contains("signalLinks")) {
            // ДОБАВЛЕНИЕМ, а не заменой: связи, переведённые из старого формата
            // при чтении "ui" выше, не должны пропасть.
            std::vector<sage::signals::Link> links;
            LinksFromJson(j["signalLinks"], links);
            if (!links.empty()) {
                auto& sl = obj.Registry()->get_or_emplace<sage::signals::SignalLinksComponent>(obj.Entity());
                sl.Links.insert(sl.Links.end(), links.begin(), links.end());
            }
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
            mr.MeshPtr = animated ? nullptr : ResourceManager::Instance().GetModel(mr.Ref.path, mr.Ref.part);
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
    // Интерфейс старого вида (раскладка «галкой» у видимого элемента, старые
    // имена типов) — к нынешнему. После SetNextId: деление заводит объекты.
    sage::ui::NormalizeElements(*scene);
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
