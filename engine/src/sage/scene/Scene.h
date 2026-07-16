#pragma once
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <entt/entt.hpp>
#include "sage/scene/Transform.h"
#include "sage/scene/Light.h"
#include "sage/scene/Components.h"

// ---------------------------------------------------------------------------
// Scene — сцена на базе ECS (entt). Сущности (entity) — это просто id; их
// поведение/данные задаются НАВЕШАННЫМИ компонентами (см. Components.h), а
// логика — системами (см. sage/ecs/*). Это и есть «настоящая» ECS: движок не
// зашивает толстый объект с фиксированным набором полей — состав сущности
// собирается из компонентов, поэтому игры расширяют мир своими компонентами,
// не трогая ядро.
//
// GameObject ниже — тонкий ДЕСКРИПТОР (handle) поверх entt: пара {registry,
// entity} с удобными аксессорами к компонентам. Он специально копируемый и
// дешёвый — им можно свободно владеть из C++ и из Lua, он не «владеет» самой
// сущностью. Прежний код (скрипты, сериализатор) работает с тем же
// Id/Name/Transform/Color, что и раньше, — сменилась только реализация под ним.
// ---------------------------------------------------------------------------
class GameObject {
public:
    GameObject() = default;
    GameObject(entt::registry* reg, entt::entity e) : m_reg(reg), m_entity(e) {}

    bool Valid() const { return m_reg && m_reg->valid(m_entity); }
    entt::entity Entity() const { return m_entity; }
    entt::registry* Registry() const { return m_reg; }

    int Id() const { return Comp<IdComponent>().Id; }
    const std::string& Name() const { return Comp<NameComponent>().Name; }
    void SetName(const std::string& name) { Comp<NameComponent>().Name = name; }

    Transform& GetTransform() { return Comp<Transform>(); }
    const Transform& GetTransform() const { return Comp<Transform>(); }

    MeshRendererComponent& Renderer() { return Comp<MeshRendererComponent>(); }
    const MeshRendererComponent& Renderer() const { return Comp<MeshRendererComponent>(); }
    glm::vec3& ColorRef() { return Comp<MeshRendererComponent>().Color; }

private:
    template <typename T>
    T& Comp() const {
        if (!Valid()) throw std::runtime_error("GameObject: обращение к уничтоженной/невалидной сущности");
        return m_reg->get<T>(m_entity);
    }

    entt::registry* m_reg = nullptr;
    entt::entity m_entity = entt::null;
};

class Scene {
public:
    explicit Scene(std::string name = "Untitled") : m_name(std::move(name)) {}

    // Дескрипторы GameObject хранят СЫРОЙ указатель на m_registry — поэтому
    // саму сцену нельзя перемещать/копировать (адрес registry должен быть
    // стабилен). Сцены живут либо как поле по значению (GameState.SceneData),
    // либо в unique_ptr (SceneManager) — и то и другое даёт стабильный адрес.
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    const std::string& Name() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    entt::registry& Registry() { return m_registry; }
    const entt::registry& Registry() const { return m_registry; }

    // Создаёт сущность с базовым набором компонентов (Id/Name/Transform/
    // MeshRenderer) и авто-присвоенным последовательным id.
    GameObject CreateObject(const std::string& name) {
        return CreateObjectWithId(name, m_nextId++);
    }

    // То же, но с явным id — используется сериализатором при загрузке сцены,
    // чтобы сохранить id из файла. Обычный код зовёт CreateObject.
    GameObject CreateObjectWithId(const std::string& name, int id) {
        entt::entity e = m_registry.create();
        m_registry.emplace<IdComponent>(e, IdComponent{id});
        m_registry.emplace<NameComponent>(e, NameComponent{name});
        m_registry.emplace<Transform>(e);
        m_registry.emplace<MeshRendererComponent>(e);
        m_idToEntity[id] = e;
        return GameObject(&m_registry, e);
    }

    void RemoveObject(int id) {
        auto it = m_idToEntity.find(id);
        if (it == m_idToEntity.end()) return;
        if (m_registry.valid(it->second)) m_registry.destroy(it->second);
        m_idToEntity.erase(it);
    }

    GameObject FindByName(const std::string& name) {
        auto view = m_registry.view<NameComponent>();
        for (auto e : view) {
            if (view.get<NameComponent>(e).Name == name) return GameObject(&m_registry, e);
        }
        return GameObject(&m_registry, entt::null);
    }

    GameObject Get(int id) {
        auto it = m_idToEntity.find(id);
        if (it == m_idToEntity.end()) return GameObject(&m_registry, entt::null);
        return GameObject(&m_registry, it->second);
    }

    size_t Count() const { return m_idToEntity.size(); }

    int NextId() const { return m_nextId; }
    void SetNextId(int id) { m_nextId = id; }

    // Освещение — часть сцены наравне с сущностями (сохраняется/загружается
    // вместе с ней). Пока это единый environment, а не per-entity компонент —
    // так проще и совпадает с прежним поведением.
    LightingEnvironment Lighting;

private:
    std::string m_name;
    entt::registry m_registry;
    std::unordered_map<int, entt::entity> m_idToEntity;
    int m_nextId = 1;
};
