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

static std::string MeshTypeToString(MeshRef::Type t) {
    switch (t) {
        case MeshRef::Type::Cube:  return "cube";
        case MeshRef::Type::Model: return "model";
        default: return "none";
    }
}

static MeshRef::Type MeshTypeFromString(const std::string& s) {
    if (s == "cube") return MeshRef::Type::Cube;
    if (s == "model") return MeshRef::Type::Model;
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
    return lighting;
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
        j["position"] = Vec3ToJson(tr.Position);
        j["rotation"] = Vec3ToJson(tr.Rotation);
        j["scale"]    = Vec3ToJson(tr.Scale);
        j["color"]    = Vec3ToJson(mr.Color);
        j["mesh"]["type"] = MeshTypeToString(mr.Ref.type);
        j["mesh"]["path"] = mr.Ref.path;
        if (const ScriptComponent* sc = reg.try_get<ScriptComponent>(e)) {
            j["script"] = sc->Path;
        }
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
    for (const auto& j : root.value("objects", json::array())) {
        int id = j.value("id", fallbackId++);
        GameObject obj = scene->CreateObjectWithId(j.value("name", "Object"), id);
        maxId = std::max(maxId, id);

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

        // Пересоздаём GPU-ресурс на основе описания
        switch (mr.Ref.type) {
            case MeshRef::Type::Cube:
                mr.MeshPtr = ResourceManager::Instance().GetCube();
                break;
            case MeshRef::Type::Model:
                mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
                break;
            default:
                mr.MeshPtr = nullptr;
                break;
        }
    }
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
