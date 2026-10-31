#include "ScriptApiAIDetail.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "sage/ai/GraphNodesDetail.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// SAGE AI GRAPH В LUA — расширение графа без правки движка.
//
//      ai.node("my.heal", {
//          title = "Heal", category = "My game", description = "Restores health",
//          inputs  = {{"target", "object"}, {"amount", "float", 10}},
//          outputs = {{"healed", "float"}},
//          run = function(n) n.outputs.healed = heal(n.inputs.target, n.inputs.amount) end,
//      })
//      ai.type("Target", {fields = {{"pos", Vec3()}, {"seen", false}}})
//      ai.connect_rule(function(from, to) if from == "Target" and to == "vec3" then return true end end)
//
// Узел на Lua — такая же запись реестра, как встроенный: палитра редактора,
// пины, проверка проводов и выполнение берут его оттуда же. Узел выполняется
// мгновенно; то, что длится (идти, ждать), — задача на Lua (ai.task), а в
// графе её зовёт Run Task.
//
// Здесь же — БИБЛИОТЕКА ИИ ПРОЕКТА (LoadAiLibrary) и компонент AI объектов
// сцены (UpdateAiAgents): «повесил на стража граф — страж ведёт себя».
// ---------------------------------------------------------------------------
namespace {

using namespace sage::scripting::aidetail;
using sage::ai::GraphExec;
using sage::ai::NodeResult;
using sage::ai::PinSpec;
using sage::vars::Value;

// Узел на Lua видит выполнение только на время своего вызова: прокси,
// оставленный скриптом «на потом», после вызова честно говорит об этом, а не
// читает чужую память.
struct NodeCall {
    GraphExec* Ex = nullptr;
};
struct AiNodeIn {
    std::shared_ptr<NodeCall> C;
};
struct AiNodeOut {
    std::shared_ptr<NodeCall> C;
};
struct AiNodeSettings {
    std::shared_ptr<NodeCall> C;
};

GraphExec& Live(const std::shared_ptr<NodeCall>& c) {
    if (!c || !c->Ex) throw std::runtime_error("узел уже отработал: его входы и выходы живут только во время run");
    return *c->Ex;
}

// Пины из Lua: {{"имя", "тип", умолчание, "подпись"}, ...}.
std::vector<PinSpec> ReadPins(ScriptEngine& se, const sol::object& o, const std::string& node) {
    std::vector<PinSpec> out;
    if (o.get_type() != sol::type::table) return out;
    for (const auto& kv : o.as<sol::table>()) {
        if (kv.second.get_type() != sol::type::table)
            throw std::runtime_error("ai.node('" + node + "'): пин — таблица {имя, тип, умолчание}");
        sol::table p = kv.second;
        PinSpec pin;
        pin.Name = p.get_or<std::string>(1, "");
        pin.Type = p.get_or<std::string>(2, "any");
        sol::object def = p[3];
        if (def.valid() && def.get_type() != sol::type::lua_nil) pin.Default = AiValue(se, def, "ai.node('" + node + "')");
        pin.Title = p.get_or<std::string>(4, "");
        if (pin.Name.empty()) throw std::runtime_error("ai.node('" + node + "'): у пина нет имени");
        out.push_back(std::move(pin));
    }
    return out;
}

bool HasExec(const std::vector<PinSpec>& pins) {
    return std::any_of(pins.begin(), pins.end(), [](const PinSpec& p) { return p.Type == sage::ai::type::Exec; });
}

} // namespace

