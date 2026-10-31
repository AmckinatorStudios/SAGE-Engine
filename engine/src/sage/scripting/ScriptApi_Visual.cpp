#include "ScriptApiAIDetail.h"

#include <stdexcept>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/scene/VisualScriptComponent.h"

// ---------------------------------------------------------------------------
// VISUAL SCRIPTING В LUA И В СЦЕНЕ.
//
//      visual.node("game.add_score", {inputs = {{"points", "int", 1}},
//          run = function(n) score = score + n.inputs.points end})
//      local out = visual.call("DamageFormula", {base = 10, armor = 3})
//      visual.of(door).vars.locked = false
//
// Visual Scripting и SAGE AI Graph — одна основа (документ, выполнение,
// типы, переменные, проверка, отладка) с разными наборами узлов. Поэтому
// здесь нет второго исполнителя: граф объекта выполняется тем же миром, что и
// ИИ, а различие — в назначении узлов (sage::ai::NodeDomain) и в том, кто его
// запускает: компонент Visual Script, а не компонент AI.
//
// Свои узлы Visual Scripting — visual.node (тот же реестр, что ai.node, но для
// другого редактора); типы общие — ai.type / visual.type — одна таблица.
// ---------------------------------------------------------------------------
namespace {

using namespace sage::scripting::aidetail;
using sage::ai::GraphExec;
using sage::ai::NodeResult;
using sage::ai::NodeType;
using sage::vars::Value;
using namespace sage::ai::nodes;
namespace type = sage::ai::type;

} // namespace

void ScriptEngine::RegisterVisualApi() {
    // Событие игры — в общую шину (sage.events): его слышат скрипты, связи
    // сигналов в инспекторе и графы (On Event). Общий узел обоих редакторов.
    {
        NodeType t = Node("script.emit", "Emit Game Event", "Events",
                          "Sends an event to the game: scripts (sage.events), inspector links and graphs hear it.");
        t.Inputs = {X("in"), P("data", type::Any)};
        t.Outputs = {X("then")};
        t.Settings = {S("event", "event", Value(std::string()), "Event")};
        t.Run = [this](GraphExec& ex) {
            DispatchEvent(ex.SettingText("event"), ValueToLua(ex.In("data")));
            return NodeResult::Go("then");
        };
        t.Keywords = {"send", "broadcast", "signal"};
        AI().Nodes().Register(std::move(t));
    }

    // Свой узел Visual Scripting.
    Bind("visual", "node", nullptr, [this](const std::string& id, sol::table spec) {
        RegisterLuaGraphNode(id, spec, sage::ai::kDomainScript);
    });
    // Свой тип — общий для обоих редакторов (тот же реестр, что ai.type).
    Bind("visual", "type", nullptr, [this](const std::string& id, sol::table spec) {
        sol::protected_function f = m_lua["sage"]["ai"]["type"];
        auto r = f(id, spec);
        if (!r.valid()) {
            sol::error e = r;
            throw std::runtime_error(e.what());
        }
    });
    Bind("visual", "graph", nullptr, [this](const std::string& path) -> std::string {
        const std::string name = LoadAiGraph(path);
        if (name.empty()) throw std::runtime_error("visual.graph: граф '" + path + "' не загрузился (причина в журнале)");
        return name;
    });

    // Вызвать граф-функцию: результат — таблица выходов в тот же вызов, если
    // функция не ждёт; ждущая — ручка выполнения, как у ai.agent().start.
    Bind("visual", "call", nullptr, [this](const std::string& name, sol::optional<sol::table> inputs) -> sol::object {
        std::shared_ptr<const sage::ai::GraphDoc> g = AI().FindGraph(name);
        if (!g || g->Kind != sage::ai::GraphKind::Function)
            throw std::runtime_error("visual.call: '" + name + "' — не граф-функция");
        sage::ai::Blackboard in;
        if (inputs)
            for (const auto& kv : *inputs)
                if (kv.first.is<std::string>())
                    in.Set(kv.first.as<std::string>(), AiValue(*this, kv.second, "visual.call('" + name + "')"));
        // Функции зовутся от общего экземпляра «библиотеки»: у функции нет
        // своего объекта, а память ей не нужна.
        static const char* kLibrary = "__visual_library";
        int agent = 0;
        if (sol::object a = m_aiNative[kLibrary]; a.is<int>() && AI().FindAgent(a.as<int>())) agent = a.as<int>();
        else {
            agent = AI().CreateAgent();
            m_aiNative[kLibrary] = agent;
        }
        sage::ai::RunPtr run = AI().RunNow(agent, name, in);
        if (run->Rejected) throw std::runtime_error("visual.call: " + run->Error);
        if (run->Status == sage::ai::TaskStatus::Failed)
            throw std::runtime_error("visual.call('" + name + "'): " + run->Error);
        if (run->Status == sage::ai::TaskStatus::Running) {
            sol::protected_function handle = m_aiNative["handle"];
            auto h = handle(AiRunRef{run});
            return h.valid() ? h.get<sol::object>() : sol::object(sol::lua_nil);
        }
        sol::table out = m_lua.create_table();
        for (const auto& [k, v] : run->Outputs->All()) out[k] = ValueToLua(v);
        return out;
    });
    // Имена графов-функций — общей библиотеки обоих редакторов.
    Bind("visual", "functions", nullptr, [this]() -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const std::string& n : AI().TaskNames())
            if (std::shared_ptr<const sage::ai::GraphDoc> g = AI().FindGraph(n); g && g->Kind == sage::ai::GraphKind::Function)
                t[i++] = n;
        return t;
    });
    // Выполнение Visual Script объекта: visual.of(door).vars.locked = false.
    Bind("visual", "of", nullptr, [this](sol::object obj) -> sol::object {
        const int owner = OwnerId(obj);
        auto it = m_vsAgents.find(owner);
        if (it == m_vsAgents.end() || !AI().FindAgent(it->second)) return sol::lua_nil;
        sol::table t = m_lua.create_table();
        t["vars"] = AiVars{AI().FindAgent(it->second)->Memory};
        const int agent = it->second;
        t["emit"] = [this, agent](const std::string& event, sol::object data) {
            return AI().Emit(event, AiValue(*this, data, "visual.of(...).emit"), agent);
        };
        t["stop"] = [this, agent]() { AI().StopAgent(agent); };
        return t;
    });
}

