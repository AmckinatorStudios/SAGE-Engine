#pragma once
#include "Mesh.h"
#include "ModelLoader.h"
#include "../scene/Transform.h" // подключаем заранее не обязательно, но пусть будет явный порядок
#include <unordered_map>
#include <memory>
#include <string>

// Простой кэш ресурсов. На вход — описание (тип + путь), на выход — готовый Mesh.
// Один и тот же .obj, запрошенный дважды, не грузится с диска повторно.
class ResourceManager {
public:
    static ResourceManager& Instance() {
        static ResourceManager instance;
        return instance;
    }

    std::shared_ptr<Mesh> GetCube() {
        if (!m_cube) m_cube = std::make_shared<Mesh>(Mesh::CreateCube());
        return m_cube;
    }

    std::shared_ptr<Mesh> GetModel(const std::string& path) {
        auto it = m_models.find(path);
        if (it != m_models.end()) return it->second;
        auto mesh = ModelLoader::LoadObj(path);
        m_models[path] = mesh;
        return mesh;
    }

    void Clear() {
        m_cube.reset();
        m_models.clear();
    }

private:
    ResourceManager() = default;
    std::shared_ptr<Mesh> m_cube;
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_models;
};
