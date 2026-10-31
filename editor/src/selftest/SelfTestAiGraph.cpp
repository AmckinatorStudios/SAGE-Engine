#include "SelfTestCommon.h"

#include <fstream>

#include "sage/ai/GraphDebug.h"
#include "sage/ai/World.h"
#include "sage/scene/AiAgentComponent.h"

// SAGE AI GRAPH в редакторе: библиотека проекта (узел и задача на Lua из
// assets/ai) видна редактору графов, граф открывается из файла, а в Play
// компонент AI объекта запускает его — граф зовёт Lua-задачу и узел из Lua,
// двигает объект, отладчик видит выполнение графа и его узлы.
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
    // Граф: Start -> Run Task SelftestMark(v = double(21)) -> Set Position(self, (3, 0, 0)).
    sage::ai::GraphDoc d;
    d.Kind = sage::ai::GraphKind::Behavior;
    d.Name = "SelftestBrain";
    const int start = d.AddNode("flow.start", 0, 0).Id;
    const int dbl = d.AddNode("selftest.double", 0, 150).Id;
    d.FindNode(dbl)->Values["x"] = sage::vars::Value(21.0f);
    const int run = d.AddNode("task.run", 250, 0).Id;
    d.FindNode(run)->Values["task"] = sage::vars::Value(std::string("SelftestMark"));
    const int self = d.AddNode("world.self", 250, 200).Id;
    const int move = d.AddNode("object.set_position", 500, 0).Id;
    d.FindNode(move)->Values["position"] = sage::vars::Value(glm::vec3(3.0f, 0.0f, 0.0f));
    d.Link({start, "then"}, {run, "in"}, true);
    d.Link({dbl, "y"}, {run, "v"}, false);
    d.Link({run, "succeeded"}, {move, "in"}, true);
    d.Link({self, "object"}, {move, "object"}, false);
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
    }
    OpenAiGraph(sage::PathToUtf8(graphPath));
    if (m_aiGraph.Doc().Name != "SelftestBrain" || m_aiGraph.Doc().Nodes.size() != 5)
        fail("граф не открылся в редакторе графов");

    GameObject probe = m_scene->CreateObject("AI Probe");
    const int probeId = probe.Id();
    AiAgentComponent c;
    c.Graph = "assets/ai/SelftestBrain.aigraph";
    m_scene->Registry().emplace<AiAgentComponent>(probe.Entity(), c);
    StartPlay();
    for (int i = 0; i < 5; ++i) m_systems.Run(*m_scene, 0.1f);
    GameObject live = m_scene->Get(probeId);
    if (!live.Valid() || std::abs(live.GetTransform().Position.x - 3.0f) > 1e-3f)
        fail("граф в Play не сдвинул объект (компонент AI не запустил граф?)");
    if (ScriptEngine* se = PlayScripts()) {
        const int agent = se->AI().AgentOf(probeId);
        sage::ai::Agent* a = se->AI().FindAgent(agent);
        if (!a || a->Memory->Get("mark").AsInt() != 42) fail("граф не позвал Lua-задачу с выходом Lua-узла");
    } else {
        fail("нет движка скриптов в Play");
    }
    bool seen = false;
    for (const auto& [id, r] : sage::ai::GraphDebugger::Get().Runs())
        if (r.Graph == "SelftestBrain" && !r.NodeHits.empty() && r.Status == "succeeded") seen = true;
    if (!seen) fail("отладчик не видел выполнения графа");
    StopPlay();
    if (GameObject o = m_scene->Get(probeId); o.Valid()) m_scene->RemoveObject(probeId);
    fs::remove(lua, ec);
    fs::remove(graphPath, ec);
    m_aiGraph.ReloadLibrary(*this);
    // Окно графа открыл OpenAiGraph — закрыть: плавающее окно поверх вьюпорта
    // перехватывало бы щелчки следующих проверок (VIEWPORT_INPUT).
    m_panelRegistry.SetVisible("AiGraph", false);
    if (ok) LOG_INFO("Editor") << "SELFTEST: ai-graph OK";
    return ok;
}
