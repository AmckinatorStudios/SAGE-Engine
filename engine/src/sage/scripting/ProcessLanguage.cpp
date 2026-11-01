#include "sage/scripting/ProcessLanguage.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sage/core/ChildProcess.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/vars/ValueJson.h"

namespace sage::scripting {

using json = nlohmann::json;

namespace {

// Разговор с процессом языка: запуск по первому запросу, запрос — ответ,
// по дороге — вызовы API движка и строки журнала.
class Channel {
public:
    explicit Channel(ProcessLanguageSpec spec) : m_spec(std::move(spec)) {}

    // engine/scene — для вызовов API из процесса (nullptr — API недоступно).
    bool Request(const json& req, json& resp, ScriptError& err, ScriptEngine* engine, Scene* scene,
                 double timeout) {
        if (!EnsureStarted(err)) return false;
        if (!m_proc.WriteLine(req.dump())) {
            Fail(err, "процесс языка закрыл вход");
            return false;
        }
        for (;;) {
            std::string line;
            bool timedOut = false;
            if (!m_proc.ReadLine(line, timeout, &timedOut)) {
                Fail(err, timedOut ? "язык не ответил за " + std::to_string((int)(timeout * 1000)) +
                                         " мс — процесс остановлен"
                                   : "процесс языка завершился");
                err.What = timedOut ? ScriptError::Kind::Timeout : ScriptError::Kind::Runtime;
                return false;
            }
            json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) {
                // Не наш протокол — отладочная печать интерпретатора: в журнал,
                // а не в ошибку (print в Python пишет в stdout).
                LOG_INFO(m_spec.Name.c_str()) << line;
                continue;
            }
            if (msg.contains("log")) {
                LOG_INFO(m_spec.Name.c_str()) << msg.value("log", std::string());
                continue;
            }
            if (msg.contains("api")) {
                m_proc.WriteLine(CallApi(msg, engine, scene).dump());
                continue;
            }
            resp = std::move(msg);
            if (!resp.value("ok", true)) {
                err.What = ScriptError::Kind::Runtime;
                err.Message = resp.value("error", std::string("ошибка языка"));
                err.Line = resp.value("line", 0);
                return false;
            }
            return true;
        }
    }

    void Stop() { m_proc.Kill(); m_started = false; }
    const ProcessLanguageSpec& Spec() const { return m_spec; }

private:
    bool EnsureStarted(ScriptError& err) {
        if (m_started && m_proc.Running()) return true;
        std::vector<std::string> argv;
        const std::string project = sage::PathToUtf8(m_spec.ProjectDir);
        for (std::string a : m_spec.Command) {
            for (size_t p; (p = a.find("{project}")) != std::string::npos;) a.replace(p, 9, project);
            argv.push_back(a);
        }
        std::string why;
        if (!m_proc.Start(argv, project, why)) {
            err.What = ScriptError::Kind::Missing;
            err.Message = m_spec.Name + ": " + why;
            return false;
        }
        m_started = true;
        // Рукопожатие: процесс жив и говорит на этом протоколе.
        json hello;
        if (!m_proc.WriteLine(json{{"op", "hello"}, {"protocol", 1}}.dump())) {
            Fail(err, "процесс языка не принял приветствие");
            return false;
        }
        std::string line;
        bool timedOut = false;
        while (m_proc.ReadLine(line, m_spec.StartTimeout, &timedOut)) {
            hello = json::parse(line, nullptr, false);
            if (hello.is_object() && hello.contains("ok")) return true;
            LOG_INFO(m_spec.Name.c_str()) << line;
        }
        Fail(err, timedOut ? "язык не ответил на приветствие" : "процесс языка завершился при запуске");
        return false;
    }

    void Fail(ScriptError& err, const std::string& why) {
        err.What = ScriptError::Kind::Runtime;
        err.Message = m_spec.Name + ": " + why;
        Stop();
    }

    // Вызов API движка по просьбе процесса — через каталог и вызыватели
    // состояния скриптов, тем же путём, что у любого языка.
    json CallApi(const json& msg, ScriptEngine* engine, Scene* scene) {
        if (!engine) return json{{"error", "API движка не привязано"}};
        ApiCall call;
        call.ScenePtr = scene;
        for (const json& a : msg.value("args", json::array())) call.Args.push_back(sage::vars::ValueFromJson(a));
        if (!engine->Api().Call(msg.value("api", std::string()), call)) return json{{"error", call.Error}};
        json results = json::array();
        for (const sage::vars::Value& v : call.Results) results.push_back(sage::vars::ValueToJson(v));
        return json{{"results", results}};
    }

