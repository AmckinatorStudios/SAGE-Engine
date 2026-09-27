#include "sage/scripting/lua/LuaInternal.h"

#include <cmath>

#include "sage/physics/PhysicsComponents.h"
#include "sage/scene/Components.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// ОБЪЕКТ СЦЕНЫ И ЕГО TRANSFORM ГЛАЗАМИ СКРИПТА.
//
//     self.gameObject.name
//     self.transform:SetPosition(0, 1, 0)
//     self.transform:Forward()
//     self:GetComponent("CharacterController")
//
// Универсальные понятия и ничего игрового: объект, его место в мире, его
// компоненты, его родитель и дети. Что этот объект ЗНАЧИТ — игрок, враг,
// дверь — решает скрипт, и движку это неизвестно.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

void Alive(const GameObject& o, const char* who) {
    // Мёртвый объект — самая частая ошибка живого игрового кода: скрипт
    // запомнил врага, враг умер, скрипт трогает его на следующем кадре. Ноль
    // вместо координаты выглядел бы как настоящая координата, и предмет
    // уезжал бы в центр карты — «иногда телепортируется», ищи потом причину.
    if (!o.Valid()) throw std::runtime_error(std::string(who) + ": объект уже уничтожен");
}

Transform& Tr(const GameObject& o, const char* who) {
    Alive(o, who);
    Transform* t = o.Registry()->try_get<Transform>(o.Entity());
    if (!t) throw std::runtime_error(std::string(who) + ": у объекта нет Transform");
    return *t;
}

// Направления берутся из углов Эйлера ТАК ЖЕ, как их собирает Transform::
// GetMatrix. Вторая независимая формула разошлась бы с матрицей на знаке
// угла, и «вперёд» у скрипта смотрело бы не туда, куда повёрнут меш.
glm::vec3 AxisOf(const Transform& t, int axis) {
    const glm::mat4 m = t.GetMatrix();
    glm::vec3 v(m[axis]);
    const float len = glm::length(v);
    return len > 1e-6f ? v / len : glm::vec3(0.0f);
}

} // namespace

