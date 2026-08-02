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

// Единственный источник правды о свойствах рендера. Читатель, писатель и
// инспектор ходят СЮДА, а не держат по своему списку: три списка, которые
// обязаны совпадать, — это три места, где они однажды разойдутся.
const std::vector<MaterialRenderField>& MaterialRenderFields() {
    static const std::vector<MaterialRenderField> fields = {
        {"doubleSided", "Двусторонний", MaterialRenderField::Kind::Bool,
         &MaterialRender::DoubleSided, nullptr, 0.0f, 1.0f,
         "Не отсекать задние грани. Стакану и листве — да; замкнутому телу\n"
         "(плитка воды, ящик стекла) — нет: иначе два слоя смешивания на один\n"
         "объект, вода темнеет вдвое, а на швах появляется сетка."},
        {"planarReflectivity", "Плоское отражение", MaterialRenderField::Kind::Float,
         nullptr, &MaterialRender::PlanarReflectivity, 0.0f, 1.0f,
         "Насколько поверхность пользуется плоским отражением сцены.\n"
         "0 — не пользуется, 1 — зеркало. Не выводится из шероховатости:\n"
         "гладкий шар тоже гладкий, но плоскости он не принадлежит."},
    };
    return fields;
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
    m.Metallic = root.value("metallic", m.Metallic);
    m.Roughness = root.value("roughness", m.Roughness);
    m.Opacity = root.value("opacity", m.Opacity);
    // Свойства рендера читаются ПО ТАБЛИЦЕ. Ключи остались в корне файла, а не
    // уехали во вложенный объект: старые .sagemat обязаны читаться как были, а
    // перекладывание ключей ради красоты структуры — ровно та ломающая правка
    // формата, ради защиты от которой заведены версии сцен.
    for (const MaterialRenderField& f : MaterialRenderFields()) {
        if (!root.contains(f.Key)) continue;
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
            m.Render.*f.AsBool = root[f.Key].get<bool>();
        } else if (f.AsFloat) {
            m.Render.*f.AsFloat = root[f.Key].get<float>();
        }
    }
    m.VertexShaderPath = root.value("vertexShader", m.VertexShaderPath);
    m.FragmentShaderPath = root.value("fragmentShader", m.FragmentShaderPath);
    if (root.contains("params") && root["params"].is_object()) {
        for (auto it = root["params"].begin(); it != root["params"].end(); ++it) {
            const auto& v = it.value();
            if (v.is_number()) {
                m.Params[it.key()] = ShaderParam::Make((float)v.get<double>());
            } else if (v.is_array()) {
                float c[4] = {0, 0, 0, 0};
                size_t n = std::min<size_t>(v.size(), 4);
                for (size_t i = 0; i < n; ++i) c[i] = (float)v[i].get<double>();
                if (n <= 1) m.Params[it.key()] = ShaderParam::Make(c[0]);
                else if (n == 2) m.Params[it.key()] = ShaderParam::Make(glm::vec2(c[0], c[1]));
                else if (n == 3) m.Params[it.key()] = ShaderParam::Make(glm::vec3(c[0], c[1], c[2]));
                else m.Params[it.key()] = ShaderParam::Make(glm::vec4(c[0], c[1], c[2], c[3]));
            }
        }
    }
    m.TexturePath = root.value("texture", m.TexturePath);
    m.NormalMapPath = root.value("normalMap", m.NormalMapPath);
    m.MetallicMapPath = root.value("metallicMap", m.MetallicMapPath);
    m.RoughnessMapPath = root.value("roughnessMap", m.RoughnessMapPath);
    m.AOMapPath = root.value("aoMap", m.AOMapPath);
    return m;
}

void Material::SaveToFile(const std::string& path) const {
    json root;
    root["albedo"] = Vec3ToJson(Albedo);
    root["emissive"] = Vec3ToJson(Emissive);
    root["shininess"] = Shininess;
    root["metallic"] = Metallic;
    root["roughness"] = Roughness;
    root["opacity"] = Opacity;
    for (const MaterialRenderField& f : MaterialRenderFields()) {
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) root[f.Key] = Render.*f.AsBool;
        else if (f.AsFloat) root[f.Key] = Render.*f.AsFloat;
    }
    root["vertexShader"] = VertexShaderPath;
    root["fragmentShader"] = FragmentShaderPath;
    for (const auto& [name, prm] : Params) {
        switch (prm.Kind) {
            case ShaderParam::Type::Float: root["params"][name] = prm.Value.x; break;
            case ShaderParam::Type::Vec2:
                root["params"][name] = {prm.Value.x, prm.Value.y}; break;
            case ShaderParam::Type::Vec3:
                root["params"][name] = {prm.Value.x, prm.Value.y, prm.Value.z}; break;
            case ShaderParam::Type::Vec4:
                root["params"][name] = {prm.Value.x, prm.Value.y, prm.Value.z, prm.Value.w}; break;
        }
    }
    root["texture"] = TexturePath;
    root["normalMap"] = NormalMapPath;
    root["metallicMap"] = MetallicMapPath;
    root["roughnessMap"] = RoughnessMapPath;
    root["aoMap"] = AOMapPath;

    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть материал для записи: " + path);
    }
    file << root.dump(4) << "\n";
}