// Компонент Visual Script: у объекта — своё выполнение графа.
void ScriptEngine::UpdateVisualScripts() {
    entt::registry& reg = m_scene->Registry();
    auto view = reg.view<VisualScriptComponent>();
    for (entt::entity e : view) {
        GameObject obj(&reg, e);
        if (!obj.Valid()) continue;
        const int id = obj.Id();
        if (m_vsAgents.count(id) || m_vsFailed.count(id)) continue;
        const VisualScriptComponent& c = view.get<VisualScriptComponent>(e);
        if (c.Graph.empty()) continue;
        const int agent = m_ai->CreateAgent(id, "world");
        m_vsAgents[id] = agent;
        if (sage::ai::Agent* a = m_ai->FindAgent(agent))
            for (const sage::vars::Var& v : c.Vars.All()) a->Memory->Set(v.Name, v.Data);
        auto known = m_aiGraphs.find(c.Graph);
        const std::string task = known != m_aiGraphs.end() ? known->second : LoadAiGraph(c.Graph);
        std::shared_ptr<const sage::ai::GraphDoc> g = task.empty() ? nullptr : m_ai->FindGraph(task);
        if (!g || g->Kind != sage::ai::GraphKind::Script) {
            if (g) LOG_ERROR("Script") << "Visual Script объекта '" << obj.Name() << "': '" << task
                                       << "' — не граф Visual Scripting";
            m_vsFailed.insert(id);
            continue;
        }
        sage::ai::RunPtr run = m_ai->Start(agent, task);
        if (run->Rejected) {
            LOG_ERROR("Script") << "Visual Script объекта '" << obj.Name() << "': " << run->Error;
            m_vsFailed.insert(id);
        }
    }
    for (auto it = m_vsAgents.begin(); it != m_vsAgents.end();) {
        if (!m_scene->Get(it->first).Valid()) it = m_vsAgents.erase(it);
        else ++it;
    }
}
