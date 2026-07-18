#pragma once
#include "Mesh.h"
#include "Material.h"
#include "ModelLoader.h"
#include "Texture.h"
#include "sage/core/Log.h"
#include "sage/scene/Transform.h" // подключаем заранее не обязательно, но пусть будет явный порядок
#include <unordered_map>
#include <memory>
#include <string>

// Простой кэш ресурсов. На вход — описание (тип + путь), на выход — готовый
// Mesh/Material. Один и тот же файл, запрошенный дважды, не грузится повторно.
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
    std::shared_ptr<Mesh> GetSphere() {
        if (!m_sphere) m_sphere = std::make_shared<Mesh>(Mesh::CreateSphere());
        return m_sphere;
    }
    std::shared_ptr<Mesh> GetPlane() {
        if (!m_plane) m_plane = std::make_shared<Mesh>(Mesh::CreatePlane());
        return m_plane;
    }
    std::shared_ptr<Mesh> GetCylinder() {
        if (!m_cylinder) m_cylinder = std::make_shared<Mesh>(Mesh::CreateCylinder());
        return m_cylinder;
    }
    std::shared_ptr<Mesh> GetCone() {
        if (!m_cone) m_cone = std::make_shared<Mesh>(Mesh::CreateCone());
        return m_cone;
    }

    // Готовый GPU-меш по описанию примитива — единая точка для сцен/сериализатора
    // (nullptr для None/Model: None не рисуется, Model грузится через GetModel).
    std::shared_ptr<Mesh> GetPrimitive(MeshRef::Type type) {
        switch (type) {
            case MeshRef::Type::Cube:     return GetCube();
            case MeshRef::Type::Sphere:   return GetSphere();
            case MeshRef::Type::Plane:    return GetPlane();
            case MeshRef::Type::Cylinder: return GetCylinder();
            case MeshRef::Type::Cone:     return GetCone();
            default:                      return nullptr;
        }
    }

    std::shared_ptr<Mesh> GetModel(const std::string& path) {
        auto it = m_models.find(path);
        if (it != m_models.end()) return it->second;
        auto mesh = ModelLoader::LoadObj(path);
        m_models[path] = mesh;
        return mesh;
    }

    // Материал КЭШИРУЕТСЯ И РАЗДЕЛЯЕТСЯ: все сущности с одним путём держат
    // один экземпляр — редактор правит его поля напрямую, объекты обновляются
    // мгновенно. Ошибка чтения не валит вызывающего: логируется, отдаётся
    // материал по умолчанию (белый) — сцена продолжает грузиться.
    // Текстура по пути (кэшируется). nullptr при ошибке — вызывающий рисует без неё.
    std::shared_ptr<Texture> GetTexture(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = m_textures.find(path);
        if (it != m_textures.end()) return it->second;
        std::shared_ptr<Texture> tex;
        try {
            tex = std::make_shared<Texture>(path);
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Текстура не загрузилась (" << path << "): " << e.what();
            tex = nullptr;
        }
        m_textures[path] = tex;
        return tex;
    }

    std::shared_ptr<Material> GetMaterial(const std::string& path) {
        auto it = m_materials.find(path);
        if (it != m_materials.end()) return it->second;
        auto material = std::make_shared<Material>();
        try {
            *material = Material::LoadFromFile(path);
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Материал не загрузился, использую дефолт: " << e.what();
        }
        ResolveMaterialTextures(*material);
        m_materials[path] = material;
        return material;
    }

    // Перечитать материал с диска В ТОТ ЖЕ разделяемый экземпляр (все
    // держатели видят обновление). Если не кэширован — просто загрузит.
    std::shared_ptr<Material> ReloadMaterial(const std::string& path) {
        auto it = m_materials.find(path);
        if (it == m_materials.end()) return GetMaterial(path);
        try {
            *it->second = Material::LoadFromFile(path);
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Перезагрузка материала не удалась: " << e.what();
        }
        ResolveMaterialTextures(*it->second);
        return it->second;
    }

    // Подгружает albedo/normal текстуры материала по его путям (для PBR-пути).
    // Вызывается после загрузки/перезагрузки и когда редактор поменял путь.
    void ResolveMaterialTextures(Material& m) {
        m.AlbedoTex = GetTexture(m.TexturePath);
        m.NormalTex = GetTexture(m.NormalMapPath);
        m.MetallicTex = GetTexture(m.MetallicMapPath);
        m.RoughnessTex = GetTexture(m.RoughnessMapPath);
        m.AOTex = GetTexture(m.AOMapPath);
    }

    void Clear() {
        m_cube.reset();
        m_sphere.reset();
        m_plane.reset();
        m_cylinder.reset();
        m_cone.reset();
        m_models.clear();
        m_materials.clear();
        m_textures.clear();
    }

private:
    ResourceManager() = default;
    std::shared_ptr<Mesh> m_cube, m_sphere, m_plane, m_cylinder, m_cone;
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_models;
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_textures;
};
