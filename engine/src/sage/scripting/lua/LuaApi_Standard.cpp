#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>

#include "sage/core/Log.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/render/ParticleComponents.h"
#include "sage/scene/Components.h"

// ---------------------------------------------------------------------------
// СТАНДАРТ SAGE LUA: единый стиль имён поверх уже существующих привязок.
//
//   * `object` — текущий объект (см. Backend::Create); методы через точку.
//   * Разделы строчными: input, scene, time, debug, physics, audio, tween, ui,
//     events, vector3 … — те же таблицы, что прежние Input/Scene/…; прежние
//     написания остаются.
//   * Компоненты по snake_case-именам: object.get_component("rigid_body") —
//     безопасный прокси (держит объект, а не указатель на компонент).
//   * input.was_pressed / was_released / get_vector — имена действий ввода
//     словами.
//
// Ничего нового в движке это не добавляет: только имена и способ вызова.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

// Прокси компонента, чьи поля уже описаны прежним API (RigidBodyComponent,
// LightComponent…) с именами в PascalCase. Он ничего не копирует: каждое
// чтение и запись находит живой компонент по объекту и обращается к нему
// прежним usertype'ом, а имя поля из стандарта (`linear_damping`) переводится
// в прежнее (`LinearDamping`).
struct ComponentRef {
    GameObject Obj;
    int Kind = 0;
};

struct Kind {
    const char* Name;       // как называет компонент скрипт (PascalCase)
    const char* Usertype;   // имя usertype'а прежнего API
    bool (*Has)(const GameObject&);
    bool (*Push)(lua_State*, const GameObject&);   // кладёт указатель на стек
};

template <class C>
bool HasC(const GameObject& o) {
    return o.Valid() && o.Registry()->all_of<C>(o.Entity());
}
template <class C>
bool PushC(lua_State* L, const GameObject& o) {
    C* c = o.Valid() ? o.Registry()->try_get<C>(o.Entity()) : nullptr;
    if (!c) return false;
    sol::stack::push(L, c);
    return true;
}

const Kind kKinds[] = {
    {"RigidBody", "RigidBodyComponent", &HasC<RigidBodyComponent>, &PushC<RigidBodyComponent>},
    {"Collider", "ColliderComponent", &HasC<ColliderComponent>, &PushC<ColliderComponent>},
    {"Joint", "JointComponent", &HasC<JointComponent>, &PushC<JointComponent>},
    {"Light", "LightComponent", &HasC<LightComponent>, &PushC<LightComponent>},
    {"Mesh", "MeshRendererComponent", &HasC<MeshRendererComponent>, &PushC<MeshRendererComponent>},
    {"ParticleEmitter", "ParticleEmitterComponent", &HasC<ParticleEmitterComponent>,
     &PushC<ParticleEmitterComponent>},
    {"Decal", "DecalComponent", &HasC<DecalComponent>, &PushC<DecalComponent>},
};
constexpr int kKindCount = (int)(sizeof(kKinds) / sizeof(kKinds[0]));

// Кандидаты прежнего имени поля: "linear_damping" -> "LinearDamping",
// "linearDamping". Само имя — первым: оно же может быть прежним.
std::vector<std::string> LegacyNames(const std::string& key) {
    std::vector<std::string> out{key};
    if (key.empty() || !std::islower((unsigned char)key[0])) return out;
    std::string pascal;
    bool up = true;
    for (char c : key) {
        if (c == '_') { up = true; continue; }
        pascal += up ? (char)std::toupper((unsigned char)c) : c;
        up = false;
    }
    if (pascal != key) out.push_back(pascal);
    return out;
}

