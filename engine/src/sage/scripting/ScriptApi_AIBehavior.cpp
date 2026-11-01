#include "ScriptApiAIDetail.h"

#include <algorithm>
#include <cmath>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/core/Log.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// AI GRAPH: ЗАДАЧИ ПОВЕДЕНИЯ В МИРЕ И КОНТЕКСТ ЦЕЛИ.
//
// Здесь — то, что связывает поведение с игрой ЯВНЫМИ точками входа:
// перемещение идёт контроллером персонажа (физика, лестницы, стены — его
// забота), видимость — лучом физики, «взаимодействие» — функцией скрипта
// объекта. Никаких игровых операций «на всякий случай»: только то, что
// нужно поведению, с результатом успеха или неудачи.
//
// ЦЕЛЬ — память персонажа "target" (ссылка на объект). Её ставит Find Target
// или Select Target, читают Current Target и условия, по ней идут Move To
// Target и Follow Target. Это память ЭКЗЕМПЛЯРА: конец одной задачи цель не
// стирает — следующая знает, за кем идти.
//
// ДОЛГИЕ ЗАДАЧИ (Move To Target, Follow Target) запускаются один раз и
// продолжаются кадр за кадром; отмена (Decide, If Condition, Abort) просто
// снимает ветку — шаг движения больше не делается, персонаж останавливается.
// ---------------------------------------------------------------------------
namespace {

using namespace sage::ai::nodes;
using namespace sage::scripting::aidetail;
using sage::ai::GraphExec;
using sage::ai::NodeResult;
using sage::ai::NodeType;
namespace type = sage::ai::type;
using sage::vars::EntityRef;
using sage::vars::Value;

constexpr const char* kTarget = "target";

NodeType AiNode(const std::string& id, const std::string& title, const std::string& category,
                const std::string& description) {
    NodeType t = Node(id, title, category, description);
    t.Domains = sage::ai::kDomainAI;
    return t;
}

} // namespace

