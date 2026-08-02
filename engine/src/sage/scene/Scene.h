#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include "sage/scene/Transform.h"
#include "sage/scene/Light.h"
#include "sage/scene/Components.h"
#include "sage/render/Reflection.h"

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
namespace sage::gi { struct GIState; } // запечённое GI сцены (см. sage/gi/GI.h)

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
    // Свечение — рядом с цветом и по той же причине: и то и другое живёт в
    // рендерере, но со стороны скрипта это свойство САМОГО объекта («фонарь
    // светится»), и заставлять писать obj:GetRenderer().Emissive там, где
    // соседняя строка — obj.Color, значило бы разложить одно и то же понятие
    // на два разных способа обращения.
    glm::vec3& EmissiveRef() { return Comp<MeshRendererComponent>().Emissive; }
    float& EmissiveStrengthRef() { return Comp<MeshRendererComponent>().EmissiveStrength; }

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
    // Занятый id (битый/правленный руками файл сцены с дубликатами) молча
    // перезаписал бы карту id->entity — прежняя сущность стала бы недостижимой
    // (утечка в registry, RemoveObject её больше не найдёт). Вместо этого
    // выдаём ближайший свободный id; фактический id — в возвращённом объекте.
    GameObject CreateObjectWithId(const std::string& name, int id) {
        while (m_idToEntity.find(id) != m_idToEntity.end()) id = m_nextId++;
        entt::entity e = m_registry.create();
        m_registry.emplace<IdComponent>(e, IdComponent{id});
        m_registry.emplace<NameComponent>(e, NameComponent{name});
        m_registry.emplace<Transform>(e);
        m_registry.emplace<MeshRendererComponent>(e);
        m_idToEntity[id] = e;
        return GameObject(&m_registry, e);
    }

    // Удаляет сущность ВМЕСТЕ с её поддеревом (детьми, внуками, …) и отвязывает
    // её от родителя. Так удаление узла иерархии в редакторе убирает всю ветку.
    void RemoveObject(int id) {
        auto it = m_idToEntity.find(id);
        if (it == m_idToEntity.end()) return;
        entt::entity e = it->second;
        DetachFromParent(e);
        DestroySubtree(e);
    }

    // --- Иерархия (родитель/дети) ------------------------------------------
    // Делает parent родителем child (parent == entt::null — открепить в корень).
    // Двусторонняя связь; циклы предотвращаются (нельзя сделать родителем своего
    // потомка). Мировая матрица ребёнка = WorldMatrix(parent) * его локальная.
    void SetParent(entt::entity child, entt::entity parent) {
        if (child == entt::null || !m_registry.valid(child) || child == parent) return;
        if (parent != entt::null && (!m_registry.valid(parent) || IsDescendant(parent, child))) return;

        auto& hc = m_registry.get_or_emplace<HierarchyComponent>(child);
        if (hc.Parent != entt::null && m_registry.valid(hc.Parent)) {
            if (auto* oldp = m_registry.try_get<HierarchyComponent>(hc.Parent)) {
                auto& v = oldp->Children;
                v.erase(std::remove(v.begin(), v.end(), child), v.end());
            }
        }
        hc.Parent = parent;
        if (parent != entt::null) {
            auto& ph = m_registry.get_or_emplace<HierarchyComponent>(parent);
            if (std::find(ph.Children.begin(), ph.Children.end(), child) == ph.Children.end())
                ph.Children.push_back(child);
        }
    }
    void SetParentById(int childId, int parentId) {
        entt::entity c = Resolve(childId), p = Resolve(parentId);
        SetParent(c, p);
    }
    entt::entity ParentOf(entt::entity e) const {
        const auto* h = m_registry.try_get<HierarchyComponent>(e);
        return (h && m_registry.valid(h->Parent)) ? h->Parent : entt::null;
    }

    // Мировая матрица: композиция локальных матриц по цепочке родителей.
    glm::mat4 WorldMatrix(entt::entity e) const {
        if (e == entt::null || !m_registry.valid(e)) return glm::mat4(1.0f);
        const Transform* tr = m_registry.try_get<Transform>(e);
        glm::mat4 local = tr ? tr->GetMatrix() : glm::mat4(1.0f);
        const auto* h = m_registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null && m_registry.valid(h->Parent))
            return WorldMatrix(h->Parent) * local;
        return local;
    }
    glm::mat4 WorldMatrixById(int id) const {
        auto it = m_idToEntity.find(id);
        return it == m_idToEntity.end() ? glm::mat4(1.0f) : WorldMatrix(it->second);
    }

    // Мировые матрицы ВСЕХ сущностей с Transform одним мемоизированным проходом:
    // O(n) на кадр вместо O(n·глубина) при повызовном WorldMatrix на каждую
    // сущность (рендер зовёт матрицу для каждой видимой сущности в КАЖДОМ
    // проходе — тени/вьюпорт/игра; общие родительские цепочки пересчитывались
    // многократно). out переиспользуется вызывающим между кадрами — ёмкость
    // хэш-таблицы не переаллоцируется.
    void ComputeWorldMatrices(std::unordered_map<entt::entity, glm::mat4>& out) const {
        out.clear();
        auto view = m_registry.view<Transform>();
        for (auto e : view) WorldMatrixMemo(e, out);
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

    // Отражения — тоже свойство сцены, а не рендера: в открытом море отражается
    // небо, в помещении — снятый зондом куб, а высота воды принадлежит игре.
    sage::render::ReflectionSettings Reflections;

    // Запечённое глобальное освещение (лайтмапы + GI-объём проб, см. sage/gi).
    // nullptr — GI не запекалось. shared_ptr намеренно: снапшоты сцены
    // редактора (undo/Play) переносят бейк без повторного запекания
    // (sage::gi::Transplant), а рендер безопасно держит ссылку на кадр.
    std::shared_ptr<sage::gi::GIState> GI;

private:
    entt::entity Resolve(int id) const {
        auto it = m_idToEntity.find(id);
        return it == m_idToEntity.end() ? entt::null : it->second;
    }

    // Рекурсия WorldMatrix с мемо-таблицей: каждая сущность (и каждый общий
    // родитель) считается ровно один раз за проход ComputeWorldMatrices.
    glm::mat4 WorldMatrixMemo(entt::entity e, std::unordered_map<entt::entity, glm::mat4>& memo) const {
        auto it = memo.find(e);
        if (it != memo.end()) return it->second;
        const Transform* tr = m_registry.try_get<Transform>(e);
        glm::mat4 world = tr ? tr->GetMatrix() : glm::mat4(1.0f);
        const auto* h = m_registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null && m_registry.valid(h->Parent))
            world = WorldMatrixMemo(h->Parent, memo) * world;
        memo.emplace(e, world);
        return world;
    }
    // Является ли node потомком ancestor (поднимаясь по родителям node, встретим ancestor).
    bool IsDescendant(entt::entity node, entt::entity ancestor) const {
        while (node != entt::null && m_registry.valid(node)) {
            const auto* h = m_registry.try_get<HierarchyComponent>(node);
            if (!h) return false;
            if (h->Parent == ancestor) return true;
            node = h->Parent;
        }
        return false;
    }
    void DetachFromParent(entt::entity e) {
        auto* h = m_registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null && m_registry.valid(h->Parent)) {
            if (auto* p = m_registry.try_get<HierarchyComponent>(h->Parent)) {
                auto& v = p->Children;
                v.erase(std::remove(v.begin(), v.end(), e), v.end());
            }
        }
    }
    // Рекурсивно уничтожает сущность и всё её поддерево (с чисткой карты id).
    void DestroySubtree(entt::entity e) {
        if (!m_registry.valid(e)) return;
        std::vector<entt::entity> kids;
        if (auto* h = m_registry.try_get<HierarchyComponent>(e)) kids = h->Children;
        for (auto k : kids) DestroySubtree(k);
        if (auto* idc = m_registry.try_get<IdComponent>(e)) m_idToEntity.erase(idc->Id);
        if (m_registry.valid(e)) m_registry.destroy(e);
    }

    std::string m_name;
    entt::registry m_registry;
    std::unordered_map<int, entt::entity> m_idToEntity;
    int m_nextId = 1;
};