    ProcessLanguageSpec m_spec;
    ChildProcess m_proc;
    bool m_started = false;
};

class ProcessBackend : public LanguageBackend {
public:
    ProcessBackend(ProcessLanguageSpec spec, LanguageHost host) : m_channel(std::move(spec)), m_host(std::move(host)) {
        m_budget = m_channel.Spec().CallTimeout;
    }
    ~ProcessBackend() override { m_channel.Stop(); }

    const char* Name() const override { return m_channel.Spec().Name.c_str(); }
    bool Handles(const std::string& ext) const override {
        for (const std::string& e : m_channel.Spec().Extensions)
            if (e == ext) return true;
        return false;
    }
    void Bind(const ScriptServices& services) override { m_services = services; }

    sage::vars::Table ParseFields(const std::string& path, const std::string& source) const override {
        json resp;
        ScriptError err;
        sage::vars::Table out;
        if (!m_channel.Request(json{{"op", "fields"}, {"path", path}, {"source", source}}, resp, err, nullptr,
                               nullptr, m_budget))
            return out;
        // Копия, а не временный объект: items() держит ссылку на перебираемое.
        const json fields = resp.value("fields", json::object());
        for (const auto& [name, v] : fields.items()) out.Set(name, sage::vars::ValueFromJson(v));
        return out;
    }

    InstanceId Create(const ScriptSource& source, GameObject owner, ScriptError& err) override {
        const InstanceId id = m_next++;
        json resp;
        const json req{{"op", "create"}, {"id", id}, {"path", source.Path}, {"source", source.Text},
                       {"entity", owner.Valid() ? owner.Id() : 0}};
        if (!Ask(req, resp, err)) {
            if (err.File.empty()) err.File = source.Path;
            err.What = err.What == ScriptError::Kind::Runtime ? ScriptError::Kind::Compile : err.What;
            return kInvalidInstance;
        }
        Instance inst;
        for (const json& h : resp.value("hooks", json::array()))
            if (h.is_string()) inst.Hooks.insert(h.get<std::string>());
        for (const json& m : resp.value("methods", json::array()))
            if (m.is_string()) inst.Methods.insert(m.get<std::string>());
        m_instances[id] = std::move(inst);
        return id;
    }

    void Destroy(InstanceId id) override {
        if (!m_instances.erase(id)) return;
        json resp;
        ScriptError err;
        Ask(json{{"op", "destroy"}, {"id", id}}, resp, err);
    }

    bool Has(InstanceId id, Hook hook) const override {
        auto it = m_instances.find(id);
        return it != m_instances.end() && it->second.Hooks.count(HookName(hook));
    }

    bool Call(InstanceId id, Hook hook, float dt, ScriptError& err) override {
        json resp;
        return Ask(json{{"op", "call"}, {"id", id}, {"hook", HookName(hook)}, {"dt", dt}}, resp, err);
    }
    bool CallWith(InstanceId id, Hook hook, GameObject other, ScriptError& err) override {
        json resp;
        return Ask(json{{"op", "call"}, {"id", id}, {"hook", HookName(hook)}, {"other", other.Valid() ? other.Id() : 0}},
                   resp, err);
    }
    bool CallNamed(InstanceId id, Hook hook, const std::string& name, ScriptError& err) override {
        json resp;
        return Ask(json{{"op", "call"}, {"id", id}, {"hook", HookName(hook)}, {"name", name}}, resp, err);
    }

    using LanguageBackend::Invoke;
    bool Invoke(InstanceId id, const std::string& method, const std::vector<sage::vars::Value>& args,
                std::vector<sage::vars::Value>& results, ScriptError& err) override {
        json list = json::array();
        for (const sage::vars::Value& v : args) list.push_back(sage::vars::ValueToJson(v));
        json resp;
        if (!Ask(json{{"op", "invoke"}, {"id", id}, {"method", method}, {"args", list}}, resp, err)) return false;
        for (const json& r : resp.value("results", json::array())) results.push_back(sage::vars::ValueFromJson(r));
        return true;
    }
    bool HasMethod(InstanceId id, const std::string& method) const override {
        auto it = m_instances.find(id);
        return it != m_instances.end() && it->second.Methods.count(method);
    }
    bool InvokeEvent(InstanceId id, const std::string& method, const sage::events::Event& event,
                     ScriptError& err) override {
        json resp;
        return Ask(json{{"op", "invoke"}, {"id", id}, {"method", method}, {"event", event.Name},
                        {"args", json::array({sage::vars::ValueToJson(event.Arg)})}},
                   resp, err);
    }

