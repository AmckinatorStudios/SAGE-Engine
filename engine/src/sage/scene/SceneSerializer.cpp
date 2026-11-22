#include "SceneSerializer.h"
#include "sage/core/AtomicFile.h"
#include "sage/scene/SceneSerializerDetail.h"
#include "sage/audio/AudioComponents.h"
#include "sage/assets/Pack.h"
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
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"
#include "sage/ui/InterfaceSource.h"
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

// НАСТРОЙКИ ГЕОМЕТРИИ ПРИМИТИВА — только то, что задано (не ноль): у сцен
// без настроек блока нет вовсе, и читаются они прежней геометрией.
static json PrimitiveToJson(const sage::render::PrimitiveParams& p) {
    json g = json::object();
    if (p.SubX || p.SubY || p.SubZ) g["subdivisions"] = json::array({p.SubX, p.SubY, p.SubZ});
    if (p.Segments) g["segments"] = p.Segments;
    if (p.Rings) g["rings"] = p.Rings;
    if (p.HeightSegments) g["heightSegments"] = p.HeightSegments;
    if (p.Height > 0.0f) g["height"] = p.Height;
    if (p.Flat) g["flat"] = true;
    return g;
}

static sage::render::PrimitiveParams PrimitiveFromJson(const json& g) {
    sage::render::PrimitiveParams p;
    if (!g.is_object()) return p;
    if (g.contains("subdivisions") && g["subdivisions"].is_array() && g["subdivisions"].size() == 3) {
        p.SubX = g["subdivisions"][0].get<int>();
        p.SubY = g["subdivisions"][1].get<int>();
        p.SubZ = g["subdivisions"][2].get<int>();
    }
    p.Segments = g.value("segments", 0);
    p.Rings = g.value("rings", 0);
    p.HeightSegments = g.value("heightSegments", 0);
    p.Height = g.value("height", 0.0f);
    p.Flat = g.value("flat", false);
    return p;
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
        // Развёрнутый из .sageui интерфейс — копия файла, а не часть сцены
        // (sage/ui/InterfaceSource.h): в сцену пишется только ссылка на файл.
        if (reg.all_of<sage::ui::InterfaceInstanceTag>(e)) continue;
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
        // Опора — только если её задали: сцены без неё не меняются ни на байт,
        // а старые без ключа открываются с опорой в начале координат объекта.
        if (tr.Pivot != glm::vec3(0.0f)) j["pivot"] = Vec3ToJson(tr.Pivot);
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
            if (!mr.Layer.empty()) j["layer"] = mr.Layer;
            // СВЕЧЕНИЯ И ПРОЗРАЧНОСТИ У ЭКЗЕМПЛЯРА БОЛЬШЕ НЕТ: и то и другое —
            // свойство материала (см. MeshRendererComponent). Ключи opacity,
            // emissive и emissiveStrength не пишутся и не читаются: сцена,
            // сохранённая старым редактором, открывается, но эти значения
            // теряет — хранить их было бы обещанием, что они на что-то влияют.
            j["mesh"]["type"] = MeshTypeToString(mr.Ref.type);
            SaveAssetRef(j["mesh"], "path", mr.Ref.path);
            if (mr.Ref.part >= 0) j["mesh"]["part"] = mr.Ref.part;
            if (mr.Ref.IsPrimitive() && !mr.Ref.prim.IsDefault()) j["mesh"]["geometry"] = PrimitiveToJson(mr.Ref.prim);
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
        // Выключенное остаётся выключенным и после перезагрузки, и в собранной
        // игре: иначе «выключил и забыл» означало бы сюрприз ровно в тот
        // момент, когда игру собрали и раздали.
        if (reg.all_of<HiddenComponent>(e)) j["hidden"] = true;
        if (reg.all_of<KeepOnLoadComponent>(e)) j["keepOnLoad"] = true;
        // Компоненты — реестром (sage/scene/ComponentRegistry.h, встроенные —
        // BuiltinComponents.cpp): камера, свет, физика, звук и компоненты игры.
        // Новый компонент — туда, а не веткой здесь (scripts/check_components.py).
        sage::scene::SaveRegisteredComponents(reg, e, j);
        if (const ScriptComponent* sc = reg.try_get<ScriptComponent>(e)) SaveScripts(j, *sc);
        if (const VarsComponent* vc = reg.try_get<VarsComponent>(e))
            if (!vc->Values.Empty()) j["vars"] = VarsToJson(vc->Values);
        if (const sage::signals::SignalLinksComponent* sl =
                reg.try_get<sage::signals::SignalLinksComponent>(e))
            if (!sl->Links.empty()) j["signalLinks"] = LinksToJson(sl->Links);
        // Элемент и его компоненты — общей записью (см. sage/ui/UISerialize.h):
        // тот же формат, каким интерфейс ложится в отдельный ресурс .sageui.
        if (json ui; sage::ui::SaveElement(ui, reg, e)) j["ui"] = std::move(ui);
        objectsJson.push_back(j);
    }
    root["objects"] = objectsJson;
    root["lighting"] = LightingToJson(scene.Lighting);
    root["reflections"] = ReflectionsToJson(scene.Reflections);
    if (scene.GI) root["gi"] = GIToJson(*scene.GI, withProbes);
    if (!scene.Layers.empty()) root["layers"] = scene.Layers;
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
        if (j.contains("pivot"))    tr.Pivot    = Vec3FromJson(j["pivot"]);
        if (j.contains("color"))    mr.Color    = Vec3FromJson(j["color"]);
        mr.CastShadows = j.value("castShadows", mr.CastShadows);
        mr.InReflections = j.value("inReflections", mr.InReflections);
        mr.Layer = j.value("layer", mr.Layer);
        // opacity / emissive / emissiveStrength из старых сцен НЕ читаются:
        // полей под них больше нет. Прозрачность и свечение задаёт материал, и
        // объект, которому они были нужны, получает свой материал — иначе вид
        // снова задавался бы в двух местах.

        if (j.contains("mesh")) {
            mr.Ref.type = MeshTypeFromString(j["mesh"].value("type", "none"));
            mr.Ref.path = LoadAssetRef(j["mesh"], "path");
            mr.Ref.part = j["mesh"].value("part", -1);
            if (j["mesh"].contains("geometry")) mr.Ref.prim = PrimitiveFromJson(j["mesh"]["geometry"]);
        }


        if (j.value("hidden", false)) obj.Registry()->emplace<HiddenComponent>(obj.Entity());
        if (j.value("keepOnLoad", false)) obj.Registry()->emplace<KeepOnLoadComponent>(obj.Entity());
        sage::scene::LoadRegisteredComponents(*obj.Registry(), obj.Entity(), j);
        if (j.contains("script") || j.contains("scripts")) {
            ScriptComponent sc;
            LoadScripts(j, sc);
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
        if (j.contains("ui")) {
            sage::ui::LoadElement(j["ui"], *obj.Registry(), obj.Entity());
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
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type, mr.Ref.prim);
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
    // Интерфейсы из файлов — сразу, а не с первым кадром: скрипт, стартующий
    // вместе со сценой, ищет кнопки меню по имени, и они обязаны уже быть.
    sage::ui::SyncInterfaceSources(*scene);
    scene->Lighting = LightingFromJson(root);
    scene->Reflections = ReflectionsFromJson(root);
    if (root.contains("gi")) scene->GI = GIFromJson(root["gi"]);
    if (root.contains("layers") && root["layers"].is_array())
        for (const json& n : root["layers"])
            if (n.is_string()) scene->Layers.push_back(n.get<std::string>());

    return scene;
}

// --- Публичные обёртки: файл и строка используют одну и ту же сборку JSON ---

void Save(const Scene& scene, const std::string& path) {
    // Сначала ВЕСЬ текст в памяти, и только потом — диск. Раньше файл
    // открывался (и обнулялся) до сборки JSON: исключение в компоненте игры
    // или битая строка (dump бросает на не-UTF-8) оставляли на месте сцены
    // пустой или обрезанный файл. Теперь любая ошибка сборки вылетает до
    // записи, а запись идёт через временный файл с подменой — прежняя сцена
    // остаётся целой при любом исходе.
    const std::string text = BuildSceneJson(scene).dump(2);
    std::string err;
    if (!sage::WriteFileAtomic(path, text, err)) {
        throw std::runtime_error("Не удалось записать сцену: " + err);
    }
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