void ScriptEngine::RegisterAiGraphApi() {
    m_lua.new_usertype<AiNodeIn>("AiNodeIn", sol::no_constructor, sol::meta_function::index,
                                 [this](AiNodeIn& p, const std::string& name) { return ValueToLua(Live(p.C).In(name)); });
    m_lua.new_usertype<AiNodeOut>(
        "AiNodeOut", sol::no_constructor, sol::meta_function::new_index,
        [this](AiNodeOut& p, const std::string& name, sol::object v) {
            GraphExec& ex = Live(p.C);
            if (!sage::ai::FindPin(ex.Pins().Outputs, name))
                throw std::runtime_error("у узла нет выхода '" + name + "'");
            ex.Out(name, AiValue(*this, v, "выход '" + name + "'"));
        });
    m_lua.new_usertype<AiNodeSettings>("AiNodeSettings", sol::no_constructor, sol::meta_function::index,
                                       [this](AiNodeSettings& p, const std::string& name) {
                                           return ValueToLua(Live(p.C).Setting(name));
                                       });

    // Свой узел графа.
    Bind("ai", "node", nullptr, [this](const std::string& id, sol::table spec) {
        // Узел ИИ из Lua — и для графа решений, и для графа задачи (Visual
        // Scripting): действие «атаковать» нужно задаче, а не только решению.
        RegisterLuaGraphNode(id, spec, sage::ai::kDomainAI | sage::ai::kDomainLogic);
    });

    // Свой тип данных — структура с полями: ai.type("Target", {fields = {{"pos", Vec3()}}}).
    Bind("ai", "type", nullptr, [this](const std::string& id, sol::table spec) {
        if (id.empty()) throw std::runtime_error("ai.type: у типа должно быть имя");
        sage::ai::TypeInfo t;
        t.Id = id;
        t.Title = spec.get_or<std::string>("title", id);
        t.Description = spec.get_or<std::string>("description", "");
        t.Origin = "Lua";
        if (sol::object c = spec["color"]; c.get_type() == sol::type::number) t.Color = c.as<uint32_t>();
        sol::object fields = spec["fields"];
        if (fields.get_type() != sol::type::table)
            throw std::runtime_error("ai.type('" + id + "'): нужны fields = {{имя, умолчание}, ...}");
        for (const auto& kv : fields.as<sol::table>()) {
            if (kv.second.get_type() == sol::type::table && kv.first.get_type() == sol::type::number) {
                sol::table f = kv.second;
                t.Fields.push_back({f.get_or<std::string>(1, ""), AiValue(*this, f[2], "ai.type('" + id + "')")});
            } else if (kv.first.is<std::string>()) {
                t.Fields.push_back({kv.first.as<std::string>(), AiValue(*this, kv.second, "ai.type('" + id + "')")});
            }
        }
        if (t.Fields.empty()) throw std::runtime_error("ai.type('" + id + "'): у структуры нет полей");
        AI().Types().Register(std::move(t));
    });

    // Своё правило проводов: fn(from, to) -> true (можно), false (нельзя), nil (не моё).
    Bind("ai", "connect_rule", nullptr, [this](sol::protected_function fn) {
        AI().Types().AddConnectRule([fn](const std::string& from, const std::string& to) {
            auto r = fn(from, to);
            if (!r.valid() || r.return_count() == 0) return -1;
            sol::object v = r.get<sol::object>(0);
            if (!v.is<bool>()) return -1;
            return v.as<bool>() ? 1 : 0;
        });
    });

    // Своё приведение: fn(value, to) -> новое значение или nil.
    Bind("ai", "convert_rule", nullptr, [this](sol::protected_function fn) {
        AI().Types().AddConvertRule([this, fn](const Value& in, const std::string& to, Value& out) {
            auto r = fn(ValueToLua(in), to);
            if (!r.valid() || r.return_count() == 0) return false;
            sol::object v = r.get<sol::object>(0);
            if (!v.valid() || v.get_type() == sol::type::lua_nil) return false;
            out = AiValue(*this, v, "ai.convert_rule");
            return true;
        });
    });

    // Свой поиск пути (навигация проекта): fn(from, to) -> {точки} или nil.
    Bind("ai", "path_finder", nullptr, [this](sol::protected_function fn) { m_aiPathFinder = fn; });

    Bind("ai", "graph", nullptr, [this](const std::string& path) -> std::string {
        const std::string name = LoadAiGraph(path);
        if (name.empty()) throw std::runtime_error("ai.graph: граф '" + path + "' не загрузился (причина в журнале)");
        return name;
    });
    Bind("ai", "load_library", nullptr, [this](const std::string& dir) -> int { return LoadAiLibrary(dir); });
    Bind("ai", "nodes", nullptr, [this]() -> sol::table {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const sage::ai::NodeType* n : AI().Nodes().All()) t[i++] = n->Id;
        return t;
    });
    // ИИ объекта с компонентом AI: ai.agent_of(door).emit("open").
    Bind("ai", "agent_of", nullptr, [this](sol::object obj) -> sol::object {
        const int owner = OwnerId(obj);
        auto it = m_aiAgents.find(owner);
        const int id = it != m_aiAgents.end() ? it->second : AI().AgentOf(owner);
        if (!id || !AI().FindAgent(id)) return sol::lua_nil;
        sol::protected_function make = m_aiNative["agent_table"];
        auto r = make(id);
        return r.valid() ? r.get<sol::object>() : sol::object(sol::lua_nil);
    });
}

