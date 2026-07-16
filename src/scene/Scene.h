#pragma once
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <any>
#include "Transform.h"
#include "Light.h"
#include "../render/Mesh.h"

// Описание того, ИЗ ЧЕГО сделан меш объекта — то, что реально
// сохраняется в файл сцены. Сам Mesh (данные на GPU) не сериализуется,
// он пересоздаётся при загрузке через AssetManager.
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
    // AssetManager'ом при загрузке сцены на основе MeshRefComponent.
    std::shared_ptr<Mesh> MeshComponent;

    // Свободный "тег" объекта (например "terrain"/"water") — игра решает,
    // что он значит; движок использует его только как ключ фильтрации в
    // рендер-проходах (см. render/ScenePass.h), сам не придаёт ему смысла.
    // Не сериализуется по умолчанию (как и LuaData ниже) — если игре нужно
    // сохранять его в .sage-сцену, это делается через собственный код.
    std::string Tag;

    // Свободное хранилище данных игры на этом объекте — Lua-таблица,
    // лениво создаваемая при первом обращении к entity.Lua из скрипта (см.
    // ScriptEngine::RegisterEngineApi, свойство "Lua" на GameObject). Хранится
    // как std::any (а не sol::table напрямую), чтобы Scene.h — часть ЯДРА,
    // используемая многими файлами движка, — не тянул зависимость на sol2/Lua;
    // фактическое создание/чтение sol::table происходит только в
    // ScriptEngine.cpp, который и так уже включает sol2. Это ПРАВИЛЬНЫЙ
    // инструмент расширяемости для Lua-первого дизайна движка: игра хранит
    // любые свои данные объекта (obj.Lua.chunkCoord, obj.Lua.driftPhase) без
    // правки заголовков движка — типизированный C++ component-registry тут
    // не нужен, т.к. Lua всё равно не параметризует C++-шаблоны.
    std::any LuaData;
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

    // Поиск по стабильному Id (в отличие от имени, он уникален). Нужен, в
    // частности, отложенным колбэкам (асинхронная загрузка меша): к моменту
    // готовности объект мог быть уже удалён — тогда вернётся nullptr.
    GameObject* FindById(int id) {
        for (auto& o : m_objects)
            if (o->Id == id) return o.get();
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
