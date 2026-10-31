// SAGE AI, ядро без языка: задачи, состояния, ожидания, дети, области
// переменных, операции (sage/ai/World.h). Задачи здесь на C++ — те же правила
// обязаны держаться и для Lua (test_ai_lua.cpp), и для графа.
#include "TestFramework.h"

#include <string>
#include <vector>

#include "sage/ai/World.h"

using namespace sage::ai;
using sage::vars::Value;

namespace {

// Задача «ждать N секунд, потом успех»: машина состояний на Phase.
TaskDefinitionPtr Sleeper() {
    TaskSignature sig;
    sig.Inputs = {{"seconds", Value(1.0f)}};
    sig.Outputs = {{"slept", Value(false)}};
    return MakeTask("Sleep", sig, [](TaskContext& t) {
        if (t.Phase()++ == 0) {
            t.WaitSeconds(t.Inputs().Get("seconds").AsFloat());
            return TaskStatus::Running;
        }
        t.Outputs().Set("slept", Value(true));
        return TaskStatus::Succeeded;
    });
}

} // namespace

TEST(AI_long_task_waits_and_is_not_restarted_each_frame) {
    World w;
    int steps = 0;
    w.RegisterTask(MakeTask("Count", {}, [&](TaskContext& t) {
        ++steps;
        if (t.Phase()++ == 0) {
            t.WaitSeconds(1.0f);
            return TaskStatus::Running;
        }
        return TaskStatus::Succeeded;
    }));
    const int a = w.CreateAgent();
    RunPtr r = w.Start(a, "Count");
    CHECK_TRUE(r->Status == TaskStatus::Running);
    for (int i = 0; i < 9; ++i) w.Tick(0.1f);   // 0.9 с — ещё ждёт
    CHECK_EQ(steps, 1);                          // первый шаг — и всё: ожидание не шагает
    CHECK_TRUE(r->Status == TaskStatus::Running);
    w.Tick(0.2f);
    CHECK_EQ(steps, 2);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_EQ((int)w.ActiveCount(), 0);
}

TEST(AI_inputs_outputs_and_defaults) {
    World w;
    w.RegisterTask(Sleeper());
    const int a = w.CreateAgent();
    Blackboard in;
    in.Set("seconds", Value(0));   // целое приводится к объявленному float
    RunPtr r = w.Start(a, "Sleep", in);
    CHECK_TRUE(r->Inputs->Find("seconds")->Type() == sage::vars::Kind::Float);
    w.Tick(0.016f);
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Succeeded);
    CHECK_TRUE(r->Outputs->Get("slept").AsBool());

    // Опечатка во входе — отказ запуска с именем, а не молчаливое умолчание.
    Blackboard bad;
    bad.Set("secnds", Value(1.0f));
    RunPtr x = w.Start(a, "Sleep", bad);
    CHECK_TRUE(x->Rejected);
    CHECK_TRUE(x->Status == TaskStatus::Failed);
    CHECK_TRUE(x->Error.find("secnds") != std::string::npos);

    RunPtr y = w.Start(a, "NoSuchTask");
    CHECK_TRUE(y->Rejected);
    CHECK_TRUE(y->Error.find("NoSuchTask") != std::string::npos);

    // Строгие выходы: необъявленное имя не пишется.
    Blackboard out;
    out.Declare("hp", Value(10));
    std::string err;
    CHECK_FALSE(out.Set("hpp", Value(1), &err));
    CHECK_TRUE(err.find("hpp") != std::string::npos);
}

TEST(AI_each_agent_has_independent_state) {
    World w;
    w.RegisterTask(Sleeper());
    const int a = w.CreateAgent();
    const int b = w.CreateAgent();
    Blackboard fast, slow;
    fast.Set("seconds", Value(0.1f));
    slow.Set("seconds", Value(1.0f));
    RunPtr ra = w.Start(a, "Sleep", fast);
    RunPtr rb = w.Start(b, "Sleep", slow);
    for (int i = 0; i < 3; ++i) w.Tick(0.1f);
    CHECK_TRUE(ra->Status == TaskStatus::Succeeded);
    CHECK_TRUE(rb->Status == TaskStatus::Running);

    w.FindAgent(a)->Memory->Set("target", Value(5));
    CHECK_FALSE(w.FindAgent(b)->Memory->Has("target"));
}