// ---------------------------------------------------------------------------
// Библиотека ИИ проекта и компонент AI.
// ---------------------------------------------------------------------------
std::string ScriptEngine::ResolveAiPath(const std::string& path) const {
    if (m_aiRoot.empty() || path.empty()) return path;
    const std::filesystem::path p = sage::PathFromUtf8(path);
    if (p.is_absolute()) return path;
    return sage::PathToUtf8(sage::PathFromUtf8(m_aiRoot) / p);
}

std::string ScriptEngine::LoadAiGraph(const std::string& path) { return LoadAiGraphFile(ResolveAiPath(path), path); }

std::string ScriptEngine::LoadAiGraphFile(const std::string& full, const std::string& key) {
    std::string text;
    if (!sage::assets::vfs::ReadText(full, text)) {
        LOG_ERROR("AI") << "Граф не найден: " << full;
        return {};
    }
    sage::ai::GraphDoc doc;
    std::string err;
    if (!sage::ai::LoadGraph(text, doc, err)) {
        LOG_ERROR("AI") << "Граф " << full << ": " << err;
        return {};
    }
    // Без имени граф зовётся по файлу: «guard.aigraph» — задача «guard».
    if (doc.Name.empty()) doc.Name = sage::PathToUtf8(sage::PathFromUtf8(full).stem());
    if (!AI().RegisterGraph(doc, err)) {
        LOG_ERROR("AI") << "Граф " << full << ": " << err;
        return {};
    }
    m_aiGraphs[key] = doc.Name;
    return doc.Name;
}

int ScriptEngine::LoadAiLibrary(const std::string& directory) {
    const std::string root = ResolveAiPath(directory);
    std::vector<std::string> luaFiles, graphFiles;
    std::vector<std::string> dirs = {root};
    // Обход вглубь через vfs: в собранной игре папки на диске нет, она в пакете.
    for (size_t i = 0; i < dirs.size() && i < 256; ++i) {
        const std::string d = dirs[i];
        // Путь файла — папка + ИМЯ, а не то, что вернул обход: на диске он
        // отдаёт путь без ведущего «/», и абсолютная папка проекта (обычный
        // случай в редакторе) превращалась в несуществующий относительный путь.
        auto add = [&](const char* ext, std::vector<std::string>& out) {
            for (const std::string& f : sage::assets::vfs::ListFiles(d, ext))
                out.push_back(sage::PathToUtf8(sage::PathFromUtf8(d) / sage::PathFromUtf8(f).filename()));
        };
        add(".lua", luaFiles);
        add(".aigraph", graphFiles);
        // Графы Visual Scripting — та же библиотека: функции общие для обоих.
        add(".vgraph", graphFiles);
        for (const std::string& sub : sage::assets::vfs::ListDirs(d))
            dirs.push_back(sage::PathToUtf8(sage::PathFromUtf8(d) / sage::PathFromUtf8(sub)));
    }
    std::sort(luaFiles.begin(), luaFiles.end());
    std::sort(graphFiles.begin(), graphFiles.end());
    int loaded = 0;
    for (const std::string& f : luaFiles) {
        std::string text;
        if (!sage::assets::vfs::ReadText(f, text)) continue;
        auto r = m_lua.safe_script(text, sol::script_pass_on_error, "@" + f);
        if (!r.valid()) {
            sol::error e = r;
            LOG_ERROR("AI") << "Библиотека ИИ: " << e.what();
            continue;
        }
        ++loaded;
    }
    // Пути из обхода уже от корня проекта — второй раз его не приклеиваем.
    for (const std::string& f : graphFiles)
        if (!LoadAiGraphFile(f, f).empty()) ++loaded;
    return loaded;
}