void ScriptEngine::RegisterAiBehaviorNodes() {
    sage::ai::NodeRegistry& r = AI().Nodes();

    auto byId = [this](int id) -> GameObject {
        if (!m_scene || id <= 0) return GameObject(nullptr, entt::null);
        return m_scene->Get(id);
    };
    auto self = [byId](GraphExec& ex) { return byId(ex.Task().Owner()); };
    auto target = [byId](GraphExec& ex) { return byId(ex.Task().Instance().Get(kTarget).AsEntity().Id); };
    auto worldPos = [this](const GameObject& o) { return glm::vec3(m_scene->WorldMatrixById(o.Id())[3]); };
    auto forward = [this](const GameObject& o) {
        const glm::vec3 f = -glm::vec3(m_scene->WorldMatrixById(o.Id())[2]);
        const float l = glm::length(f);
        return l > 1e-6f ? f / l : glm::vec3(0, 0, -1);
    };
    // Видит ли a объект b: дальность, угол обзора, ничего ли не стоит между.
    auto sees = [this, worldPos, forward](const GameObject& a, const GameObject& b, float fov, float range,
                                          float eye, float& dist) {
        const glm::vec3 from = worldPos(a) + glm::vec3(0, eye, 0);
        const glm::vec3 to = worldPos(b) + glm::vec3(0, 0.5f, 0);
        const glm::vec3 d = to - from;
        dist = glm::length(d);
        if (dist > range) return false;
        if (dist > 1e-4f) {
            const float c = std::clamp(glm::dot(forward(a), d / dist), -1.0f, 1.0f);
            if (glm::degrees(std::acos(c)) > fov * 0.5f) return false;
        }
        if (!m_physics || dist < 1e-4f) return true;
        const PhysicsScene::EntityHit h = m_physics->Raycast(from, d / dist, dist);
        return !h.Hit || h.Entity == b.Entity() || h.Entity == a.Entity();
    };
    // Шаг к точке по земле: контроллером персонажа, если он есть.
    auto step = [this, worldPos](GraphExec& ex, GameObject& o, const glm::vec3& goal, float speed, float dt) {
        glm::vec3 d = goal - worldPos(o);
        d.y = 0.0f;
        const float dist = glm::length(d);
        if (dist < 1e-5f || dt <= 0.0f) return;
        const glm::vec3 v = d / dist * speed;
        if (o.Registry()->all_of<CharacterControllerComponent>(o.Entity())) {
            sol::protected_function move = m_lua["sage"]["physics"]["MoveCharacter"];
            auto res = move(o, v, dt);
            if (!res.valid()) {
                sol::error e = res;
                ex.Error(e.what());
            }
        } else {
            const glm::vec3 s = v * dt;
            o.GetTransform().Position += glm::length(s) > dist ? d : s;
        }
    };
    auto flatDistance = [worldPos](const GameObject& a, const GameObject& b) {
        glm::vec3 d = worldPos(b) - worldPos(a);
        d.y = 0.0f;
        return glm::length(d);
    };
    auto pure = [&r](NodeType t) {
        t.Pure = true;
        r.Register(std::move(t));
    };

    // --- Контекст --------------------------------------------------------------
    {
        NodeType t = AiNode("ai.self", "Self", "AI Context", "The character this behavior belongs to.");
        t.Outputs = {P("object", type::Object), P("position", type::Vec3), P("forward", type::Vec3)};
        t.Run = [self, worldPos, forward](GraphExec& ex) {
            GameObject o = self(ex);
            ex.Out("object", Value(EntityRef{o.Valid() ? o.Id() : 0}));
            ex.Out("position", Value(o.Valid() ? worldPos(o) : glm::vec3(0.0f)));
            ex.Out("forward", Value(o.Valid() ? forward(o) : glm::vec3(0, 0, -1)));
            return NodeResult::Go("");
        };
        t.Keywords = {"me", "owner", "character"};
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.target", "Current Target", "AI Context",
                            "The target of the character (set by Find Target or Select Target): the object, "
                            "whether it still exists, where it is and how far.");
        t.Outputs = {P("object", type::Object), P("exists", type::Bool, Value(false)), P("position", type::Vec3),
                     P("distance", type::Float)};
        t.Run = [self, target, worldPos, flatDistance](GraphExec& ex) {
            GameObject me = self(ex), o = target(ex);
            const bool ok = o.Valid();
            ex.Out("object", Value(EntityRef{ok ? o.Id() : 0}));
            ex.Out("exists", Value(ok));
            ex.Out("position", Value(ok ? worldPos(o) : glm::vec3(0.0f)));
            ex.Out("distance", Value(ok && me.Valid() ? flatDistance(me, o) : 0.0f));
            return NodeResult::Go("");
        };
        t.Keywords = {"enemy", "goal", "focus"};
        pure(std::move(t));
    }

    // --- Условия ---------------------------------------------------------------
    {
        NodeType t = AiNode("ai.has_target", "Has Target", "Conditions",
                            "Whether the character has a target and it still exists.");
        t.Outputs = {P("result", type::Bool, Value(false))};
        t.Run = [target](GraphExec& ex) {
            ex.Out("result", Value(target(ex).Valid()));
            return NodeResult::Go("");
        };
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.target_visible", "Can See Target", "Conditions",
                            "Whether the target is within range, inside the field of view and not hidden.");
        t.Inputs = {P("fov", type::Float, Value(120.0f)), P("range", type::Float, Value(20.0f)),
                    P("eye_height", type::Float, Value(1.6f))};
        t.Outputs = {P("result", type::Bool, Value(false)), P("distance", type::Float)};
        t.Run = [self, target, sees](GraphExec& ex) {
            GameObject me = self(ex), o = target(ex);
            float dist = 0.0f;
            const bool v = me.Valid() && o.Valid() &&
                           sees(me, o, ex.In("fov").AsFloat(), ex.In("range").AsFloat(), ex.In("eye_height").AsFloat(),
                                dist);
            ex.Out("result", Value(v));
            ex.Out("distance", Value(dist));
            return NodeResult::Go("");
        };
        t.Keywords = {"see", "vision", "perception", "available", "reachable"};
        pure(std::move(t));
    }
    {
        NodeType t = AiNode("ai.target_near", "Target Within Distance", "Conditions",
                            "Whether the target is not farther than the distance (along the ground).");
        t.Inputs = {P("distance", type::Float, Value(2.0f))};
        t.Outputs = {P("result", type::Bool, Value(false)), P("current", type::Float)};
        t.Run = [self, target, flatDistance](GraphExec& ex) {
            GameObject me = self(ex), o = target(ex);
            const float d = me.Valid() && o.Valid() ? flatDistance(me, o) : 0.0f;
            ex.Out("current", Value(d));
            ex.Out("result", Value(me.Valid() && o.Valid() && d <= ex.In("distance").AsFloat()));
            return NodeResult::Go("");
        };
        t.Keywords = {"close", "range", "near", "far"};
        pure(std::move(t));
    }

    // --- Задачи поведения --------------------------------------------------------
    {
        NodeType t = AiNode("ai.find_target", "Find Target", "Behavior Tasks",
                            "Looks for the nearest object whose name contains the filter within the radius "
                            "(optionally only one it can see) and makes it the target.");
        t.Inputs = {X("in"), P("radius", type::Float, Value(15.0f)), P("must_see", type::Bool, Value(false)),
                    P("fov", type::Float, Value(120.0f))};
        t.Outputs = {X("success"), X("failure"), P("target", type::Object)};
        t.Settings = {S("name_contains", "string", Value(std::string()), "Name contains")};
        t.Run = [this, self, worldPos, sees](GraphExec& ex) {
            GameObject me = self(ex);
            if (!me.Valid() || !m_scene) ex.Error("у поведения нет персонажа");
            const std::string filter = ex.SettingText("name_contains");
            const float radius = ex.In("radius").AsFloat();
            const bool mustSee = ex.In("must_see").AsBool();
            const float fov = ex.In("fov").AsFloat();
            const glm::vec3 c = worldPos(me);
            int best = 0;
            float bestD = radius * radius + 1.0f;
            for (entt::entity e : m_scene->Registry().view<IdComponent>()) {
                GameObject o(&m_scene->Registry(), e);
                if (o.Id() == me.Id() || (!filter.empty() && o.Name().find(filter) == std::string::npos)) continue;
                const glm::vec3 d = worldPos(o) - c;
                const float d2 = glm::dot(d, d);
                if (d2 > radius * radius || d2 >= bestD) continue;
                float dist = 0.0f;
                if (mustSee && !sees(me, o, fov, radius, 1.6f, dist)) continue;
                best = o.Id();
                bestD = d2;
            }
            ex.Out("target", Value(EntityRef{best}));
            if (!best) {
                ex.Report("failed");
                ex.SetLastResult(false, "Find Target", "цель не найдена");
                ex.FailReason("Find Target: никого" + (filter.empty() ? std::string() : " с '" + filter + "'") +
                              " в радиусе " + std::to_string((int)radius));
                return NodeResult::Go("failure");
            }
            ex.Task().Instance().Set(kTarget, Value(EntityRef{best}));
            ex.Report("succeeded");
            ex.SetLastResult(true, "Find Target", {});
            return NodeResult::Go("success");
        };
        t.Keywords = {"search", "detect", "look", "enemy"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.select_target", "Select Target", "Behavior Tasks",
                            "Makes the object the target of the character; fails if it does not exist.");
        t.Inputs = {X("in"), P("object", type::Object, Value(EntityRef{}))};
        t.Outputs = {X("success"), X("failure")};
        t.Run = [byId](GraphExec& ex) {
            GameObject o = byId(ex.In("object").AsEntity().Id);
            if (!o.Valid()) {
                ex.FailReason("Select Target: объекта нет");
                return NodeResult::Go("failure");
            }
            ex.Task().Instance().Set(kTarget, Value(EntityRef{o.Id()}));
            return NodeResult::Go("success");
        };
        t.Keywords = {"choose", "focus", "set target"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.forget_target", "Forget Target", "Behavior Tasks", "Clears the target of the character.");
        t.Inputs = {X("in")};
        t.Outputs = {X("then")};
        t.Run = [](GraphExec& ex) {
            ex.Task().Instance().Set(kTarget, Value(EntityRef{}));
            return NodeResult::Go("then");
        };
        t.Keywords = {"clear", "lose", "reset"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.move_to", "Move To Target", "Behavior Tasks",
                            "Walks to the target (or to a point) frame by frame with the character controller; "
                            "succeeds on arrival, fails if the target is gone or the timeout runs out.");
        t.Inputs = {X("in"), P("point", type::Vec3), P("speed", type::Float, Value(3.0f)),
                    P("arrive_distance", type::Float, Value(1.0f)), P("timeout", type::Float, Value(0.0f))};
        t.Outputs = {X("success"), X("failure"), P("distance", type::Float)};
        t.Settings = {S("destination", "choice", Value(std::string("target")), "Go to", {"target", "point"})};
        t.Run = [self, target, worldPos, step](GraphExec& ex) {
            sage::ai::NodeState& s = ex.State();
            const double now = ex.GetWorld().Time();
            auto fail = [&](const std::string& why) {
                ex.Report("failed");
                ex.SetLastResult(false, "Move To Target", why);
                ex.FailReason("Move To Target: " + why);
                return NodeResult::Go("failure");
            };
            GameObject me = self(ex);
            if (!me.Valid()) return fail("у поведения нет персонажа");
            const bool toTarget = ex.SettingText("destination") != "point";
            GameObject goalObj = toTarget ? target(ex) : GameObject(nullptr, entt::null);
            if (toTarget && !goalObj.Valid()) return fail("цели нет");
            if (!ex.Resumed()) {
                const float timeout = ex.In("timeout").AsFloat();
                s.Until = timeout > 0.0f ? now + timeout : 0.0;
                s.NextEval = now;
            }
            const float dt = (float)(now - s.NextEval);
            s.NextEval = now;
            const glm::vec3 goal = toTarget ? worldPos(goalObj) : ex.In("point").AsVec3();
            glm::vec3 d = goal - worldPos(me);
            d.y = 0.0f;
            const float dist = glm::length(d);
            ex.Out("distance", Value(dist));
            if (dist <= ex.In("arrive_distance").AsFloat()) {
                ex.Report("succeeded");
                ex.SetLastResult(true, "Move To Target", {});
                return NodeResult::Go("success");
            }
            if (s.Until > 0.0 && now >= s.Until) return fail("не дошёл за отведённое время");
            ex.Report("running");
            step(ex, me, goal, ex.In("speed").AsFloat(), dt);
            ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"walk", "go", "chase", "approach", "navigate"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.follow", "Follow Target", "Behavior Tasks",
                            "Keeps near the target for a time (0 — until interrupted); fails if the target is "
                            "gone or gets farther than the lose distance (0 — never).");
        t.Inputs = {X("in"), P("keep_distance", type::Float, Value(2.0f)), P("speed", type::Float, Value(3.5f)),
                    P("duration", type::Float, Value(0.0f)), P("lose_distance", type::Float, Value(0.0f))};
        t.Outputs = {X("success"), X("failure")};
        t.Run = [self, target, worldPos, step, flatDistance](GraphExec& ex) {
            sage::ai::NodeState& s = ex.State();
            const double now = ex.GetWorld().Time();
            auto fail = [&](const std::string& why) {
                ex.Report("failed");
                ex.SetLastResult(false, "Follow Target", why);
                ex.FailReason("Follow Target: " + why);
                return NodeResult::Go("failure");
            };
            GameObject me = self(ex), o = target(ex);
            if (!me.Valid()) return fail("у поведения нет персонажа");
            if (!o.Valid()) return fail("цель пропала");
            if (!ex.Resumed()) {
                const float duration = ex.In("duration").AsFloat();
                s.Until = duration > 0.0f ? now + duration : 0.0;
                s.NextEval = now;
            }
            const float dt = (float)(now - s.NextEval);
            s.NextEval = now;
            const float dist = flatDistance(me, o);
            const float lose = ex.In("lose_distance").AsFloat();
            if (lose > 0.0f && dist > lose) return fail("цель ушла дальше " + std::to_string((int)lose) + " м");
            if (s.Until > 0.0 && now >= s.Until) {
                ex.Report("succeeded");
                ex.SetLastResult(true, "Follow Target", {});
                return NodeResult::Go("success");
            }
            ex.Report("running");
            if (dist > ex.In("keep_distance").AsFloat()) step(ex, me, worldPos(o), ex.In("speed").AsFloat(), dt);
            ex.WaitFrame();
            return NodeResult::Wait();
        };
        t.Keywords = {"chase", "escort", "accompany", "pursue"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.face_target", "Face Target", "Behavior Tasks",
                            "Turns the character (around the vertical) toward the target.");
        t.Inputs = {X("in")};
        t.Outputs = {X("success"), X("failure")};
        t.Run = [self, target, worldPos](GraphExec& ex) {
            GameObject me = self(ex), o = target(ex);
            if (!me.Valid() || !o.Valid()) {
                ex.FailReason("Face Target: " + std::string(me.Valid() ? "цели нет" : "у поведения нет персонажа"));
                return NodeResult::Go("failure");
            }
            const glm::vec3 d = worldPos(o) - worldPos(me);
            if (d.x * d.x + d.z * d.z > 1e-8f) me.GetTransform().Rotation.y = glm::degrees(std::atan2(-d.x, -d.z));
            return NodeResult::Go("success");
        };
        t.Keywords = {"look", "turn", "aim"};
        r.Register(std::move(t));
    }
    {
        NodeType t = AiNode("ai.interact", "Interact", "Behavior Tasks",
                            "Calls a function of the target's script (or of the given object) — open a door, "
                            "pick up, hit. Fails if there is no such object, the call fails, or it returned false.");
        t.Inputs = {X("in"), P("object", type::Object, Value(EntityRef{})), P("args", type::Array, Value::Array())};
        t.Outputs = {X("success"), X("failure"), P("result", type::Any)};
        t.Settings = {S("function", "string", Value(std::string()), "Function"),
                      S("with", "choice", Value(std::string("target")), "With", {"target", "object"})};
        t.Run = [this, target, byId](GraphExec& ex) {
            const std::string fn = ex.SettingText("function");
            if (fn.empty()) ex.Error("не задана функция");
            GameObject o = ex.SettingText("with") == "object" ? byId(ex.In("object").AsEntity().Id) : target(ex);
            auto fail = [&](const std::string& why) {
                ex.Report("failed");
                ex.SetLastResult(false, "Interact", why);
                ex.FailReason("Interact '" + fn + "': " + why);
                return NodeResult::Go("failure");
            };
            if (!o.Valid()) return fail("объекта нет");
            sol::protected_function call = m_aiNative["call_script"];
            sol::object args = ValueToLua(Value::Array(ex.In("args").Items()));
            auto res = call(ValueToLua(Value(EntityRef{o.Id()})), fn, args);
            if (!res.valid()) {
                sol::error e = res;
                return fail(e.what());
            }
            const sol::object first = res.return_count() > 0 ? res.get<sol::object>(0) : sol::object(sol::lua_nil);
            ex.Out("result", first.valid() && first.get_type() != sol::type::lua_nil ? AiValue(*this, first, "результат")
                                                                                    : Value());
            if (first.get_type() == sol::type::boolean && !first.as<bool>()) return fail("функция вернула false");
            ex.Report("succeeded");
            ex.SetLastResult(true, "Interact", {});
            return NodeResult::Go("success");
        };
        t.Keywords = {"use", "call", "action", "open", "pick"};
        r.Register(std::move(t));
    }
}
