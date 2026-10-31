#include "SelfTestCommon.h"

#include <fstream>

#include "sage/ai/GraphValidate.h"
#include "sage/ai/World.h"
#include "sage/scene/VisualScriptComponent.h"

// VISUAL SCRIPTING в редакторе: свой редактор с общей основой, свой набор
// узлов (задач ИИ в палитре нет, On Update есть), проверка графа, граф
// открывается из assets/visual, а в Play компонент Visual Script ведёт объект.
bool EditorLayer::SelfTestVisualScript() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: visual-script: " << why;
        ok = false;
    };
    const fs::path dir = m_project.AssetsDir() / "visual";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // On Update: сдвигать свой объект на (dt, 0, 0) — по секунде за секунду.
    sage::ai::GraphDoc d;
    d.Kind = sage::ai::GraphKind::Script;
    d.Name = "SelftestMover";
    const int upd = d.AddNode("script.on_update", 0, 0).Id;
    const int self = d.AddNode("world.self", 0, 150).Id;
    const int info = d.AddNode("object.info", 200, 150).Id;
    const int step = d.AddNode("vec.make3", 200, 300).Id;
    const int sum = d.AddNode("math.add", 400, 200).Id;
    const int move = d.AddNode("object.set_position", 600, 0).Id;
    d.Link({upd, "then"}, {move, "in"}, true);
    d.Link({self, "object"}, {info, "object"}, false);
    d.Link({upd, "dt"}, {step, "x"}, false);
    d.Link({info, "position"}, {sum, "a"}, false);
    d.Link({step, "vector"}, {sum, "b"}, false);
    d.Link({sum, "result"}, {move, "position"}, false);
    d.Link({self, "object"}, {move, "object"}, false);
    const fs::path path = dir / "SelftestMover.vgraph";
    std::ofstream(path, std::ios::binary) << sage::ai::SaveGraph(d);

    m_visualScript.ReloadLibrary(*this);
    sage::ai::World* lib = m_visualScript.Library();
    if (!lib) fail("нет библиотеки у редактора Visual Script");
    else {
        if (sage::ai::NodeAllowed(lib->Nodes().Find("task.run")->Domains, sage::ai::GraphKind::Script))
            fail("узел задачи ИИ предлагается в Visual Scripting");
        if (!sage::ai::NodeAllowed(lib->Nodes().Find("flow.branch")->Domains, sage::ai::GraphKind::Script))
            fail("общего узла Branch нет в Visual Scripting");
        for (const sage::ai::GraphIssue& i : sage::ai::ValidateGraph(d, *lib))
            if (i.Error) fail("проверка нашла ошибку в верном графе: " + i.Text);
    }
    OpenVisualScript(sage::PathToUtf8(path));
    if (m_visualScript.Doc().Name != "SelftestMover") fail("скрипт не открылся в редакторе Visual Script");
    if (m_aiGraph.Doc().Name == "SelftestMover") fail("скрипт открылся в редакторе ИИ");

    GameObject box = m_scene->CreateObject("VS Probe");
    const int boxId = box.Id();
    VisualScriptComponent c;
    c.Graph = "assets/visual/SelftestMover.vgraph";
    m_scene->Registry().emplace<VisualScriptComponent>(box.Entity(), c);
    StartPlay();
    for (int i = 0; i < 6; ++i) m_systems.Run(*m_scene, 0.1f);
    GameObject live = m_scene->Get(boxId);
    if (!live.Valid() || live.GetTransform().Position.x < 0.3f)
        fail("Visual Script в Play не двигал объект (компонент не запустил граф?)");
    StopPlay();
    if (GameObject o = m_scene->Get(boxId); o.Valid()) m_scene->RemoveObject(boxId);
    fs::remove(path, ec);
    m_panelRegistry.SetVisible("VisualScript", false);
    if (ok) LOG_INFO("Editor") << "SELFTEST: visual-script OK";
    return ok;
}