void RegisterObject(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // --- Transform -----------------------------------------------------------
    sol::usertype<TransformRef> tr = lua.new_usertype<TransformRef>("SageTransform");
    tr["position"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.position").Position; },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.position").Position = v; });
    tr["rotation"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.rotation").Rotation; },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.rotation").Rotation = v; });
    tr["scale"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.scale").Scale; },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.scale").Scale = v; });
    // Локальные имена — синонимы того же самого: Transform в SAGE и есть
    // локальный, а мировую позицию считает сцена по цепочке родителей
    // (WorldPosition ниже). Синонимы нужны потому, что в скриптах пишут и
    // `transform.position`, и `transform.localPosition`, имея в виду одно.
    tr["localPosition"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.localPosition").Position; },
        [](TransformRef& r, const glm::vec3& v) {
            Tr(r.Obj, "transform.localPosition").Position = v;
        });
    tr["localRotation"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.localRotation").Rotation; },
        [](TransformRef& r, const glm::vec3& v) {
            Tr(r.Obj, "transform.localRotation").Rotation = v;
        });
    tr["localScale"] = sol::property(
        [](TransformRef& r) { return Tr(r.Obj, "transform.localScale").Scale; },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.localScale").Scale = v; });

    tr["SetPosition"] = [](TransformRef& r, float x, float y, float z) {
        Tr(r.Obj, "SetPosition").Position = glm::vec3(x, y, z);
    };
    tr["SetRotation"] = [](TransformRef& r, float x, float y, float z) {
        Tr(r.Obj, "SetRotation").Rotation = glm::vec3(x, y, z);
    };
    tr["SetLocalRotation"] = [](TransformRef& r, float x, float y, float z) {
        Tr(r.Obj, "SetLocalRotation").Rotation = glm::vec3(x, y, z);
    };
    tr["SetScale"] = [](TransformRef& r, float x, float y, float z) {
        Tr(r.Obj, "SetScale").Scale = glm::vec3(x, y, z);
    };
    tr["Translate"] = [](TransformRef& r, const glm::vec3& delta) {
        Tr(r.Obj, "Translate").Position += delta;
    };
    tr["Rotate"] = [](TransformRef& r, float x, float y, float z) {
        Tr(r.Obj, "Rotate").Rotation += glm::vec3(x, y, z);
    };
    tr["Forward"] = [](TransformRef& r) { return AxisOf(Tr(r.Obj, "Forward"), 2); };
    tr["Right"] = [](TransformRef& r) { return AxisOf(Tr(r.Obj, "Right"), 0); };
    tr["Up"] = [](TransformRef& r) { return AxisOf(Tr(r.Obj, "Up"), 1); };
    tr["LookAt"] = [](TransformRef& r, const glm::vec3& target) {
        Transform& t = Tr(r.Obj, "LookAt");
        const glm::vec3 d = target - t.Position;
        const float len = glm::length(d);
        if (len < 1e-6f) return;
        const glm::vec3 n = d / len;
        t.Rotation.y = glm::degrees(std::atan2(n.x, n.z));
        t.Rotation.x = glm::degrees(std::asin(-n.y));
    };
    tr["WorldPosition"] = [self](TransformRef& r) {
        Alive(r.Obj, "WorldPosition");
        Scene* scene = self->ScenePtr();
        if (!scene) return Tr(r.Obj, "WorldPosition").Position;
        return glm::vec3(scene->WorldMatrix(r.Obj.Entity())[3]);
    };

    // --- Объект --------------------------------------------------------------
    sol::usertype<ObjectRef> ot = lua.new_usertype<ObjectRef>("SageObject");
    ot["name"] = sol::property(
        [](ObjectRef& r) { Alive(r.Obj, "name"); return r.Obj.Name(); },
        [](ObjectRef& r, const std::string& n) { Alive(r.Obj, "name"); r.Obj.SetName(n); });
    ot["id"] = sol::property([](ObjectRef& r) { Alive(r.Obj, "id"); return r.Obj.Id(); });
    ot["transform"] = sol::property([](ObjectRef& r) { return TransformRef(r.Obj); });
    // «Включён» — это ВИДИМОСТЬ И УЧАСТИЕ В КАДРЕ (HiddenComponent), то же
    // самое, что галочка в редакторе. Второго понятия «активности» у объекта
    // быть не должно: их немедленно перепутают.
    ot["active"] = sol::property(
        [self](ObjectRef& r) {
            Alive(r.Obj, "active");
            Scene* scene = self->ScenePtr();
            return scene ? !scene->IsHidden(r.Obj.Entity())
                         : !r.Obj.Registry()->all_of<HiddenComponent>(r.Obj.Entity());
        },
        [](ObjectRef& r, bool on) {
            Alive(r.Obj, "active");
            if (on) r.Obj.Registry()->remove<HiddenComponent>(r.Obj.Entity());
            else r.Obj.Registry()->emplace_or_replace<HiddenComponent>(r.Obj.Entity());
        });
    ot["parent"] = sol::property([self](ObjectRef& r) -> sol::object {
        Alive(r.Obj, "parent");
        Scene* scene = self->ScenePtr();
        if (!scene) return sol::nil;
        const entt::entity p = scene->ParentOf(r.Obj.Entity());
        if (p == entt::null) return sol::nil;
        return self->Wrap(GameObject(&scene->Registry(), p));
    });
    ot["children"] = sol::property([self](ObjectRef& r) {
        Alive(r.Obj, "children");
        sol::table list = self->Lua().create_table();
        const HierarchyComponent* h =
            r.Obj.Registry()->try_get<HierarchyComponent>(r.Obj.Entity());
        if (!h) return list;
        int i = 1;
        for (entt::entity c : h->Children)
            if (r.Obj.Registry()->valid(c))
                list[i++] = self->Wrap(GameObject(r.Obj.Registry(), c));
        return list;
    });
    ot["IsValid"] = [](ObjectRef& r) { return r.Obj.Valid(); };

    ot["SetParent"] = [self](ObjectRef& r, sol::object parent) {
        Alive(r.Obj, "SetParent");
        Scene* scene = SceneOrThrow(*self, "SetParent");
        entt::entity p = entt::null;
        if (parent.is<ObjectRef>()) p = parent.as<ObjectRef>().Obj.Entity();
        scene->SetParent(r.Obj.Entity(), p);
    };
    ot["Destroy"] = [self](ObjectRef& r) {
        // Удалить уже удалённое — не ошибка, а обычное положение двух скриптов,
        // целящихся в одного врага.
        if (!r.Obj.Valid()) return;
        Scene* scene = SceneOrThrow(*self, "Destroy");
        scene->RemoveObject(r.Obj.Id());
        self->DropDeadSubscriptions();
    };

    // Компоненты: универсальный GetComponent и типизированные короткие пути.
    ot["GetComponent"] = [self](ObjectRef& r, const std::string& name) {
        return self->ComponentOf(r.Obj, name);
    };
    ot["GetCharacterController"] = [self](ObjectRef& r) {
        return self->ComponentOf(r.Obj, "CharacterController");
    };
    ot["GetAnimation"] = [self](ObjectRef& r) { return self->ComponentOf(r.Obj, "Animation"); };

    // --- СБОРКА ОБЪЕКТА ИЗ СКРИПТА -------------------------------------------
    //
    // Раньше скрипт нового API мог только ЧИТАТЬ компоненты: ящик, созданный
    // Scene.Create, так и оставался без тела и коллайдера — падающий предмет,
    // зону или персонажа собрать кодом было нечем (прежний API это умел, но
    // его объект с новым не совместим). Теперь — теми же словами, что
    // GetComponent, с таблицей настроек:
    //
    //     crate:AddComponent("RigidBody", {type = "dynamic", mass = 2})
    //     crate:AddComponent("Collider",  {shape = "box", size = Vector3(1, 1, 1)})
    //     zone:AddComponent("RigidBody",  {type = "static", sensor = true})
    ot["AddComponent"] = [self](ObjectRef& r, const std::string& name, sol::optional<sol::table> opts)
        -> sol::object {
        Alive(r.Obj, "AddComponent");
        entt::registry& reg = *r.Obj.Registry();
        const entt::entity e = r.Obj.Entity();
        auto str = [&](const char* key, const std::string& def) {
            return opts ? opts->get_or<std::string>(key, def) : def;
        };
        auto num = [&](const char* key, float def) { return opts ? opts->get_or(key, def) : def; };
        if (name == "RigidBody" || name == "Body") {
            RigidBodyComponent& rb = reg.get_or_emplace<RigidBodyComponent>(e);
            const std::string type = str("type", "dynamic");
            if (type == "static") rb.Type = sage::physics::BodyType::Static;
            else if (type == "kinematic") rb.Type = sage::physics::BodyType::Kinematic;
            else if (type == "dynamic") rb.Type = sage::physics::BodyType::Dynamic;
            else throw std::runtime_error("AddComponent(RigidBody): тип '" + type +
                                          "' — нужен static, dynamic или kinematic");
            rb.Mass = num("mass", rb.Mass);
            rb.Friction = num("friction", rb.Friction);
            rb.Restitution = num("restitution", rb.Restitution);
            if (opts) rb.Sensor = opts->get_or("sensor", rb.Sensor);
            return sol::make_object(self->Lua(), true);
        }
        if (name == "Collider") {
            ColliderComponent& c = reg.get_or_emplace<ColliderComponent>(e);
            const std::string shape = str("shape", "box");
            if (shape == "box") c.Shape = sage::physics::ShapeType::Box;
            else if (shape == "sphere") c.Shape = sage::physics::ShapeType::Sphere;
            else if (shape == "capsule") c.Shape = sage::physics::ShapeType::Capsule;
            else throw std::runtime_error("AddComponent(Collider): форма '" + shape +
                                          "' — нужна box, sphere или capsule");
            if (opts) {
                if (sol::optional<glm::vec3> size = (*opts)["size"]) c.HalfExtents = *size * 0.5f;
                c.Radius = opts->get_or("radius", c.Radius);
                c.HalfHeight = opts->get_or("halfHeight", c.HalfHeight);
            }
            return sol::make_object(self->Lua(), true);
        }
        if (name == "CharacterController") {
            reg.get_or_emplace<CharacterControllerComponent>(e);
            return self->ComponentOf(r.Obj, "CharacterController");
        }
        // Скрипт на объект — и сразу в работу (Start зовётся здесь же):
        // объект, собранный кодом, не должен ждать перезапуска сцены, чтобы
        // ожить. Возвращает таблицу нового скрипта (как GetScript).
        if (name == "Script") {
            const std::string path = str("path", "");
            if (path.empty()) throw std::runtime_error("AddComponent(Script): нужен путь — {path = \"assets/scripts/x.lua\"}");
            ScriptingSystem* system = self->Services().System;
            if (!system) throw std::runtime_error("AddComponent(Script): система скриптинга не привязана");
            reg.emplace_or_replace<ScriptComponent>(e).Path = path;
            if (!system->Attach(r.Obj))
                throw std::runtime_error("AddComponent(Script): скрипт '" + path + "' не загрузился (причина — в консоли)");
            return self->ScriptOf(r.Obj);
        }
        throw std::runtime_error("AddComponent: компонент '" + name +
                                 "' из скрипта не добавляется (есть RigidBody, Collider, CharacterController, Script)");
    };
    ot["HasComponent"] = [](ObjectRef& r, const std::string& name) {
        Alive(r.Obj, "HasComponent");
        entt::registry& reg = *r.Obj.Registry();
        const entt::entity e = r.Obj.Entity();
        if (name == "RigidBody" || name == "Body") return reg.all_of<RigidBodyComponent>(e);
        if (name == "Collider") return reg.all_of<ColliderComponent>(e);
        if (name == "CharacterController") return reg.all_of<CharacterControllerComponent>(e);
        if (name == "Script") return reg.all_of<ScriptComponent>(e);
        if (name == "Light") return reg.all_of<LightComponent>(e);
        if (name == "Camera") return reg.all_of<CameraComponent>(e);
        return false;
    };
    ot["RemoveComponent"] = [](ObjectRef& r, const std::string& name) {
        Alive(r.Obj, "RemoveComponent");
        entt::registry& reg = *r.Obj.Registry();
        const entt::entity e = r.Obj.Entity();
        if (name == "RigidBody" || name == "Body") return reg.remove<RigidBodyComponent>(e) > 0;
        if (name == "Collider") return reg.remove<ColliderComponent>(e) > 0;
        if (name == "CharacterController") return reg.remove<CharacterControllerComponent>(e) > 0;
        throw std::runtime_error("RemoveComponent: компонент '" + name + "' из скрипта не снимается");
    };
    // Тег — метка для поиска (Scene.FindByTag): «enemy», «pickup».
    ot["tag"] = sol::property(
        [](ObjectRef& r) {
            Alive(r.Obj, "tag");
            const TagComponent* t = r.Obj.Registry()->try_get<TagComponent>(r.Obj.Entity());
            return t ? t->Tag : std::string();
        },
        [](ObjectRef& r, const std::string& tag) {
            Alive(r.Obj, "tag");
            r.Obj.Registry()->get_or_emplace<TagComponent>(r.Obj.Entity()).Tag = tag;
        });
    ot["GetAudio"] = [self](ObjectRef& r) { return self->ComponentOf(r.Obj, "Audio"); };
    ot["GetCamera"] = [self](ObjectRef& r) { return self->ComponentOf(r.Obj, "Camera"); };

    // Скрипты разговаривают друг с другом через объект, а не через глобальные
    // переменные: `other:GetScript()` и `enemy:Call("TakeDamage", 20)`.
    ot["GetScript"] = [self](ObjectRef& r) { return self->ScriptOf(r.Obj); };
    ot["Call"] = [self](ObjectRef& r, const std::string& method, sol::variadic_args args) {
        return self->InvokeOn(r.Obj, method, args);
    };

    // Сигналы (`obj.clicked`, obj:on/emit) и интерфейс как часть объекта
    // (text, visible, enabled) — см. LuaApi_Signals.cpp.
    RegisterObjectSignals(backend, ot);
}

Scene* SceneOrThrow(Backend& backend, const char* who) {
    Scene* scene = backend.ScenePtr();
    if (!scene) throw std::runtime_error(std::string(who) + ": сцена не привязана");
    return scene;
}

} // namespace sage::scripting::lua
