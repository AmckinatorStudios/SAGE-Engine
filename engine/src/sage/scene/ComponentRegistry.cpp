#include "sage/scene/ComponentRegistry.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/VisualScriptComponent.h"
#include "sage/scene/Signals.h"
#include "sage/vars/Refs.h"

namespace sage::scene {

namespace {
void EnsureBuiltins();

std::vector<ComponentType>& Types() {
    static std::vector<ComponentType> t;
    EnsureBuiltins();
    return t;
}

// Встроенные компоненты, описанные реестром, — при первом обращении к нему:
// статический инициализатор в другом файле статической библиотеки
// компоновщик вправе выбросить, и компонент молча перестал бы писаться.
void EnsureBuiltins() {
    static bool done = false;
    if (done) return;
    done = true;
    RegisterAiAgentComponent();
    RegisterVisualScriptComponent();
}
} // namespace

void RegisterComponent(ComponentType type) {
    if (type.Key.empty() || !type.Has || !type.Save || !type.Load) {
        LOG_ERROR("Scene") << "RegisterComponent: у типа «" << type.Key
                           << "» нет ключа или функций Has/Save/Load — не зарегистрирован";
        return;
    }
    // Ключи «components» и встроенные поля объекта — не трогаем: пересечься
    // с ними значит перезаписать чужие данные в файле.
    std::vector<ComponentType>& all = Types();
    auto it = std::find_if(all.begin(), all.end(), [&](const ComponentType& t) { return t.Key == type.Key; });
    if (it != all.end()) *it = std::move(type);
    else all.push_back(std::move(type));
}

bool UnregisterComponent(const std::string& key) {
    std::vector<ComponentType>& all = Types();
    auto it = std::find_if(all.begin(), all.end(), [&](const ComponentType& t) { return t.Key == key; });
    if (it == all.end()) return false;
    all.erase(it);
    return true;
}

const std::vector<ComponentType>& ComponentTypes() { return Types(); }

const ComponentType* FindComponentType(const std::string& key) {
    for (const ComponentType& t : Types())
        if (t.Key == key) return &t;
    return nullptr;
}

void VisitEntityRefs(entt::registry& reg, entt::entity e, const RefVisit& visit) {
    if (!visit || !reg.valid(e)) return;
    // Публичные переменные — забота vars (он ниже сцены и про сцену не знает).
    sage::vars::VisitEntityRefs(reg, e, visit);
    // Цели связей сигналов: копия кнопки из префаба обязана звать СВОЮ дверь,
    // а не дверь оригинала.
    if (sage::signals::SignalLinksComponent* sl = reg.try_get<sage::signals::SignalLinksComponent>(e))
        for (sage::signals::Link& link : sl->Links) visit(link.Target);
    for (const ComponentType& t : Types())
        if (t.VisitRefs && t.Has(reg, e)) t.VisitRefs(reg, e, visit);
}

void SaveRegisteredComponents(const entt::registry& reg, entt::entity e, nlohmann::json& object) {
    nlohmann::json comps = nlohmann::json::object();
    // Сначала неизвестные — чтобы зарегистрированный тип с тем же ключом
    // (код игры вернулся) записал своё, свежее.
    if (const UnknownComponents* u = reg.try_get<UnknownComponents>(e))
        for (auto it = u->Data.begin(); it != u->Data.end(); ++it) comps[it.key()] = it.value();
    for (const ComponentType& t : Types()) {
        if (!t.Has(reg, e)) continue;
        try {
            comps[t.Key] = t.Save(reg, e);
        } catch (const std::exception& ex) {
            LOG_ERROR("Scene") << "компонент «" << t.Key << "» не сохранился: " << ex.what();
        }
    }
    if (!comps.empty()) object["components"] = std::move(comps);
}

void LoadRegisteredComponents(entt::registry& reg, entt::entity e, const nlohmann::json& object) {
    auto it = object.find("components");
    if (it == object.end() || !it->is_object()) return;
    nlohmann::json unknown = nlohmann::json::object();
    for (auto c = it->begin(); c != it->end(); ++c) {
        const ComponentType* t = FindComponentType(c.key());
        if (!t) {
            unknown[c.key()] = c.value();
            continue;
        }
        try {
            t->Load(reg, e, c.value());
        } catch (const std::exception& ex) {
            // Не прочитался — сохраняем как есть: следующее сохранение не
            // имеет права стереть то, что этот код не понял.
            LOG_ERROR("Scene") << "компонент «" << c.key() << "» не прочитался: " << ex.what()
                               << " — сохранён как есть";
            unknown[c.key()] = c.value();
        }
    }
    if (!unknown.empty()) reg.emplace_or_replace<UnknownComponents>(e, UnknownComponents{std::move(unknown)});
}

void CopyRegisteredComponents(const entt::registry& src, entt::entity se, entt::registry& dst, entt::entity de) {
    if (const UnknownComponents* u = src.try_get<UnknownComponents>(se)) dst.emplace_or_replace<UnknownComponents>(de, *u);
    for (const ComponentType& t : Types()) {
        if (!t.Has(src, se)) continue;
        if (t.Copy) {
            t.Copy(src, se, dst, de);
        } else {
            // Без своей копии — через файл: медленнее, но ничего не теряется.
            t.Load(dst, de, t.Save(src, se));
        }
    }
}

} // namespace sage::scene
