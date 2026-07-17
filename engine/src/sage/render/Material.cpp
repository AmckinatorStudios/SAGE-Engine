#include "sage/render/Material.h"

#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Формат .sagemat: цвета — массивы [r,g,b] (как в create-шаблоне редактора).
static json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

static glm::vec3 Vec3FromJson(const json& j, glm::vec3 fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

Material Material::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть материал: " + path);
    }
    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка парсинга материала (" + path + "): " + e.what());
    }

    Material m;
    m.Albedo = Vec3FromJson(root.value("albedo", json()), m.Albedo);
    m.Emissive = Vec3FromJson(root.value("emissive", json()), m.Emissive);
    m.Shininess = root.value("shininess", m.Shininess);
    m.TexturePath = root.value("texture", m.TexturePath);
    return m;
}

void Material::SaveToFile(const std::string& path) const {
    json root;
    root["albedo"] = Vec3ToJson(Albedo);
    root["emissive"] = Vec3ToJson(Emissive);
    root["shininess"] = Shininess;
    root["texture"] = TexturePath;

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть материал для записи: " + path);
    }
    file << root.dump(4) << "\n";
}
