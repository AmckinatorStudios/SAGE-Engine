#include "SelfTestCommon.h"

#include <fstream>

#include "sage/ai/GraphDebug.h"
#include "sage/scene/AiAgentComponent.h"

// ОКНА SAGE AI GRAPH В НАСТОЯЩЕМ КАДРЕ. Самопроверка выше гоняет граф без
// кадра; здесь окно графа и отладчик РИСУЮТСЯ несколько кадров идущего Play:
// холст с подсветкой выполнения, палитра типов, дерево задач. Ошибка ImGui
// (непарный Push/Pop, список без EndCombo) или исключение в отрисовке видны
// как новые ошибки консоли.
void EditorLayer::CheckAiGraphFrame() {
    if (m_aiGraphFrameStep < 0 || !std::getenv("SAGE_EDITOR_SELFTEST") || !m_tweenChecked) return;
    const fs::path dir = m_project.AssetsDir() / "ai";
    const fs::path graphPath = dir / "SelftestFrame.aigraph";
    auto done = [&](bool ok, const std::string& why) {
        if (m_play.Active()) StopPlay();
        if (GameObject o = m_scene->FindByName("AI Frame Probe"); o.Valid()) m_scene->RemoveObject(o.Id());
        std::error_code ec;
        fs::remove(graphPath, ec);
        m_panelRegistry.SetVisible("AiGraph", false);
        m_panelRegistry.SetVisible("AiDebugger", false);
        m_panelRegistry.SetVisible("VisualScript", false);
        if (ok) LOG_INFO("Editor") << "AI_GRAPH: OK — окно графа и отладчик ИИ рисуются в Play без ошибок";
        else LOG_ERROR("Editor") << "AI_GRAPH: FAIL — " << why;
        m_aiGraphFrameStep = -1;
    };
    switch (m_aiGraphFrameStep) {
        case 0: {
            std::error_code ec;
            fs::create_directories(dir, ec);
            sage::ai::GraphDoc d;
            d.Kind = sage::ai::GraphKind::Behavior;
            d.Name = "SelftestFrame";
            const int start = d.AddNode("flow.start", 0, 0).Id;
            const int loop = d.AddNode("flow.while", 220, 0).Id;
            d.FindNode(loop)->Values["condition"] = sage::vars::Value(true);
            const int wait = d.AddNode("flow.wait", 440, 0).Id;
            d.FindNode(wait)->Values["seconds"] = sage::vars::Value(0.05f);
            const int dist = d.AddNode("object.distance", 220, 180).Id;
            d.Link({start, "then"}, {loop, "in"}, true);
            d.Link({loop, "body"}, {wait, "in"}, true);
            (void)dist;
            d.AddGroup("Selftest", -40, -60, 720, 340);
            std::ofstream(graphPath, std::ios::binary) << sage::ai::SaveGraph(d);
            GameObject probe = m_scene->CreateObject("AI Frame Probe");
            AiAgentComponent c;
            c.Graph = "assets/ai/SelftestFrame.aigraph";
            m_scene->Registry().emplace<AiAgentComponent>(probe.Entity(), c);
            OpenAiGraph(sage::PathToUtf8(graphPath));
            // Редактор Visual Script — та же основа в своём режиме: рисуется рядом.
            m_panelRegistry.SetVisible("VisualScript", true);
            m_panelRegistry.SetVisible("AiDebugger", true);
            m_aiGraphFrameErrors = m_console.ErrorCount();
            StartPlay();
            ++m_aiGraphFrameStep;
            return;
        }
        case 1: {
            // Показать на холсте выполнение графа, как только оно появилось.
            for (const auto& [id, r] : sage::ai::GraphDebugger::Get().Runs())
                if (r.Graph == "SelftestFrame") m_aiGraph.SetDebugRun(id);
            ++m_aiGraphFrameStep;
            return;
        }
        default:
            if (++m_aiGraphFrameStep < 12) return;   // окна рисуются несколько кадров
            if (m_console.ErrorCount() != m_aiGraphFrameErrors)
                return done(false, "окна графа ИИ дали ошибки в консоли");
            bool running = false;
            for (const auto& [id, r] : sage::ai::GraphDebugger::Get().Runs())
                if (r.Graph == "SelftestFrame" && r.Status == "running" && !r.NodeHits.empty()) running = true;
            if (!running) return done(false, "граф с циклом и ожиданием не идёт в Play");
            return done(true, "");
    }
}
