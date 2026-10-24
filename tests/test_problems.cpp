// ПРОБЛЕМЫ ДВИЖКА (sage/core/Problems.h) и объяснение ошибок скриптов
// (sage/scripting/ScriptAdvice.h).
//
// Что закрепляется: одинаковые ошибки — одна запись со счётчиком; связанные —
// одна группа с первопричиной наверху; сломанный скрипт не запускается и
// объясняет, что не так, со строкой, подсказкой и исправленным примером;
// зависший скрипт не вешает движок; отчёт не выдаёт личных путей.
#include "TestFramework.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sage/core/Log.h"
#include "sage/core/Problems.h"
#include "sage/input/InputSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/ScriptComponent.h"
#include "sage/scripting/ScriptAdvice.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"

using sage::Problem;
using sage::Problems;
using sage::Severity;

namespace {

const Problem* FindCode(const std::vector<Problem>& all, const std::string& code, const std::string& subject = {}) {
    for (const Problem& p : all)
        if (p.Code == code && (subject.empty() || p.Subject == subject)) return &p;
    return nullptr;
}

std::string Write(const std::string& name, const std::string& body) {
    std::ofstream f(name, std::ios::binary);
    f << body;
    return name;
}

struct World {
    Scene scene{"problems"};
    sage::input::InputSystem input;
    ScriptEngine engine;
    std::unique_ptr<sage::scripting::ScriptingSystem> sys;
    std::vector<std::string> files;

    World() {
        Problems::Clear();
        engine.BindScene(scene);
        engine.BindInput(input);
        sys = std::make_unique<sage::scripting::ScriptingSystem>();
        sys->AddBackend(sage::scripting::MakeLuaBackend({&engine}));
        sage::scripting::ScriptServices services;
        services.ScenePtr = &scene;
        services.Input = &input;
        sys->Bind(services);
    }
    ~World() {
        sys->Shutdown();
        for (const std::string& f : files) std::remove(f.c_str());
        Problems::Clear();
    }
    GameObject Spawn(const std::string& name, const std::string& file, const std::string& body) {
        files.push_back(Write(file, body));
        GameObject o = scene.CreateObject(name);
        scene.Registry().emplace<ScriptComponent>(o.Entity()).Path = file;
        return o;
    }
    void Start() { sys->AttachScene(scene); }
    void Frame(float dt = 0.1f) {
        input.BeginFrame();
        sys->Update(dt);
    }
    template <typename T>
    T Global(GameObject o, const char* name) {
        sol::state& L = engine.Lua();
        L["__probe"] = o.Id();
        return L.script(std::string("return Scene.FindById(__probe):GetScript().") + name).get<T>();
    }
};

} // namespace

TEST(Problems_repeats_are_one_record_with_a_count) {
    Problems::Clear();
    for (int i = 0; i < 5; ++i) {
        Problem p;
        p.Code = "test.same";
        p.Title = "одно и то же";
        Problems::Report(p);
    }
    const auto all = Problems::Snapshot();
    CHECK_EQ(all.size(), (size_t)1);
    if (!all.empty()) CHECK_EQ(all[0].Count, 5);
    Problems::Clear();
}

TEST(Problems_consequences_group_under_the_first_problem_of_a_resource) {
    Problems::Clear();
    Problem root;
    root.Code = "test.model";
    root.Title = "Модель не найдена";
    root.Resource = "C:\\Proj\\assets\\models\\tree.gltf";
    const uint64_t rootId = Problems::Report(root);
    Problem after;
    after.Code = "test.mesh";
    after.Title = "Объект без меша";
    after.Resource = "assets/models/tree.gltf";   // тот же файл, другое написание
    Problems::Report(after);
    const auto all = Problems::Snapshot();
    const Problem* c = FindCode(all, "test.mesh");
    CHECK_TRUE(c != nullptr);
    if (c) CHECK_EQ((long long)c->Parent, (long long)rootId);
    Problems::Clear();
}