    void ApplyFields(InstanceId id, const sage::vars::Table& fields) override {
        json f = json::object();
        for (const sage::vars::Var& v : fields.All()) f[v.Name] = sage::vars::ValueToJson(v.Data);
        json resp;
        ScriptError err;
        if (!Ask(json{{"op", "set_fields"}, {"id", id}, {"fields", f}}, resp, err))
            LOG_ERROR("Scripting") << err.Format();
    }

    void SetCallBudget(double seconds) override { m_budget = seconds > 0.0 ? seconds : m_channel.Spec().CallTimeout; }

    void Reset() override {
        json resp;
        ScriptError err;
        if (!m_instances.empty()) Ask(json{{"op", "reset"}}, resp, err);
        m_instances.clear();
    }

private:
    struct Instance {
        std::set<std::string> Hooks;
        std::set<std::string> Methods;
    };

    bool Ask(const json& req, json& resp, ScriptError& err) {
        return m_channel.Request(req, resp, err, m_host.Engine, m_services.ScenePtr, m_budget);
    }

    mutable Channel m_channel;
    LanguageHost m_host;
    ScriptServices m_services;
    std::map<InstanceId, Instance> m_instances;
    InstanceId m_next = 1;
    double m_budget = 2.0;
};

} // namespace

std::unique_ptr<LanguageBackend> MakeProcessBackend(const ProcessLanguageSpec& spec, const LanguageHost& host) {
    return std::make_unique<ProcessBackend>(spec, host);
}

Language MakeProcessLanguage(const ProcessLanguageSpec& spec) {
    Language l;
    l.Id = spec.Id;
    l.Name = spec.Name;
    l.Extensions = spec.Extensions;
    l.Origin = "project";
    l.CreateBackend = [spec](const LanguageHost& host) { return MakeProcessBackend(spec, host); };
    // Поля для инспектора — тем же процессом, без запуска скрипта: процесс
    // разбирает текст (протокол "fields"). Один процесс на все вопросы
    // инспектора, а не по процессу на каждый.
    auto tool = std::make_shared<ProcessBackend>(spec, LanguageHost{});
    l.Fields = [tool](const std::string& path, const std::string& source) { return tool->ParseFields(path, source); };
    return l;
}

int LoadProjectLanguages(const std::filesystem::path& projectDir, std::vector<std::string>* problems) {
    UnregisterLanguages("project");
    const std::filesystem::path file = projectDir / "scripting" / "languages.json";
    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    std::stringstream ss;
    ss << in.rdbuf();
    const json root = json::parse(ss.str(), nullptr, false);
    auto problem = [&](const std::string& p) {
        LOG_ERROR("Scripting") << "scripting/languages.json: " << p;
        if (problems) problems->push_back(p);
    };
    if (root.is_discarded() || !root.is_object()) {
        problem("не JSON");
        return 0;
    }
    int made = 0;
    for (const json& j : root.value("languages", json::array())) {
        ProcessLanguageSpec spec;
        spec.Id = j.value("id", std::string());
        spec.Name = j.value("name", spec.Id);
        spec.ProjectDir = projectDir;
        spec.CallTimeout = j.value("call_timeout", spec.CallTimeout);
        spec.StartTimeout = j.value("start_timeout", spec.StartTimeout);
        for (const json& e : j.value("extensions", json::array()))
            if (e.is_string()) spec.Extensions.push_back(e.get<std::string>());
        for (const json& c : j.value("command", json::array()))
            if (c.is_string()) spec.Command.push_back(c.get<std::string>());
        if (spec.Id.empty() || spec.Extensions.empty() || spec.Command.empty()) {
            problem("у языка нет id, расширений или команды");
            continue;
        }
        std::string err;
        if (!RegisterLanguage(MakeProcessLanguage(spec), &err)) {
            problem(spec.Name + ": " + err);
            continue;
        }
        ++made;
    }
    return made;
}

} // namespace sage::scripting
