#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>
#include <cmath>

#include "sage/anim/AnimProperty.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/scripting/ScriptTween.h"
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

glm::vec3& FieldOf(Transform& t, TransformVector::Field f) {
    switch (f) {
        case TransformVector::Field::Rotation: return t.Rotation;
        case TransformVector::Field::Scale: return t.Scale;
        default: return t.Position;
    }
}

// Ось вектора: читается из снимка, пишется И в снимок, И в объект — только
// эта ось, чтобы `p.y = 1` не вернул объекту старые x и z из снимка.
auto Axis(int i) {
    return sol::property(
        [i](TransformVector& v) { return v[i]; },
        [i](TransformVector& v, float value) {
            v[i] = value;
            if (!v.Obj.Valid()) return;   // объект уничтожен — остаётся просто числом
            if (Transform* t = v.Obj.Registry()->try_get<Transform>(v.Obj.Entity()))
                FieldOf(*t, v.Which)[i] = value;
        });
}

} // namespace

std::string SnakeToPascal(const std::string& name) {
    if (name.empty() || !std::islower((unsigned char)name[0])) return {};
    std::string out;
    bool upper = true;
    for (char c : name) {
        if (c == '_') { upper = true; continue; }
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out == name ? std::string() : out;
}

sol::object SnakeMethod(sol::state_view lua, const char* type, const sol::stack_object& key) {
    if (!key.is<std::string>()) return sol::lua_nil;
    const std::string pascal = SnakeToPascal(key.as<std::string>());
    if (pascal.empty()) return sol::lua_nil;
    sol::object cls = lua[type];
    if (cls.get_type() != sol::type::table) return sol::lua_nil;
    sol::object fn = cls.as<sol::table>()[pascal];
    return fn.get_type() == sol::type::function ? fn : sol::object(sol::lua_nil);
}

void RegisterObject(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // --- Transform -----------------------------------------------------------
    using TV = TransformVector;
    lua.new_usertype<TV>("SageTransformVector", sol::no_constructor,
        sol::base_classes, sol::bases<glm::vec3>(),
        "x", Axis(0), "y", Axis(1), "z", Axis(2),
        sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
        sol::meta_function::multiplication,
        sol::overload([](const glm::vec3& a, float k) { return a * k; },
                      [](float k, const glm::vec3& a) { return a * k; },
                      [](const glm::vec3& a, const glm::vec3& b) { return a * b; }),
        sol::meta_function::division, [](const glm::vec3& a, float k) { return a / k; },
        sol::meta_function::equal_to, [](const glm::vec3& a, const glm::vec3& b) { return a == b; },
        sol::meta_function::to_string, [](const glm::vec3& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", " + std::to_string(a.z) + ")";
        },
        "copy", [](const glm::vec3& a) { return a; },
        "length", [](const glm::vec3& a) { return glm::length(a); },
        "normalized", [](const glm::vec3& a) {
            const float len = glm::length(a);
            return len > 1e-6f ? a / len : glm::vec3(0.0f);
        },
        "distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); },
        "dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
        "cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); });

    sol::usertype<TransformRef> tr = lua.new_usertype<TransformRef>("SageTransform");
    tr["position"] = sol::property(
        [](TransformRef& r) { return TV(Tr(r.Obj, "transform.position").Position, r.Obj, TV::Field::Position); },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.position").Position = v; });
    tr["rotation"] = sol::property(
        [](TransformRef& r) { return TV(Tr(r.Obj, "transform.rotation").Rotation, r.Obj, TV::Field::Rotation); },
        [](TransformRef& r, const glm::vec3& v) { Tr(r.Obj, "transform.rotation").Rotation = v; });
    tr["scale"] = sol::property(
        [](TransformRef& r) { return TV(Tr(r.Obj, "transform.scale").Scale, r.Obj, TV::Field::Scale); },
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
    // translate, look_at, forward… — нынешние имена тех же методов.
    tr[sol::meta_function::index] = [](TransformRef&, sol::stack_object key) {
        return SnakeMethod(key.lua_state(), "SageTransform", key);
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
            rb.LinearDamping = num("linearDamping", rb.LinearDamping);
            rb.AngularDamping = num("angularDamping", rb.AngularDamping);
            rb.GravityScale = num("gravityScale", rb.GravityScale);
            if (opts) {
                rb.Sensor = opts->get_or("sensor", rb.Sensor);
                rb.Continuous = opts->get_or("continuous", rb.Continuous);
                // «Не опрокидывается» — самое частое, поэтому отдельным флагом.
                if (opts->get_or("freezeRotation", false)) rb.Locks |= sage::physics::kLockRotAll;
                rb.Locks = (uint8_t)(opts->get_or("locks", (int)rb.Locks) & 0x3F);
            }
            return sol::make_object(self->Lua(), true);
        }
        if (name == "Collider") {
            ColliderComponent& c = reg.get_or_emplace<ColliderComponent>(e);
            const std::string shape = str("shape", "box");
            if (shape == "box") c.Shape = sage::physics::ShapeType::Box;
            else if (shape == "sphere") c.Shape = sage::physics::ShapeType::Sphere;
            else if (shape == "capsule") c.Shape = sage::physics::ShapeType::Capsule;
            else if (shape == "convex") c.Shape = sage::physics::ShapeType::ConvexHull;
            else if (shape == "mesh") c.Shape = sage::physics::ShapeType::Mesh;
            else throw std::runtime_error("AddComponent(Collider): форма '" + shape +
                                          "' — нужна box, sphere, capsule, convex или mesh");
            if (opts) {
                c.MeshPath = opts->get_or<std::string>("mesh", c.MeshPath);
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
        // Свет, камера и вид объекта — чтобы собранный кодом объект мог светить,
        // смотреть и быть покрашенным (а твины — менять яркость, угол и цвет).
        if (name == "Light") {
            LightComponent& l = reg.get_or_emplace<LightComponent>(e);
            const std::string type = str("type", "point");
            l.Kind = type == "spot" ? LightComponent::Type::Spot
                   : type == "directional" ? LightComponent::Type::Directional : LightComponent::Type::Point;
            l.Intensity = num("intensity", l.Intensity);
            l.Range = num("range", l.Range);
            if (opts) {
                sol::object c = (*opts)["color"];
                if (c.is<glm::vec3>()) l.Color = c.as<glm::vec3>();
            }
            return sol::make_object(self->Lua(), true);
        }
        if (name == "Camera") {
            CameraComponent& c = reg.get_or_emplace<CameraComponent>(e);
            c.Fov = num("fov", c.Fov);
            // Камера, добавленная кодом, НЕ главная, пока её об этом не
            // попросят: иначе любой вспомогательный объект перехватывал бы кадр.
            c.Primary = opts ? opts->get_or("primary", false) : false;
            return self->ComponentOf(r.Obj, "Camera");
        }
        if (name == "Mesh") {
            MeshRendererComponent& m = reg.get_or_emplace<MeshRendererComponent>(e);
            if (opts) {
                sol::object c = (*opts)["color"];
                if (c.is<glm::vec3>()) m.Color = c.as<glm::vec3>();
            }
            return sol::make_object(self->Lua(), true);
        }
        throw std::runtime_error("AddComponent: компонент '" + name +
                                 "' из скрипта не добавляется (есть RigidBody, Collider, CharacterController, "
                                 "Script, Light, Camera, Mesh)");
    };
    // Любое свойство по имени — тем же реестром, что твины и анимация:
    // obj:get("opacity"), obj:set("color", Color(1, 0, 0)). Имена те же, что у
    // Tween.to ("position", "scale", "fov", "intensity", "fill.color"…).
    auto propertyOf = [](ObjectRef& r, const std::string& name, const char* who) {
        Alive(r.Obj, who);
        const std::string id = sage::scripting::tween::ResolveProperty(*r.Obj.Registry(), r.Obj.Entity(), name);
        const sage::anim::PropertyType* p = id.empty() ? nullptr : sage::anim::FindProperty(id);
        if (!p) throw std::runtime_error(std::string(who) + ": у объекта '" + r.Obj.Name() + "' нет свойства '" + name + "'");
        return p;
    };
    ot["get"] = [self, propertyOf](ObjectRef& r, const std::string& name) -> sol::object {
        const sage::anim::PropertyType* p = propertyOf(r, name, "get");
        glm::vec4 v(0.0f);
        sage::anim::ReadProperty(*p, *r.Obj.Registry(), r.Obj.Entity(), v);
        sol::state& L = self->Lua();
        switch (p->Components) {
            case 1: return sol::make_object(L, v.x);
            case 2: return sol::make_object(L, glm::vec2(v));
            case 3: return sol::make_object(L, glm::vec3(v));
            default: return sol::make_object(L, v);
        }
    };
    ot["set"] = [propertyOf](ObjectRef& r, const std::string& name, sol::object value) {
        const sage::anim::PropertyType* p = propertyOf(r, name, "set");
        glm::vec4 v(0.0f);
        if (!sage::scripting::tween::ValueFrom(value, p->Components, v))
            throw std::runtime_error("set: значение для '" + name + "' — число, Vector3 или Color");
        sage::anim::WriteProperty(*p, *r.Obj.Registry(), r.Obj.Entity(), v);
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
        if (name == "Mesh") return reg.all_of<MeshRendererComponent>(e);
        return false;
    };
    ot["RemoveComponent"] = [](ObjectRef& r, const std::string& name) {
        Alive(r.Obj, "RemoveComponent");
        entt::registry& reg = *r.Obj.Registry();
        const entt::entity e = r.Obj.Entity();
        if (name == "RigidBody" || name == "Body") return reg.remove<RigidBodyComponent>(e) > 0;
        if (name == "Collider") return reg.remove<ColliderComponent>(e) > 0;
        if (name == "CharacterController") return reg.remove<CharacterControllerComponent>(e) > 0;
        if (name == "Light") return reg.remove<LightComponent>(e) > 0;
        if (name == "Camera") return reg.remove<CameraComponent>(e) > 0;
        if (name == "Mesh") return reg.remove<MeshRendererComponent>(e) > 0;
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