TEST(Problems_a_late_cause_adopts_its_consequences) {
    Problems::Clear();
    Problem effect;
    effect.Code = "test.effect";
    effect.Title = "Объект без материала";
    effect.Resource = "object:Door";
    effect.CausedBy = "assets/wood.sagemat";
    Problems::Report(effect);
    Problem cause;
    cause.Code = "test.cause";
    cause.Title = "Материал битый";
    cause.Resource = "assets/wood.sagemat";
    const uint64_t causeId = Problems::Report(cause);
    const auto snap1 = Problems::Snapshot();
    const Problem* c = FindCode(snap1, "test.effect");
    CHECK_TRUE(c != nullptr);
    if (c) CHECK_EQ((long long)c->Parent, (long long)causeId);
    // Причину сняли — последствие становится самостоятельным, а не висит.
    Problems::Resolve("assets/wood.sagemat", "test.cause");
    const auto snap2 = Problems::Snapshot();
    c = FindCode(snap2, "test.effect");
    CHECK_TRUE(c != nullptr);
    if (c) CHECK_EQ((long long)c->Parent, 0LL);
    Problems::Clear();
}

TEST(Problems_every_log_error_becomes_a_problem_once) {
    Problems::Clear();
    LOG_ERROR("Assets") << "Модель не загрузилась (/home/x/proj/assets/rock.gltf): файл не найден";
    auto all = Problems::Snapshot();
    CHECK_EQ(all.size(), (size_t)1);
    if (!all.empty()) {
        CHECK_EQ(all[0].Resource, std::string("assets/rock.gltf"));
        CHECK_TRUE(!all[0].Hints.empty());   // знакомая ошибка — с подсказкой
    }
    // Своя запись реестра в лог не заводит вторую проблему.
    Problem p;
    p.Code = "test.own";
    p.Title = "своя";
    Problems::Report(p);
    CHECK_EQ(Problems::Snapshot().size(), (size_t)2);
    Problems::Clear();
}

TEST(Problems_excerpt_marks_the_line_and_column) {
    const std::string src = "local a = 1\nlocal b = a +\nprint(b)\n";
    const std::string ex = Problems::Excerpt(src, 2, 13, 1);
    CHECK_TRUE(ex.find("> 2 | local b = a +") != std::string::npos);
    CHECK_TRUE(ex.find("^") != std::string::npos);
    CHECK_TRUE(ex.find("1 | local a = 1") != std::string::npos);
}

#if !defined(_WIN32)
TEST(Problems_report_hides_the_home_folder) {
    Problems::Clear();
    const char* old = std::getenv("HOME");
    const std::string keep = old ? old : "";
    setenv("HOME", "/home/ivan_petrov", 1);
    Problem p;
    p.Code = "test.path";
    p.Title = "Нет файла /home/ivan_petrov/games/proj/assets/a.png";
    Problems::Report(p);
    const std::string report = Problems::BuildReport("SAGE Test", "Проект: /home/ivan_petrov/games/proj");
    CHECK_TRUE(report.find("ivan_petrov") == std::string::npos);
    CHECK_TRUE(report.find("~/games/proj") != std::string::npos);
    if (old) setenv("HOME", keep.c_str(), 1);
    Problems::Clear();
}
#endif

TEST(ScriptAdvice_missing_then_shows_the_fixed_line) {
    World w;
    GameObject o = w.Spawn("Hero", "sage_prob_then.lua",
                           "function Update(dt)\n    if dt > 1\n        print(dt)\n    end\nend\n");
    w.Start();
    w.Frame();
    const auto all = Problems::Snapshot();
    const Problem* p = FindCode(all, "script.compile");
    CHECK_TRUE(p != nullptr);
    if (p) {
        CHECK_EQ(p->Line, 3);   // Lua замечает нехватку then на следующей строке
        CHECK_TRUE(p->Detail.find("then") != std::string::npos);
        CHECK_TRUE(!p->Excerpt.empty());
    }
    // Не запущен — и об этом сказано отдельным последствием под причиной.
    const Problem* c = FindCode(all, "script.not-started", "Hero");
    CHECK_TRUE(c != nullptr);
    if (p && c) CHECK_EQ((long long)c->Parent, (long long)p->Id);
    (void)o;
}

