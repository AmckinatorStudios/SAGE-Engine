#pragma once
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include "Transform.h"
#include "Light.h"
#include "sage/render/Mesh.h"

// Описание того, ИЗ ЧЕГО сделан меш объекта — то, что реально
// сохраняется в файл сцены. Сам Mesh (данные на GPU) не сериализуется,
// он пересоздаётся при загрузке через ResourceManager.
struct MeshRef {
    enum class Type { None, Cube, Model };
    Type type = Type::None;
    std::string path; // используется только при Type::Model

    bool operator==(const MeshRef& other) const {
        return type == other.type && path == other.path;
    }
};

struct GameObject {
    int Id = 0;
    std::string Name;
    ::Transform TransformComponent;
    ::MeshRef MeshRefComponent;
    glm::vec3 Color{1.0f, 1.0f, 1.0f};

    // Runtime-указатель на GPU-меш. Не сериализуется — заполняется
    // ResourceManager'ом при загрузке сцены на основе MeshRefComponent.
    std::shared_ptr<Mesh> MeshComponent;
};

class Scene {
public:
    explicit Scene(std::string name = "Untitled") : m_name(std::move(name)) {}

    const std::string& Name() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    GameObject& CreateObject(const std::string& name) {
        auto obj = std::make_unique<GameObject>();
        obj->Id = m_nextId++;
        obj->Name = name;
        m_objects.push_back(std::move(obj));
        return *m_objects.back();
    }

    void RemoveObject(int id) {
        m_objects.erase(
            std::remove_if(m_objects.begin(), m_objects.end(),
                [id](const std::unique_ptr<GameObject>& o) { return o->Id == id; }),
            m_objects.end());
    }

    GameObject* FindByName(const std::string& name) {
        for (auto& o : m_objects)
            if (o->Name == name) return o.get();
        return nullptr;
    }

    std::vector<std::unique_ptr<GameObject>>& Objects() { return m_objects; }
    int NextId() const { return m_nextId; }
    void SetNextId(int id) { m_nextId = id; }

    // Освещение — часть сцены наравне с объектами: сохраняется и
    // загружается вместе с ней через SceneSerializer.
    LightingEnvironment Lighting;

private:
    std::string m_name;
    std::vector<std::unique_ptr<GameObject>> m_objects;
    int m_nextId = 1;
};