void ScriptEngine::UpdateAiAgents() {
    if (!m_scene || !m_ai) return;
    UpdateVisualScripts();
    entt::registry& reg = m_scene->Registry();
    auto view = reg.view<AiAgentComponent>();
    for (entt::entity e : view) {
        GameObject obj(&reg, e);
        if (!obj.Valid()) continue;
        const int id = obj.Id();
        if (m_aiAgents.count(id) || m_aiFailed.count(id)) continue;
        const AiAgentComponent& c = view.get<AiAgentComponent>(e);
        const int agent = m_ai->CreateAgent(id, c.Shared);
        m_aiAgents[id] = agent;
        if (sage::ai::Agent* a = m_ai->FindAgent(agent))
            for (const sage::vars::Var& v : c.Memory.All()) a->Memory->Set(v.Name, v.Data);
        if (!c.AutoStart || c.Graph.empty()) continue;
        auto known = m_aiGraphs.find(c.Graph);
        const std::string task = known != m_aiGraphs.end() ? known->second : LoadAiGraph(c.Graph);
        if (task.empty()) {
            m_aiFailed.insert(id);
            continue;
        }
        sage::ai::RunPtr run = m_ai->Start(agent, task);
        if (run->Rejected) {
            LOG_ERROR("AI") << "ИИ объекта '" << obj.Name() << "': " << run->Error;
            m_aiFailed.insert(id);
        }
    }
    // Объект удалили — мир ИИ уже снял его экземпляр; забываем и мы. Живой
    // объект, чей экземпляр сняли скриптом, нового не получает: «выключить ИИ»
    // значит выключить, а не перезапустить в следующем кадре.
    for (auto it = m_aiAgents.begin(); it != m_aiAgents.end();) {
        if (!m_scene->Get(it->first).Valid()) it = m_aiAgents.erase(it);
        else ++it;
    }
}

