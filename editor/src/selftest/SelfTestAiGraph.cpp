#include "SelfTestCommon.h"

#include <fstream>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/GraphValidate.h"
#include "sage/ai/World.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/VisualScriptComponent.h"

// AI GRAPH в редакторе: библиотека проекта (узел и задача на Lua из
// assets/ai) видна редактору, граф открывается из файла, а в Play компонент AI
// объекта запускает его: Task Start -> Run Task (Lua-задача со входом от
// Lua-узла) -> Move To Target (кадр за кадром, контроллером персонажа) ->
// Succeed. Отладчик видит выполнение, его узлы и итог задач. Script Graph,
// открытый из окна AI Graph, уходит в свой редактор; узлы игровой логики
// палитре AI Graph недоступны.
bool EditorLayer::SelfTestAiGraph() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: ai-graph: " << why;
        ok = false;
    };
    const fs::path dir = m_project.AssetsDir() / "ai";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path lua = dir / "selftest_ai.lua";
    {
        std::ofstream f(lua, std::ios::binary);
        f << "ai.node('selftest.double', {pure = true, inputs = {{'x', 'float', 1}}, outputs = {{'y', 'float'}},\n"
             "    run = function(n) n.outputs.y = n.inputs.x * 2 end})\n"
             "ai.task('SelftestMark', {inputs = {v = 0}, run = function(t) t.instance.mark = t.inputs.v; return true end})\n";
    }
    // Script Graph рядом — для проверки, что он не откроется в AI Graph.
    const fs::path vdir = m_project.AssetsDir() / "visual";
    fs::create_directories(vdir, ec);
    sage::ai::GraphDoc sg;
    sg.Kind = sage::ai::GraphKind::Script;
    sg.Name = "SelftestLogic";
    sg.AddNode("script.on_start", 0, 0);
    const fs::path sgPath = vdir / "SelftestLogic.sgraph";
    {
        std::ofstream f(sgPath, std::ios::binary);
        f << sage::ai::SaveGraph(sg);
    }
    sage::ai::GraphDoc d;
    d.Kind = sage::ai::GraphKind::Behavior;
    d.Name = "SelftestBrain";
    const int start = d.AddNode("ai.task_start", 0, 0).Id;
    const int dbl = d.AddNode("selftest.double", 0, 150).Id;
    d.FindNode(dbl)->Values["x"] = sage::vars::Value(21.0f);
    const int run = d.AddNode("task.run", 250, 0).Id;
    d.FindNode(run)->Values["task"] = sage::vars::Value(std::string("SelftestMark"));
    const int move = d.AddNode("ai.move_to", 500, 0).Id;
    d.FindNode(move)->Values["destination"] = sage::vars::Value(std::string("point"));
    d.FindNode(move)->Values["point"] = sage::vars::Value(glm::vec3(3.0f, 0.0f, 0.0f));
    d.FindNode(move)->Values["speed"] = sage::vars::Value(100.0f);
    d.FindNode(move)->Values["arrive_distance"] = sage::vars::Value(0.01f);
    const int done = d.AddNode("ai.succeed", 750, 0).Id;
    d.Link({start, "then"}, {run, "in"}, true);
    d.Link({dbl, "y"}, {run, "v"}, false);
    d.Link({run, "succeeded"}, {move, "in"}, true);
    d.Link({move, "success"}, {done, "in"}, true);
    const fs::path graphPath = dir / "SelftestBrain.aigraph";
    {
        std::ofstream f(graphPath, std::ios::binary);
        f << sage::ai::SaveGraph(d);
    }

    m_aiGraph.ReloadLibrary(*this);
    sage::ai::World* lib = m_aiGraph.Library();
    if (!lib) fail("нет библиотеки ИИ у редактора графов");
    else {
        if (!lib->Nodes().Find("selftest.double")) fail("узел из Lua библиотеки проекта не виден палитре");
        if (!lib->FindTask("SelftestMark")) fail("Lua-задача библиотеки не видна редактору");
        if (!lib->Nodes().Find("api.scene.Find")) fail("нет узлов API движка");
        if (!lib->FindGraph("SelftestBrain")) fail("граф проекта не попал в библиотеку");
        // Строгий набор узлов: игровой логике в AI Graph не место.
        if (const sage::ai::NodeType* b = lib->Nodes().Find("flow.branch");
            b && sage::ai::NodeAllowed(b->Domains, sage::ai::GraphKind::Behavior))
            fail("узел Branch (Script Graph) доступен AI Graph");
        if (auto bad = lib->FindGraph("SelftestBrain"))
            for (const auto& issue : sage::ai::ValidateGraph(*bad, *lib))
                if (issue.Error) fail("граф решений с ошибкой: " + issue.Text);
    }
    // Script Graph из окна AI Graph уходит в свой редактор.
    m_aiGraph.Open(*this, sage::PathToUtf8(sgPath));
    if (m_visualScript.Doc().Name != "SelftestLogic") fail("Script Graph не открылся в своём редакторе");
    if (m_aiGraph.Doc().Name == "SelftestLogic") fail("Script Graph открылся в AI Graph");
    m_panelRegistry.SetVisible("VisualScript", false);
    OpenAiGraph(sage::PathToUtf8(graphPath));
    if (m_aiGraph.Doc().Name != "SelftestBrain" || m_aiGraph.Doc().Nodes.size() != 5)
        fail("граф не открылся в редакторе графов");

    GameObject probe = m_scene->CreateObject("AI Probe");
    const int probeId = probe.Id();
    AiAgentComponent c;
    c.Graph = "assets/ai/SelftestBrain.aigraph";
    m_scene->Registry().emplace<AiAgentComponent>(probe.Entity(), c);
    StartPlay();
    for (int i = 0; i < 8; ++i) m_systems.Run(*m_scene, 0.1f);
    GameObject live = m_scene->Get(probeId);
    if (!live.Valid() || std::abs(live.GetTransform().Position.x - 3.0f) > 1e-3f)
        fail("AI Graph в Play не довёл объект (Move To Target, компонент AI)");
    if (ScriptEngine* se = PlayScripts()) {
        const int agent = se->AI().AgentOf(probeId);
        sage::ai::Agent* a = se->AI().FindAgent(agent);
        if (!a || a->Memory->Get("mark").AsInt() != 42) fail("граф не позвал Lua-задачу с выходом Lua-узла");
    } else {
        fail("нет движка скриптов в Play");
    }
    bool seen = false, outcome = false;
    for (const auto& [id, r] : sage::ai::GraphDebugger::Get().Runs())
        if (r.Graph == "SelftestBrain" && !r.NodeHits.empty() && r.Status == "succeeded") {
            seen = true;
            auto oc = r.Outcomes.find(move);
            outcome = oc != r.Outcomes.end() && oc->second == "succeeded";
        }
    if (!seen) fail("отладчик не видел выполнения графа");
    if (!outcome) fail("отладчик не знает итога задачи Move To Target");
    StopPlay();
    // ПЕРЕТАСКИВАНИЕ НА ОБЪЕКТ: AI Graph -> компонент AI, Script Graph ->
    // компонент Script Graph (тот же путь, что бросок в иерархии и во вьюпорт).
    {
        GameObject target = m_scene->CreateObject("Drop Target");
        const int tid = target.Id();
        if (!ApplyAssetToEntity(tid, graphPath)) fail("AI Graph не принимается броском на объект");
        if (!ApplyAssetToEntity(tid, sgPath)) fail("Script Graph не принимается броском на объект");
        GameObject t2 = m_scene->Get(tid);
        const auto* ai = t2.Valid() ? m_scene->Registry().try_get<AiAgentComponent>(t2.Entity()) : nullptr;
        const auto* vs = t2.Valid() ? m_scene->Registry().try_get<VisualScriptComponent>(t2.Entity()) : nullptr;
        if (!ai || ai->Graph.find("SelftestBrain.aigraph") == std::string::npos)
            fail("бросок AI Graph не поставил компонент AI");
        if (!vs || vs->Graph.find("SelftestLogic.sgraph") == std::string::npos)
            fail("бросок Script Graph не поставил компонент Script Graph");
        m_scene->RemoveObject(tid);
    }
    if (GameObject o = m_scene->Get(probeId); o.Valid()) m_scene->RemoveObject(probeId);
    fs::remove(lua, ec);
    fs::remove(graphPath, ec);
    fs::remove(sgPath, ec);
    m_aiGraph.ReloadLibrary(*this);
    // Окно графа открыл OpenAiGraph — закрыть: плавающее окно поверх вьюпорта
    // перехватывало бы щелчки следующих проверок (VIEWPORT_INPUT).
    m_panelRegistry.SetVisible("AiGraph", false);
    if (ok) LOG_INFO("Editor") << "SELFTEST: ai-graph OK";
    return ok;
}