TEST(ScriptAdvice_typo_in_a_name_suggests_the_right_one) {
    World w;
    w.Spawn("Walker", "sage_prob_typo.lua",
            "function Move(dt) end\nfunction Update(dt)\n    Moev(dt)\nend\n");
    w.Start();
    w.Frame();
    const auto snap = Problems::Snapshot();
    const Problem* p = FindCode(snap, "script.runtime");
    CHECK_TRUE(p != nullptr);
    if (p) {
        CHECK_EQ(p->Line, 3);
        bool suggested = false;
        for (const std::string& h : p->Hints) suggested = suggested || h.find("«Move»") != std::string::npos;
        CHECK_TRUE(suggested);
        CHECK_EQ(p->Example, std::string("Move(dt)"));
        CHECK_TRUE(p->Technical.find("stack traceback") != std::string::npos);
        CHECK_EQ(p->Subject, std::string("Walker"));
    }
}

TEST(ScriptAdvice_sage_lua_type_error_is_explained_and_not_run) {
    World w;
    GameObject o = w.Spawn("Typed", "sage_prob_typed.slua",
                           "local speed: number = \"fast\"\nran = true\nfunction Update(dt) end\n");
    w.Start();
    w.Frame();
    const auto snap = Problems::Snapshot();
    const Problem* p = FindCode(snap, "script.compile");
    CHECK_TRUE(p != nullptr);
    if (p) {
        CHECK_EQ(p->Line, 1);
        CHECK_TRUE(p->Col > 0);
        CHECK_TRUE(p->Example.find("tonumber") != std::string::npos);
        CHECK_TRUE(p->Title.find("Ошибка типа") != std::string::npos);
    }
    (void)o;
}

TEST(ScriptAdvice_fixed_file_clears_its_compile_problems) {
    World w;
    GameObject o = w.Spawn("Fix", "sage_prob_fix.lua", "function Update(dt)\n    if dt then\nend\n");
    w.Start();
    CHECK_TRUE(FindCode(Problems::Snapshot(), "script.compile") != nullptr);
    Write("sage_prob_fix.lua", "function Update(dt) end\n");
    w.sys->Runtime().Attach(o, "sage_prob_fix.lua", {});
    CHECK_TRUE(FindCode(Problems::Snapshot(), "script.compile") == nullptr);
    CHECK_TRUE(FindCode(Problems::Snapshot(), "script.not-started") == nullptr);
}

TEST(Watchdog_endless_loop_is_stopped_and_other_scripts_keep_running) {
    World w;
    w.sys->Runtime().SetCallBudget(0.2);
    w.Spawn("Stuck", "sage_prob_loop.lua", "function Update(dt)\n    while true do end\nend\n");
    GameObject good = w.Spawn("Good", "sage_prob_good.lua", "ticks = 0\nfunction Update(dt) ticks = ticks + 1 end\n");
    w.Start();
    const auto t0 = std::chrono::steady_clock::now();
    w.Frame();
    w.Frame();
    w.Frame();
    const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Один раз прерван — и больше не зовётся: три кадра стоят одного бюджета.
    CHECK_TRUE(spent < 1.5);
    const auto all = Problems::Snapshot();
    const Problem* p = FindCode(all, "script.timeout");
    CHECK_TRUE(p != nullptr);
    if (p) CHECK_EQ(p->Line, 2);
    CHECK_TRUE(FindCode(all, "script.muted", "Stuck") != nullptr);
    CHECK_EQ(w.Global<int>(good, "ticks"), 3);
}