// Положить на стек поле компонента (или nil) и вернуть, нашлось ли оно.
// На стеке остаётся ровно одно значение.
bool GetField(lua_State* L, const ComponentRef& r, const std::string& key) {
    if (!kKinds[r.Kind].Push(L, r.Obj)) {
        lua_pushnil(L);
        return false;
    }
    for (const std::string& name : LegacyNames(key)) {
        lua_pushlstring(L, name.data(), name.size());
        lua_gettable(L, -2);
        if (!lua_isnil(L, -1)) {
            lua_remove(L, -2);   // указатель на компонент
            return true;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    return false;
}

// Приведение прежнего (PascalCase) имени стандарта: у прокси есть и прежние
// имена полей, так что старый код (`body.Mass`) работает как раньше.

const char* kAliases = R"LUA(
return function(G)
    -- Строчное имя раздела — тот же раздел. Занятое имя не отбирается: если
    -- под ним уже таблица прежнего API, недостающее в неё ДОПИСЫВАЕТСЯ, а
    -- нужное сверх этого берётся из нового раздела по __index.
    local function merge(lower, upper)
        local up = rawget(G, upper)
        if type(up) ~= "table" then return end
        local lo = rawget(G, lower)
        if lo == nil then G[lower] = up return end
        if lo == up or type(lo) ~= "table" then return end
        for k, v in next, up do
            if rawget(lo, k) == nil then lo[k] = v end
        end
        if getmetatable(up) and getmetatable(lo) == nil then
            setmetatable(lo, {__index = function(_, k) return up[k] end})
        end
    end

    -- Ввод: действия словами.
    local input = rawget(G, "Input")
    if type(input) == "table" then
        input.was_pressed = input.was_pressed or input.IsActionPressed
        input.was_released = input.was_released or input.IsActionReleased
        input.get_vector = input.get_vector or input.GetVector2
    end

    for _, pair in ipairs({
        {"input", "Input"}, {"time", "Time"}, {"debug", "Debug"}, {"physics", "Physics"},
        {"audio", "Audio"}, {"tween", "Tween"}, {"ui", "UI"}, {"events", "Events"},
        {"scene", "Scene"}, {"vector3", "Vector3"}, {"vector2", "Vector2"},
        {"quaternion", "Quaternion"}, {"color", "Color"}, {"ease", "Ease"},
    }) do
        merge(pair[1], pair[2])
    end
end
)LUA";

} // namespace

sol::object StandardComponentOf(Backend& backend, GameObject object, const std::string& canonical) {
    for (int i = 0; i < kKindCount; ++i) {
        if (canonical != kKinds[i].Name) continue;
        // Без прежнего usertype'а (бэкенд без прежнего движка) поля читать
        // нечем — честный nil, как у неизвестного компонента.
        sol::state& lua = backend.Lua();
        if (!lua[kKinds[i].Usertype].valid()) return sol::lua_nil;
        if (!kKinds[i].Has(object)) return sol::lua_nil;
        ComponentRef ref;
        ref.Obj = object;
        ref.Kind = i;
        return sol::make_object(lua, ref);
    }
    return sol::lua_nil;
}

bool StandardHasComponent(const GameObject& object, const std::string& canonical) {
    for (int i = 0; i < kKindCount; ++i)
        if (canonical == kKinds[i].Name) return kKinds[i].Has(object);
    return false;
}

void InstallStandard(Backend& backend) {
    sol::state& lua = backend.Lua();

    sol::usertype<ComponentRef> ct = lua.new_usertype<ComponentRef>("SageComponent", sol::no_constructor);
    ct[sol::meta_function::index] = [](ComponentRef& r, sol::stack_object key, sol::this_state ts) -> sol::object {
        if (!key.is<std::string>()) return sol::lua_nil;
        lua_State* L = ts;
        GetField(L, r, key.as<std::string>());
        return sol::stack::pop<sol::object>(L);
    };
    ct[sol::meta_function::new_index] = [](ComponentRef& r, sol::stack_object key, sol::stack_object value,
                                           sol::this_state ts) {
        lua_State* L = ts;
        if (!key.is<std::string>()) throw std::runtime_error("компонент: имя поля — строка");
        if (!kKinds[r.Kind].Push(L, r.Obj))
            throw std::runtime_error(std::string(kKinds[r.Kind].Name) + ": компонента больше нет");
        const std::string name = key.as<std::string>();
        for (const std::string& candidate : LegacyNames(name)) {
            lua_pushlstring(L, candidate.data(), candidate.size());
            lua_gettable(L, -2);
            const bool exists = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (!exists) continue;
            lua_pushlstring(L, candidate.data(), candidate.size());
            lua_pushvalue(L, value.stack_index());
            lua_settable(L, -3);
            lua_pop(L, 1);
            return;
        }
        lua_pop(L, 1);
        throw std::runtime_error(std::string(kKinds[r.Kind].Name) + ": нет поля '" + name + "'");
    };
    ct[sol::meta_function::to_string] = [](const ComponentRef& r) {
        return std::string("Component(") + kKinds[r.Kind].Name + ")";
    };

    sol::protected_function_result made = lua.safe_script(kAliases, sol::script_pass_on_error, "@sage_standard");
    if (made.valid()) {
        sol::protected_function install = made;
        sol::table globals = lua.globals();
        sol::protected_function_result r = install(globals);
        if (!r.valid()) {
            sol::error e = r;
            LOG_ERROR("Lua") << "SAGE Lua: стандартные имена не встали: " << e.what();
        }
    } else {
        sol::error e = made;
        LOG_ERROR("Lua") << "SAGE Lua: стандартные имена не встали: " << e.what();
    }
}

} // namespace sage::scripting::lua