TEST(AI_shared_spaces_are_bounded_by_name) {
    World w;
    const int g1 = w.CreateAgent(0, "guards");
    const int g2 = w.CreateAgent(0, "guards");
    const int m = w.CreateAgent(0, "merchants");
    bool seen = false, foreign = true;
    w.RegisterTask(MakeTask("Raise", {}, [](TaskContext& t) {
        t.Shared().Set("alarm", Value(true));
        return TaskStatus::Succeeded;
    }));
    w.RegisterTask(MakeTask("Look", {}, [&](TaskContext& t) {
        if (t.Agent() == g2) seen = t.Shared().Get("alarm").AsBool();
        else foreign = t.Shared().Get("alarm").AsBool();
        return TaskStatus::Succeeded;
    }));
    w.Start(g1, "Raise");
    w.Tick(0.016f);
    w.Start(g2, "Look");
    w.Start(m, "Look");
    w.Tick(0.016f);
    CHECK_TRUE(seen);
    CHECK_FALSE(foreign);
}

TEST(AI_cancel_cascades_to_children_and_calls_cleanup) {
    World w;
    std::vector<std::string> cleaned;
    auto forever = [](TaskContext& t) {
        t.WaitEvent("never");
        return TaskStatus::Running;
    };
    w.RegisterTask(MakeTask("Leaf", {}, forever, [&](TaskContext& t) { cleaned.push_back("leaf"); }));
    RunPtr leaf;
    w.RegisterTask(MakeTask("Root", {}, [&](TaskContext& t) {
        if (t.Phase()++ == 0) leaf = t.Start("Leaf");
        t.WaitEvent("never");
        return TaskStatus::Running;
    }, [&](TaskContext&) { cleaned.push_back("root"); }));
    const int a = w.CreateAgent();
    RunPtr root = w.Start(a, "Root");
    w.Tick(0.016f);
    CHECK_TRUE(leaf && leaf->Status == TaskStatus::Running);
    CHECK_TRUE(w.Cancel(root->Id));
    CHECK_TRUE(root->Status == TaskStatus::Cancelled);
    CHECK_TRUE(leaf->Status == TaskStatus::Cancelled);
    // Внутреннее прибирается раньше внешнего.
    CHECK_EQ((int)cleaned.size(), 2);
    if (cleaned.size() == 2) {
        CHECK_TRUE(cleaned[0] == "leaf");
        CHECK_TRUE(cleaned[1] == "root");
    }
}

TEST(AI_children_do_not_outlive_parent_and_parallel_waits) {
    World w;
    w.RegisterTask(Sleeper());
    RunPtr fast, slow;
    bool anyFirst = false;
    w.RegisterTask(MakeTask("Race", {}, [&](TaskContext& t) {
        switch (t.Phase()++) {
            case 0: {
                Blackboard f, s;
                f.Set("seconds", Value(0.2f));
                s.Set("seconds", Value(5.0f));
                fast = t.Start("Sleep", f);
                slow = t.Start("Sleep", s);
                t.WaitRuns({fast, slow}, /*all=*/false);
                return TaskStatus::Running;
            }
            default:
                anyFirst = fast->Status == TaskStatus::Succeeded && slow->Status == TaskStatus::Running;
                return TaskStatus::Succeeded;   // медленного — снимет мир
        }
    }));
    const int a = w.CreateAgent();
    RunPtr race = w.Start(a, "Race");
    for (int i = 0; i < 5; ++i) w.Tick(0.1f);
    CHECK_TRUE(race->Status == TaskStatus::Succeeded);
    CHECK_TRUE(anyFirst);
    CHECK_TRUE(slow->Status == TaskStatus::Cancelled);
}

TEST(AI_instant_child_continues_parent_in_same_frame_with_cap) {
    World w;
    w.RegisterTask(MakeTask("Instant", {}, [](TaskContext&) { return TaskStatus::Succeeded; }));
    int rounds = 0;
    w.RegisterTask(MakeTask("Loop", {}, [&](TaskContext& t) {
        ++rounds;
        t.WaitRuns({t.Start("Instant")});
        return TaskStatus::Running;
    }));
    const int a = w.CreateAgent();
    w.Start(a, "Loop");
    w.Tick(0.016f);
    // Мгновенная дочерняя задача не стоит кадра, но бесконечный цикл таких
    // упирается в предел и не вешает игру.
    CHECK_EQ(rounds, World::kMaxStepsPerFrame);
    w.Tick(0.016f);
    CHECK_EQ(rounds, 2 * World::kMaxStepsPerFrame);
}

