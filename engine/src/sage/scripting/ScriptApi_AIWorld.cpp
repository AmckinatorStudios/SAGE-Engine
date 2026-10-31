#include "ScriptApiAIDetail.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/core/Log.h"
#include "sage/lang/SageApi.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// УЗЛЫ МИРА ДВИЖКА для SAGE AI Graph.
//
// Это ДАННЫЕ И ДЕЙСТВИЯ, а не поведение: «виден ли объект», «где он», «что
// на луче», «кто рядом». Что персонаж сделает, увидев цель, — рисунок автора.
//
// ДВА ПУТИ, И ОБА БЕЗ НОВОЙ СТРОКИ В РЕДАКТОРЕ НА КАЖДЫЙ КОМПОНЕНТ:
//   • узлы ниже — то, что ИИ спрашивает постоянно (позиция, видимость,
//     луч), сделанное на C++ и быстрое;
//   • узлы «Engine API/…» — по одному на КАЖДУЮ функцию API скриптов
//     (sage.<модуль>.<функция>), собранные из той же таблицы сигнатур, что
//     и подсказка редактора кода (sage/lang/SageApiTable.inl). Новый Bind в
//     движке — новый узел в палитре, сам. А «Get/Set Property» и «Call
//     Script» достают до любого свойства компонента и любой функции скрипта
//     объекта по имени — включая компоненты и скрипты самой игры.
// ---------------------------------------------------------------------------
namespace {

using namespace sage::ai::nodes;
using namespace sage::scripting::aidetail;
using sage::ai::GraphExec;
using sage::ai::NodeResult;
using sage::ai::NodeType;
using sage::ai::PinSpec;
namespace type = sage::ai::type;
using sage::vars::EntityRef;
using sage::vars::Value;

// Свойства по пути «Light.Intensity» / «Transform.Position» — на Lua: у
// прежнего объекта компонент достаётся GetLight(), у нового — полем, и
// разбирать оба вида на C++ значило бы повторить весь API объектов.
const char* kWorldLua = R"LUA(
local N = __sage_ai
local function step(cur, part)
    local ok, v = pcall(function() return cur[part] end)
    if ok and v ~= nil then return v end
    local ok2, getter = pcall(function() return cur["Get" .. part] end)
    if ok2 and type(getter) == "function" then return getter(cur) end
    return nil
end
function N.prop_get(obj, path)
    local cur = obj
    for part in string.gmatch(path, "[^%.]+") do
        if cur == nil then return nil end
        cur = step(cur, part)
    end
    return cur
end
function N.prop_set(obj, path, value)
    local parts = {}
    for part in string.gmatch(path, "[^%.]+") do parts[#parts + 1] = part end
    local cur = obj
    for i = 1, #parts - 1 do
        cur = step(cur, parts[i])
        if cur == nil then error("нет '" .. parts[i] .. "' на пути '" .. path .. "'") end
    end
    cur[parts[#parts]] = value
end
function N.call_script(obj, name, args)
    if obj == nil then error("объекта нет") end
    if type(obj.Call) == "function" then return obj:Call(name, table.unpack(args)) end
    return sage.msg.Call(obj, name, table.unpack(args))
end
function N.lua_call(path, args)
    local cur = _G
    for part in string.gmatch(path, "[^%.]+") do
        if type(cur) ~= "table" then cur = nil break end
        cur = cur[part]
    end
    if type(cur) ~= "function" then error("нет функции '" .. path .. "'") end
    return cur(table.unpack(args))
end
)LUA";

// Тип пина по типу из таблицы сигнатур.
std::string PinTypeOf(const std::string& t) {
    if (t == "number") return type::Float;
    if (t == "string") return type::String;
    if (t == "boolean") return type::Bool;
    if (t == "Vec3") return type::Vec3;
    if (t == "Vec2") return type::Vec2;
    if (t == "Vec4") return type::Color;
    if (t == "Entity") return type::Object;
    return type::Any;
}

} // namespace

void ScriptEngine::RegisterAiWorldNodes() {
    sol::protected_function_result made = m_lua.safe_script(kWorldLua, sol::script_pass_on_error);
    if (!made.valid()) {
        sol::error e = made;
        LOG_ERROR("AI") << "Узлы мира ИИ: " << e.what();
    }
    sage::ai::NodeRegistry& r = AI().Nodes();

    // Объект по ссылке: живой или ошибка с понятной причиной.
    auto object = [this](GraphExec& ex, const char* pin) -> GameObject {
        const int id = ex.In(pin).AsEntity().Id;
        if (!m_scene || id <= 0) return GameObject(nullptr, entt::null);
        return m_scene->Get(id);
    };
    auto worldPos = [this](const GameObject& o) -> glm::vec3 {
        return glm::vec3(m_scene->WorldMatrixById(o.Id())[3]);
    };
    auto forward = [this](const GameObject& o) -> glm::vec3 {
        const glm::mat4 m = m_scene->WorldMatrixById(o.Id());
        const glm::vec3 f = -glm::vec3(m[2]);
        const float l = glm::length(f);
        return l > 1e-6f ? f / l : glm::vec3(0, 0, -1);
    };
    auto pure = [&r](NodeType t) {
        t.Pure = true;
        r.Register(std::move(t));
    };
    const Value noObject(EntityRef{});

    {
        NodeType t = Node("world.self", "Self", "World", "The object this AI belongs to.");
        t.Outputs = {P("object", type::Object)};
        t.Run = [](GraphExec& ex) {
            ex.Out("object", Value(EntityRef{ex.Task().Owner()}));
            return NodeResult::Go("");
        };
        t.Keywords = {"me", "owner", "this"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("world.find", "Find Object", "World", "An object of the scene by its name.");
        t.Inputs = {P("name", type::String, Value(std::string()))};
        t.Outputs = {P("object", type::Object), P("found", type::Bool)};
        t.Run = [this](GraphExec& ex) {
            GameObject o = m_scene ? m_scene->FindByName(ex.In("name").AsString()) : GameObject(nullptr, entt::null);
            ex.Out("object", Value(EntityRef{o.Valid() ? o.Id() : 0}));
            ex.Out("found", Value(o.Valid()));
            return NodeResult::Go("");
        };
        t.Keywords = {"search", "name"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("object.info", "Object Info", "World/Object",
                          "Whether the object exists, its name, world position and forward direction.");
        t.Inputs = {P("object", type::Object, noObject)};
        t.Outputs = {P("valid", type::Bool), P("name", type::String), P("position", type::Vec3),
                     P("forward", type::Vec3)};
        t.Run = [object, worldPos, forward](GraphExec& ex) {
            GameObject o = object(ex, "object");
            const bool ok = o.Valid();
            ex.Out("valid", Value(ok));
            ex.Out("name", Value(ok ? o.Name() : std::string()));
            ex.Out("position", Value(ok ? worldPos(o) : glm::vec3(0.0f)));
            ex.Out("forward", Value(ok ? forward(o) : glm::vec3(0, 0, -1)));
            return NodeResult::Go("");
        };
        t.Keywords = {"position", "location", "where", "forward", "valid"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("object.distance", "Distance To", "World/Object", "Distance and direction between objects.");
        t.Inputs = {P("from", type::Object, noObject), P("to", type::Object, noObject)};
        t.Outputs = {P("distance", type::Float), P("direction", type::Vec3)};
        t.Run = [object, worldPos](GraphExec& ex) {
            GameObject a = object(ex, "from"), b = object(ex, "to");
            if (!a.Valid() || !b.Valid()) {
                ex.Out("distance", Value(0.0f));
                ex.Out("direction", Value(glm::vec3(0.0f)));
                return NodeResult::Go("");
            }
            const glm::vec3 d = worldPos(b) - worldPos(a);
            const float l = glm::length(d);
            ex.Out("distance", Value(l));
            ex.Out("direction", Value(l > 1e-6f ? d / l : glm::vec3(0.0f)));
            return NodeResult::Go("");
        };
        pure(std::move(t));
    }
    {
        NodeType t = Node("object.set_position", "Set Position", "World/Object", "Moves the object to a point.");
        // Действие над миром — в задачах и скриптах; AI Graph решает, а не действует.
        t.Domains = sage::ai::kDomainScript;
        t.Inputs = {X("in"), P("object", type::Object, noObject), P("position", type::Vec3)};
        t.Outputs = {X("then")};
        t.Run = [object](GraphExec& ex) {
            GameObject o = object(ex, "object");
            if (!o.Valid()) ex.Error("объекта нет");
            o.GetTransform().Position = ex.In("position").AsVec3();
            return NodeResult::Go("then");
        };
        t.Keywords = {"move", "teleport"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("object.look_at", "Look At", "World/Object", "Turns the object (around the vertical) to a point.");
        // Действие над миром — в задачах и скриптах; AI Graph решает, а не действует.
        t.Domains = sage::ai::kDomainScript;
        t.Inputs = {X("in"), P("object", type::Object, noObject), P("point", type::Vec3)};
        t.Outputs = {X("then")};
        t.Run = [object, worldPos](GraphExec& ex) {
            GameObject o = object(ex, "object");
            if (!o.Valid()) ex.Error("объекта нет");
            const glm::vec3 d = ex.In("point").AsVec3() - worldPos(o);
            if (d.x * d.x + d.z * d.z > 1e-8f)
                o.GetTransform().Rotation.y = glm::degrees(std::atan2(-d.x, -d.z));
            return NodeResult::Go("then");
        };
        t.Keywords = {"face", "turn", "rotate"};
        r.Register(std::move(t));
    }
    {
        // ПЕРЕМЕЩЕНИЕ — через системы движка, а не мимо них: у объекта с
        // контроллером персонажа шаг делает контроллер (лестницы, стены,
        // склоны — его забота), без контроллера — Transform. Узел ждёт
        // прибытия кадр за кадром: «иди к двери» запускается один раз.
        NodeType t = Node("object.move_to", "Move To", "World/Object",
                          "Moves the object toward a point every frame (its character controller if it has one) and "
                          "goes on when it arrives or the timeout runs out.");
        t.Domains = sage::ai::kDomainScript;
        t.Inputs = {X("in"), P("object", type::Object, noObject), P("target", type::Vec3),
                    P("speed", type::Float, Value(3.0f)), P("arrive_distance", type::Float, Value(0.5f)),
                    P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("arrived"), X("failed"), P("distance", type::Float)};
        t.Run = [this, object, worldPos](GraphExec& ex) {
            sage::ai::NodeState& s = ex.State();
            GameObject o = object(ex, "object");
            if (!o.Valid()) {
                ex.Out("distance", Value(0.0f));
                return NodeResult::Go("failed");
            }
            const double now = ex.GetWorld().Time();
            if (!ex.Resumed()) {
                s.Until = ex.In("timeout").AsFloat() > 0.0f ? now + ex.In("timeout").AsFloat() : 0.0;
                s.NextEval = now;   // время прошлого шага — для dt
            }
            const float dt = (float)(now - s.NextEval);
            s.NextEval = now;
            glm::vec3 d = ex.In("target").AsVec3() - worldPos(o);
            d.y = 0.0f;   // по земле: высоту держит контроллер или сцена
            const float dist = glm::length(d);
            ex.Out("distance", Value(dist));
            if (dist <= ex.In("arrive_distance").AsFloat()) return NodeResult::Go("arrived");
            if (s.Until > 0.0 && now >= s.Until) return NodeResult::Go("failed");
            if (dt > 0.0f) {
                const glm::vec3 v = d / dist * ex.In("speed").AsFloat();
                if (o.Registry()->all_of<CharacterControllerComponent>(o.Entity())) {
                    sol::protected_function move = m_lua["sage"]["physics"]["MoveCharacter"];
                    auto res = move(o, v, dt);
                    if (!res.valid()) {
                        sol::error e = res;
                        ex.Error(e.what());
                    }
                } else {
                    const glm::vec3 step = v * dt;
                    o.GetTransform().Position += glm::length(step) > dist ? d : step;
                }
            }
            ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"walk", "go", "navigate", "path", "chase"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("world.raycast", "Raycast", "World/Physics", "What a ray from a point hits first.");
        t.Inputs = {P("origin", type::Vec3), P("direction", type::Vec3, Value(glm::vec3(0, 0, -1))),
                    P("distance", type::Float, Value(100.0f))};
        t.Outputs = {P("hit", type::Bool), P("object", type::Object), P("point", type::Vec3), P("normal", type::Vec3),
                     P("distance", type::Float)};
        t.Run = [this](GraphExec& ex) {
            if (!m_physics) ex.Error("физика не привязана к скриптам");
            const PhysicsScene::EntityHit h =
                m_physics->Raycast(ex.In("origin").AsVec3(), ex.In("direction").AsVec3(), ex.In("distance").AsFloat());
            const int id = (h.Hit && m_scene) ? GameObject(&m_scene->Registry(), h.Entity).Id() : 0;
            ex.Out("hit", Value(h.Hit));
            ex.Out("object", Value(EntityRef{id}));
            ex.Out("point", Value(h.Point));
            ex.Out("normal", Value(h.Normal));
            ex.Out("distance", Value(h.Distance));
            return NodeResult::Go("");
        };
        t.Keywords = {"ray", "trace", "shoot", "hit"};
        pure(std::move(t));
    }
    // Видимость — луч от глаз наблюдателя к цели: ничего ли не стоит между.
    // Без физики видимость считается свободной: судить не по чему, а «никого
    // не видно никогда» хуже честного «препятствий не проверить».
    auto clearLine = [this](const GameObject& from, const glm::vec3& eye, const GameObject& to,
                            const glm::vec3& target) {
        if (!m_physics) return true;
        const glm::vec3 d = target - eye;
        const float len = glm::length(d);
        if (len < 1e-4f) return true;
        const PhysicsScene::EntityHit h = m_physics->Raycast(eye, d / len, len);
        if (!h.Hit) return true;
        return h.Entity == to.Entity() || h.Entity == from.Entity();
    };
    {
        NodeType t = Node("world.line_of_sight", "Line Of Sight", "World/Perception",
                          "Whether nothing stands between the eyes of one object and another.");
        t.Inputs = {P("from", type::Object, noObject), P("to", type::Object, noObject),
                    P("eye_height", type::Float, Value(1.6f))};
        t.Outputs = {P("visible", type::Bool)};
        t.Run = [object, worldPos, clearLine](GraphExec& ex) {
            GameObject a = object(ex, "from"), b = object(ex, "to");
            if (!a.Valid() || !b.Valid()) {
                ex.Out("visible", Value(false));
                return NodeResult::Go("");
            }
            const glm::vec3 eye = worldPos(a) + glm::vec3(0, ex.In("eye_height").AsFloat(), 0);
            ex.Out("visible", Value(clearLine(a, eye, b, worldPos(b) + glm::vec3(0, 0.5f, 0))));
            return NodeResult::Go("");
        };
        t.Keywords = {"visibility", "see", "sight"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("world.in_view", "Can See", "World/Perception",
                          "Whether the target is within range, inside the field of view and not hidden.");
        t.Inputs = {P("observer", type::Object, noObject), P("target", type::Object, noObject),
                    P("fov", type::Float, Value(90.0f)), P("range", type::Float, Value(20.0f)),
                    P("eye_height", type::Float, Value(1.6f))};
        t.Outputs = {P("visible", type::Bool), P("distance", type::Float), P("angle", type::Float)};
        t.Run = [object, worldPos, forward, clearLine](GraphExec& ex) {
            GameObject a = object(ex, "observer"), b = object(ex, "target");
            bool visible = false;
            float dist = 0.0f, angle = 180.0f;
            if (a.Valid() && b.Valid()) {
                const glm::vec3 eye = worldPos(a) + glm::vec3(0, ex.In("eye_height").AsFloat(), 0);
                const glm::vec3 target = worldPos(b) + glm::vec3(0, 0.5f, 0);
                const glm::vec3 d = target - eye;
                dist = glm::length(d);
                if (dist > 1e-4f) {
                    const float c = std::clamp(glm::dot(forward(a), d / dist), -1.0f, 1.0f);
                    angle = glm::degrees(std::acos(c));
                }
                visible = dist <= ex.In("range").AsFloat() && angle <= ex.In("fov").AsFloat() * 0.5f &&
                          clearLine(a, eye, b, target);
            }
            ex.Out("visible", Value(visible));
            ex.Out("distance", Value(dist));
            ex.Out("angle", Value(angle));
            return NodeResult::Go("");
        };
        t.Keywords = {"perception", "vision", "detect", "fov"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("world.find_nearby", "Find Nearby", "World/Perception",
                          "Objects within a radius whose name contains the filter (empty — any), nearest first.");
        t.Inputs = {P("center", type::Vec3), P("radius", type::Float, Value(10.0f)),
                    P("name_contains", type::String, Value(std::string())), P("exclude", type::Object, noObject)};
        t.Outputs = {P("objects", type::Array), P("nearest", type::Object), P("count", type::Int)};
        t.Run = [this, worldPos](GraphExec& ex) {
            std::vector<std::pair<float, int>> found;
            if (m_scene) {
                const glm::vec3 c = ex.In("center").AsVec3();
                const float r2 = ex.In("radius").AsFloat() * ex.In("radius").AsFloat();
                const std::string filter = ex.In("name_contains").AsString();
                const int exclude = ex.In("exclude").AsEntity().Id;
                auto view = m_scene->Registry().view<IdComponent>();
                for (entt::entity e : view) {
                    GameObject o(&m_scene->Registry(), e);
                    if (o.Id() == exclude) continue;
                    if (!filter.empty() && o.Name().find(filter) == std::string::npos) continue;
                    const glm::vec3 d = worldPos(o) - c;
                    const float l2 = glm::dot(d, d);
                    if (l2 <= r2) found.push_back({l2, o.Id()});
                }
            }
            std::sort(found.begin(), found.end());
            std::vector<Value> items;
            for (const auto& [_, id] : found) items.push_back(Value(EntityRef{id}));
            ex.Out("nearest", found.empty() ? Value(EntityRef{}) : Value(EntityRef{found.front().second}));
            ex.Out("count", Value((int)items.size()));
            ex.Out("objects", Value::Array(std::move(items)));
            return NodeResult::Go("");
        };
        t.Keywords = {"search", "around", "radius", "overlap"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("nav.find_path", "Find Path", "World/Navigation",
                          "Points of a path from one place to another (the project's navigation, "
                          "or a straight line if it has none).");
        t.Inputs = {P("from", type::Vec3), P("to", type::Vec3)};
        t.Outputs = {P("path", type::Array), P("found", type::Bool)};
        t.Run = [this](GraphExec& ex) {
            const glm::vec3 a = ex.In("from").AsVec3(), b = ex.In("to").AsVec3();
            std::vector<Value> points;
            bool found = true;
            if (m_aiPathFinder.valid()) {
                auto res = m_aiPathFinder(a, b);
                if (!res.valid()) {
                    sol::error e = res;
                    ex.Error(e.what());
                }
                const Value v = res.return_count() > 0 ? AiValue(*this, res.get<sol::object>(0), "путь") : Value();
                found = v.Type() == sage::vars::Kind::Array && !v.Items().empty();
                points = v.Items();
            } else {
                points = {Value(a), Value(b)};
            }
            ex.Out("path", Value::Array(std::move(points)));
            ex.Out("found", Value(found));
            return NodeResult::Go("");
        };
        t.Keywords = {"navigation", "route", "pathfinding"};
        pure(std::move(t));
    }

    // --- Свойства компонентов и функции скриптов — по имени ----------------
    {
        NodeType t = Node("object.get", "Get Property", "World/Object",
                          "A property of the object by path: \"Transform.Position\", \"Light.Intensity\", "
                          "a variable of its script.");
        t.Inputs = {P("object", type::Object, noObject)};
        t.Outputs = {P("value", type::Any)};
        t.Settings = {S("path", "string", Value(std::string("Transform.Position")), "Property")};
        t.Run = [this](GraphExec& ex) {
            sol::protected_function get = m_aiNative["prop_get"];
            auto res = get(ValueToLua(ex.In("object")), ex.SettingText("path"));
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
            ex.Out("value", AiValue(*this, res.get<sol::object>(0), "свойство"));
            return NodeResult::Go("");
        };
        t.Keywords = {"component", "field", "read"};
        pure(std::move(t));
    }
    {
        NodeType t = Node("object.set", "Set Property", "World/Object", "Changes a property of the object by path.");
        // Действие над миром — в задачах и скриптах; AI Graph решает, а не действует.
        t.Domains = sage::ai::kDomainScript;
        t.Inputs = {X("in"), P("object", type::Object, noObject), P("value", type::Any)};
        t.Outputs = {X("then")};
        t.Settings = {S("path", "string", Value(std::string("Transform.Position")), "Property")};
        t.Run = [this](GraphExec& ex) {
            sol::protected_function set = m_aiNative["prop_set"];
            auto res = set(ValueToLua(ex.In("object")), ex.SettingText("path"), ValueToLua(ex.In("value")));
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
            return NodeResult::Go("then");
        };
        t.Keywords = {"component", "field", "write"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("object.call", "Call Script", "World/Object",
                          "Calls a function of the object's script by name and gives what it returned.");
        // Действие над миром — в задачах и скриптах; AI Graph решает, а не действует.
        t.Domains = sage::ai::kDomainScript;
        t.Inputs = {X("in"), P("object", type::Object, noObject), P("args", type::Array, Value::Array())};
        t.Outputs = {X("then"), P("result", type::Any)};
        t.Settings = {S("function", "string", Value(std::string()), "Function")};
        t.Run = [this](GraphExec& ex) {
            sol::protected_function call = m_aiNative["call_script"];
            sol::object args = ValueToLua(Value::Array(ex.In("args").Items()));
            auto res = call(ValueToLua(ex.In("object")), ex.SettingText("function"), args);
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
            ex.Out("result", res.return_count() > 0 ? AiValue(*this, res.get<sol::object>(0), "результат") : Value());
            return NodeResult::Go("then");
        };
        t.Keywords = {"method", "function", "message", "component"};
        r.Register(std::move(t));
    }
    {
        NodeType t = Node("lua.call", "Call Lua Function", "Functions",
                          "Calls any global Lua function by path (\"MyLib.compute\") with the arguments.");
        t.Inputs = {X("in"), P("args", type::Array, Value::Array())};
        t.Outputs = {X("then"), P("result", type::Any)};
        t.Settings = {S("function", "string", Value(std::string()), "Function")};
        t.Run = [this](GraphExec& ex) {
            sol::protected_function call = m_aiNative["lua_call"];
            auto res = call(ex.SettingText("function"), ValueToLua(Value::Array(ex.In("args").Items())));
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
            ex.Out("result", res.return_count() > 0 ? AiValue(*this, res.get<sol::object>(0), "результат") : Value());
            return NodeResult::Go("then");
        };
        t.Keywords = {"script", "function"};
        r.Register(std::move(t));
    }

    // --- Узел на каждую функцию API скриптов -------------------------------
    for (const sage::lang::ApiFunction* f : sage::lang::AllApiFunctions()) {
        const std::string full = f->Name;
        if (full.rfind("sage.", 0) != 0) continue;
        const size_t dot = full.find('.', 5);
        if (dot == std::string::npos) continue;
        const std::string mod = full.substr(5, dot - 5), fn = full.substr(dot + 1);
        if (mod == "ai") continue;   // своё API ИИ граф зовёт своими узлами
        std::vector<PinSpec> params;
        bool usable = true;
        std::string list = f->Params;
        size_t pos = 0;
        while (pos < list.size() && usable) {
            size_t comma = list.find(',', pos);
            if (comma == std::string::npos) comma = list.size();
            const std::string part = list.substr(pos, comma - pos);
            pos = comma + 1;
            if (part.empty() || part == "...") continue;
            const size_t colon = part.find(':');
            std::string name = part.substr(0, colon);
            const std::string ty = colon == std::string::npos ? "any" : part.substr(colon + 1);
            if (!name.empty() && name.back() == '?') name.pop_back();
            if (ty == "function") usable = false;   // функцию проводом не передать
            params.push_back(P(name, PinTypeOf(ty)));
        }
        if (!usable) continue;
        NodeType t = Node("api." + mod + "." + fn, mod + "." + fn, "Engine API/" + mod,
                          "sage." + mod + "." + fn + "(" + f->Params + ")");
        t.Origin = "API";
        t.Inputs = {X("in")};
        for (const PinSpec& p : params) t.Inputs.push_back(p);
        t.Outputs = {X("then")};
        // Результат — всегда: у многих функций тип возврата в таблице не
        // выведен («nil»), хотя они его возвращают (Clamp, Lerp).
        t.Outputs.push_back(P("result", PinTypeOf(f->Ret)));
        t.Run = [this, mod, fn, params](GraphExec& ex) {
            sol::object target = m_lua["sage"][mod][fn];
            if (target.get_type() != sol::type::function) ex.Error("в движке нет sage." + mod + "." + fn);
            sol::protected_function call = target.as<sol::protected_function>();
            std::vector<sol::object> args;
            // Хвостовые незаданные аргументы не передаются вовсе: у функции
            // движка своё умолчание, и ноль вместо него — другое поведение.
            size_t last = 0;
            for (size_t i = 0; i < params.size(); ++i)
                if (ex.Has(params[i].Name)) last = i + 1;
            for (size_t i = 0; i < last; ++i)
                args.push_back(ex.Has(params[i].Name) ? ValueToLua(ex.In(params[i].Name)) : sol::object(sol::lua_nil));
            auto res = call(sol::as_args(args));
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
            if (sage::ai::FindPin(ex.Pins().Outputs, "result"))
                ex.Out("result", res.return_count() > 0 ? AiValue(*this, res.get<sol::object>(0), "результат") : Value());
            return NodeResult::Go("then");
        };
        r.Register(std::move(t));
    }
}