// Свой узел графа из Lua — общий для ai.node и visual.node.
void ScriptEngine::RegisterLuaGraphNode(const std::string& id, sol::table spec, unsigned defaultDomain) {
    if (id.empty()) throw std::runtime_error("ai.node: у узла должно быть имя (id)");
    sage::ai::NodeType t;
    t.Id = id;
    t.Origin = "Lua";
    t.Title = spec.get_or<std::string>("title", id);
    t.Category = spec.get_or<std::string>("category", "Lua");
    t.Description = spec.get_or<std::string>("description", "");
    t.Pure = spec.get_or("pure", false);
    // Для какого графа: "ai" — только AI Graph (решения), "task" — AI Graph и
    // задачи, "script" — Visual Scripting, "both" — везде. Без указания — для
    // того, чей это вызов (ai.node — решения и задачи, visual.node — Visual Scripting).
    const std::string domain = spec.get_or<std::string>("domain", "");
    if (domain == "ai") t.Domains = sage::ai::kDomainAI;
    else if (domain == "task") t.Domains = sage::ai::kDomainAI | sage::ai::kDomainLogic;
    else if (domain == "script") t.Domains = sage::ai::kDomainScript;
    else if (domain == "both") t.Domains = sage::ai::kDomainAll;
    else if (domain.empty()) t.Domains = defaultDomain;
    else throw std::runtime_error("node('" + id + "'): domain — \"ai\", \"task\", \"script\" или \"both\"");
    t.Inputs = ReadPins(*this, spec["inputs"], id);
    t.Outputs = ReadPins(*this, spec["outputs"], id);
    // Узел с потоком без явных пинов потока получает обычные «in/then»:
    // писать их руками в каждом узле — шум.
    if (!t.Pure) {
        if (!HasExec(t.Inputs)) t.Inputs.insert(t.Inputs.begin(), sage::ai::nodes::X("in"));
        if (!HasExec(t.Outputs)) t.Outputs.insert(t.Outputs.begin(), sage::ai::nodes::X("then"));
    }
    if (sol::object st = spec["settings"]; st.get_type() == sol::type::table) {
        for (const auto& kv : st.as<sol::table>()) {
            if (kv.second.get_type() != sol::type::table) continue;
            sol::table s = kv.second;
            sage::ai::SettingSpec set;
            set.Name = s.get_or<std::string>(1, "");
            set.Kind = s.get_or<std::string>(2, "string");
            if (sol::object d = s[3]; d.valid() && d.get_type() != sol::type::lua_nil)
                set.Default = AiValue(*this, d, "ai.node('" + id + "')");
            if (sol::object ch = s[4]; ch.get_type() == sol::type::table)
                for (const auto& c : ch.as<sol::table>())
                    if (c.second.is<std::string>()) set.Choices.push_back(c.second.as<std::string>());
            if (!set.Name.empty()) t.Settings.push_back(std::move(set));
        }
    }
    sol::object run = spec["run"];
    if (run.get_type() != sol::type::function)
        throw std::runtime_error("ai.node('" + id + "'): нужна функция run(n)");
    sol::protected_function fn = run.as<sol::protected_function>();
    const bool pure = t.Pure;
    t.Run = [this, fn, pure](GraphExec& ex) -> NodeResult {
        auto call = std::make_shared<NodeCall>();
        call->Ex = &ex;
        sol::table n = m_lua.create_table();
        n["inputs"] = AiNodeIn{call};
        n["outputs"] = AiNodeOut{call};
        n["settings"] = AiNodeSettings{call};
        const int owner = ex.Task().Owner();
        n["self"] = owner > 0 ? ValueToLua(Value(sage::vars::EntityRef{owner})) : sol::object(sol::lua_nil);
        n["instance"] = AiVars{ex.GetWorld().FindAgent(ex.Task().Agent())
                                   ? ex.GetWorld().FindAgent(ex.Task().Agent())->Memory
                                   : std::make_shared<sage::ai::Blackboard>()};
        n["agent"] = ex.Task().Agent();
        auto r = fn(n);
        call->Ex = nullptr;
        if (!r.valid()) {
            sol::error e = r;
            ex.Error(e.what());
        }
        if (pure) return NodeResult::Go("");
        // Узел вернул имя выхода — идти по нему; ничего — по первому.
        if (r.return_count() > 0) {
            sol::object out = r.get<sol::object>(0);
            if (out.is<std::string>()) return NodeResult::Go(out.as<std::string>());
        }
        for (const PinSpec& p : ex.Pins().Outputs)
            if (p.Type == sage::ai::type::Exec) return NodeResult::Go(p.Name);
        return NodeResult::Go("");
    };
    if (sol::object c = spec["connect"]; c.get_type() == sol::type::function) {
        sol::protected_function check = c.as<sol::protected_function>();
        t.CanConnect = [check](const sage::ai::GraphNode&, const std::string& pin, const std::string& other,
                               std::string& why) {
            auto r = check(pin, other);
            if (!r.valid()) {
                sol::error e = r;
                why = e.what();
                return false;
            }
            sol::object ok = r.get<sol::object>(0);
            if (ok.is<bool>() && !ok.as<bool>()) {
                if (r.return_count() > 1 && r.get<sol::object>(1).is<std::string>())
                    why = r.get<std::string>(1);
                if (why.empty()) why = "узел не принимает такой провод";
                return false;
            }
            return true;
        };
    }
    AI().Nodes().Register(std::move(t));
}