TEST(AI_events_wake_only_waiters_with_payload_and_timeout) {
    World w;
    int heard = 0;
    bool timedOut = false;
    w.RegisterTask(MakeTask("Listen", {}, [&](TaskContext& t) {
        if (t.Phase()++ == 0) {
            t.WaitEvent("noise", 1.0f);
            return TaskStatus::Running;
        }
        if (t.GetRun().TimedOut) timedOut = true;
        else heard = t.GetRun().EventPayload.AsInt();
        return TaskStatus::Succeeded;
    }));
    const int a = w.CreateAgent();
    const int b = w.CreateAgent();
    RunPtr ra = w.Start(a, "Listen");
    RunPtr rb = w.Start(b, "Listen");
    w.Tick(0.016f);
    CHECK_EQ(w.Emit("noise", Value(7), a), 1);   // только экземпляру a
    w.Tick(0.016f);
    CHECK_TRUE(ra->Status == TaskStatus::Succeeded);
    CHECK_EQ(heard, 7);
    CHECK_TRUE(rb->Status == TaskStatus::Running);
    w.Tick(1.0f);
    CHECK_TRUE(rb->Status == TaskStatus::Succeeded);
    CHECK_TRUE(timedOut);
    CHECK_EQ(w.Emit("noise"), 0);   // ждущих нет — событие никого не будит
}

TEST(AI_condition_wait_and_failure_reason) {
    World w;
    bool open = false;
    w.RegisterTask(MakeTask("Door", {}, [&](TaskContext& t) {
        if (t.Phase()++ == 0) {
            t.WaitUntil([&] { return open; });
            return TaskStatus::Running;
        }
        return t.Fail("дверь заперта");
    }));
    const int a = w.CreateAgent();
    RunPtr r = w.Start(a, "Door");
    w.Tick(0.016f);
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Running);
    open = true;
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Failed);
    CHECK_TRUE(r->Error == "дверь заперта");
}

TEST(AI_cancel_from_own_step_is_deferred_and_owner_death_stops_agent) {
    World w;
    int cleanups = 0;
    w.RegisterTask(MakeTask("Quit", {}, [&](TaskContext& t) {
        t.GetWorld().StopAgent(t.Agent());   // отмена самого себя посреди шага
        return TaskStatus::Running;
    }, [&](TaskContext&) { ++cleanups; }));
    const int a = w.CreateAgent();
    RunPtr r = w.Start(a, "Quit");
    w.Tick(0.016f);
    CHECK_TRUE(r->Status == TaskStatus::Cancelled);
    CHECK_EQ(cleanups, 1);

    bool alive = true;
    w.SetOwnerAlive([&](int) { return alive; });
    w.RegisterTask(Sleeper());
    const int owned = w.CreateAgent(42);
    RunPtr s = w.Start(owned, "Sleep");
    w.Tick(0.016f);
    alive = false;
    w.Tick(0.016f);
    CHECK_TRUE(s->Status == TaskStatus::Cancelled);
    CHECK_TRUE(w.FindAgent(owned) == nullptr);
}

TEST(AI_operations_check_signature) {
    World w;
    Operation add;
    add.Name = "add";
    add.Inputs = {{"a", Value(0.0f)}, {"b", Value(0.0f)}};
    add.Outputs = {{"sum", Value(0.0f)}};
    add.Fn = [](const Values& in, Values& out, std::string&) {
        out.push_back(Value(in[0].AsFloat() + in[1].AsFloat()));
        return true;
    };
    w.RegisterOperation(add);
    Values out;
    std::string err;
    CHECK_TRUE(w.Call("add", {Value(2), Value(0.5f)}, out, err));
    CHECK_EQ((int)out.size(), 1);
    if (!out.empty()) CHECK_NEAR(out[0].AsFloat(), 2.5f, 1e-5f);
    CHECK_TRUE(w.Call("add", {Value(1.0f)}, out, err));   // недостающее — умолчанием
    CHECK_FALSE(w.Call("add", {Value(1), Value(2), Value(3)}, out, err));
    CHECK_FALSE(w.Call("nope", {}, out, err));
    CHECK_TRUE(err.find("nope") != std::string::npos);
}
