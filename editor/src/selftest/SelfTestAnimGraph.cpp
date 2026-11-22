#include "SelfTestCommon.h"

#include "AssetOpen.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/anim/AnimGraph.h"
#include "sage/scene/SceneSerializer.h"
#include <functional>
#include <nlohmann/json.hpp>

// ANIMATION GRAPH в редакторе: «Создать» даёт пустой граф с выходом, файл
// узнаётся слотом и открывается в своём окне, окно правит и сохраняет граф,
// компонент Animation подхватывает граф по пути, путь переживает сохранение
// сцены, а сохранение графа доезжает до объекта без перезапуска.
bool EditorLayer::SelfTestAnimGraph() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: anim-graph: " << why;
        ok = false;
    };
    const fs::path dir = m_project.AssetsDir() / "animations";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path created;
    std::string err;
    if (!AssetsPanel::CreateAsset(AssetsPanel::CreateKind::AnimGraph, "SelftestGraph", dir, created, err)) {
        fail("граф не создан: " + err);
        return ok;
    }
    if (created.extension() != ".sageanimgraph") fail("у созданного графа не то расширение");
    if (assetslot::KindOf(created) != assetslot::Kind::AnimGraph) fail("слот не узнаёт файл графа");
    if (sage::editor::assetopen::Of(created, false) != sage::editor::assetopen::OpenAs::AnimGraph)
        fail("файл графа открывается не в окне графа");
    try {
        const sage::anim::AnimGraph g = sage::anim::LoadAnimGraph(sage::PathToUtf8(created));
        if (g.Nodes.size() != 1 || g.OutputNode() == 0) fail("новый граф — не «только выход»");
    } catch (const std::exception& e) {
        fail(std::string("созданный граф не читается: ") + e.what());
    }

    // --- Создание через слот: файл настоящий, назначается, Workspace не открывается.
    // Слот зовёт то же CreateAsset (окно «Создать») и кладёт ссылку в GraphPath;
    // OpenAfter у него выключен, поэтому ничего не открывается — проверяем, что
    // после назначения окно графа и пространство остались как были.
    SetWorkspace(EditorWorkspace::Scene);
    if (m_animGraph.HasGraph()) fail("предусловие: окно графа уже занято");
    GameObject probe = m_scene->CreateObject("AnimGraph Probe");
    AnimationComponent am;
    am.ClipPath = "Character.sageanim";
    am.GraphPath = m_project.AssetRef(created);   // как назначает слот
    am.Source = AnimationComponent::SourceKind::Graph;
    m_scene->Registry().emplace<AnimationComponent>(probe.Entity(), am);
    if (m_animGraph.HasGraph() || m_workspace != EditorWorkspace::Scene) fail("создание/назначение графа открыло Workspace");
    {
        const auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        if (c.ClipPath != "Character.sageanim") fail("назначение графа стёрло ClipPath");
        if (!fs::exists(created)) fail("файла графа нет на диске");
    }

    // --- Runtime: Source=Graph, граф только с Output — не работающая анимация.
    sage::anim::UpdateAnimators(*m_scene, 0.016f);
    {
        const auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        if (c.GraphAsset) fail("граф без подключённого выхода принят за рабочий");
        if (c.GraphProblem.empty()) fail("пустой граф не объяснён");
    }
    // Source=Clip: граф не читается, пути на месте.
    {
        auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        c.Source = AnimationComponent::SourceKind::Clip;
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (c.GraphAsset || !c.GraphProblem.empty()) fail("при Source=Clip граф загружен или диагностирован");
        if (c.GraphPath.empty() || c.ClipPath != "Character.sageanim") fail("переключение на Clip стёрло пути");
        c.Source = AnimationComponent::SourceKind::Graph;
        c.GraphPath.clear();
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (c.GraphProblem.empty() || c.ClipPath.empty()) fail("Graph без пути: нет диагностики или стёрт клип");
        c.GraphPath = "missing/Nope.sageanimgraph";
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (c.GraphAsset || c.GraphProblem.empty()) fail("нечитаемый граф не диагностирован");
        c.GraphPath = m_project.AssetRef(created);
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
    }

    // --- Источник и оба пути переживают сцену; старая сцена выбирает по GraphPath.
    {
        const std::string text = SceneSerializer::SaveToString(*m_scene);
        if (text.find("\"graphPath\"") == std::string::npos) fail("путь графа не попал в сцену");
        if (text.find("\"clipPath\":\"Character.sageanim\"") == std::string::npos &&
            text.find("\"clipPath\": \"Character.sageanim\"") == std::string::npos)
            fail("ClipPath не попал в сцену вместе с графом");
        if (text.find("\"source\"") == std::string::npos) fail("Source не попал в сцену");
        const auto round = SceneSerializer::LoadFromString(text);
        if (!round) fail("сцена не загрузилась");
        bool found = false;
        if (round)
        for (auto [e, c] : round->Registry().view<AnimationComponent>().each()) {
            (void)e;
            if (c.GraphPath.empty()) continue;
            found = true;
            if (c.Source != AnimationComponent::SourceKind::Graph || c.ClipPath != "Character.sageanim")
                fail("Source/ClipPath не восстановились после загрузки");
        }
        if (!found) fail("компонент не найден после загрузки");
        // Сцена без поля source.
        std::string legacy = text;
        const size_t pos = legacy.find("\"source\"");
        if (pos != std::string::npos) {
            nlohmann::json root = nlohmann::json::parse(text);
            std::function<void(nlohmann::json&)> strip = [&](nlohmann::json& j) {
                if (j.is_object()) {
                    if (j.contains("animation") && j["animation"].is_object()) j["animation"].erase("source");
                    for (auto& [k, v] : j.items()) strip(v);
                } else if (j.is_array()) {
                    for (auto& v : j) strip(v);
                }
            };
            strip(root);
            const auto old = SceneSerializer::LoadFromString(root.dump());
            if (!old) fail("старая сцена не загрузилась");
            if (old)
            for (auto [e, c] : old->Registry().view<AnimationComponent>().each()) {
                (void)e;
                if (c.Source != AnimationComponent::SourceKind::Graph) fail("старая сцена с графом загрузилась не в Graph");
            }
            nlohmann::json noGraph = root;
            std::function<void(nlohmann::json&)> dropGraph = [&](nlohmann::json& j) {
                if (j.is_object()) {
                    if (j.contains("animation") && j["animation"].is_object()) j["animation"].erase("graphPath");
                    for (auto& [k, v] : j.items()) dropGraph(v);
                } else if (j.is_array()) {
                    for (auto& v : j) dropGraph(v);
                }
            };
            dropGraph(noGraph);
            const auto old2 = SceneSerializer::LoadFromString(noGraph.dump());
            if (!old2) fail("старая сцена без графа не загрузилась");
            if (old2)
            for (auto [e, c] : old2->Registry().view<AnimationComponent>().each()) {
                (void)e;
                if (c.Source != AnimationComponent::SourceKind::Clip) fail("старая сцена без графа загрузилась не в Clip");
            }
        } else {
            fail("в тексте сцены нет source для проверки миграции");
        }
    }

    // --- Открытие ресурса: цель предпросмотра, Source не меняется.
    EditAnimation(probe.Id(), m_project.AssetRef(created));
    if (!m_animGraph.HasGraph() || m_animGraph.Path() != sage::AssetDatabase::Instance().LocatePath(m_project.AssetRef(created)))
        fail("EditAnimation не открыл граф по пути");
    if (m_workspace != EditorWorkspace::Animation) fail("граф открылся не в пространстве анимации");
    if (m_animStage.TargetId() != probe.Id()) fail("EditAnimation не поставил цель предпросмотра");
    if (m_animStage.Mode() != AnimStage::Source::Graph) fail("стейдж не в режиме графа");
    {
        auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        c.Source = AnimationComponent::SourceKind::Clip;
        EditAnimation(probe.Id(), m_project.AssetRef(created));
        if (c.Source != AnimationComponent::SourceKind::Clip) fail("открытие графа переключило Source объекта");
        if (c.GraphPath.empty() || c.ClipPath != "Character.sageanim") fail("открытие графа тронуло пути компонента");
        c.Source = AnimationComponent::SourceKind::Graph;
    }
    if (!m_animGraph.Save(*this)) fail("окно не сохранило граф");

    // Сохранение графа в окне доезжает до объекта.
    {
        auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        // Подключаем выход к клипу — теперь граф «рабочий».
        sage::anim::AnimGraph g = sage::anim::LoadAnimGraph(sage::PathToUtf8(created));
        const int clipNode = g.Add(sage::anim::AnimNodeKind::Clip);
        if (!g.Connect(clipNode, g.OutputNode(), 0)) fail("не подключить клип к выходу");
        sage::anim::SaveAnimGraph(g, sage::PathToUtf8(created));
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (!c.GraphAsset || !c.GraphProblem.empty()) fail("подключённый граф не принят после сохранения");
    }

    // --- Защита несохранённого: Cancel / Discard / Save.
    {
        const fs::path other = dir / "SelftestGraphOther.sageanimgraph";
        fs::path made;
        std::string err2;
        if (!AssetsPanel::CreateAsset(AssetsPanel::CreateKind::AnimGraph, "SelftestGraphOther", dir, made, err2)) {
            fail("второй граф не создан: " + err2);
        } else {
            const std::string firstFull = m_animGraph.Path();
            const size_t nodesBefore = m_animGraph.Graph().Nodes.size();
            const size_t fileNodes = sage::anim::LoadAnimGraph(firstFull).Nodes.size();
            // Правим открытый граф, не сохраняя.
            m_animGraph.AddNodeAt(*this, sage::anim::AnimNodeKind::Float, glm::vec2(10.0f, 10.0f));
            if (!m_animGraph.Dirty()) fail("правка не отметила документ изменённым");
            const std::string otherFull = sage::PathToUtf8(made);
            // Тот же файл — без вопроса.
            OpenAnimGraph(firstFull);
            if (m_animOpenPrompt) fail("повторное открытие того же файла показало диалог");
            // Cancel.
            OpenAnimGraph(otherFull);
            if (!m_animOpenPrompt) fail("несохранённый граф не вызвал диалог");
            AnswerAnimOpenPrompt(AnimOpenAnswer::Cancel);
            if (m_animGraph.Path() != firstFull || !m_animGraph.Dirty() ||
                m_animGraph.Graph().Nodes.size() != nodesBefore + 1)
                fail("Cancel не оставил документ и его правки нетронутыми");
            if (m_scene->Registry().get<AnimationComponent>(probe.Entity()).GraphPath.empty())
                fail("Cancel потерял назначенный граф");
            // Discard: правка не попадает в файл, открывается запрошенный.
            OpenAnimGraph(otherFull);
            AnswerAnimOpenPrompt(AnimOpenAnswer::Discard);
            if (m_animGraph.Path() != otherFull) fail("Discard не открыл запрошенный граф");
            if (sage::anim::LoadAnimGraph(firstFull).Nodes.size() != fileNodes) fail("Discard записал правку в файл");
            // Save: сначала запись, потом открытие.
            m_animGraph.AddNodeAt(*this, sage::anim::AnimNodeKind::Float, glm::vec2(20.0f, 20.0f));
            const size_t otherNodes = m_animGraph.Graph().Nodes.size();
            OpenAnimGraph(firstFull);
            AnswerAnimOpenPrompt(AnimOpenAnswer::Save);
            if (m_animGraph.Path() != firstFull) fail("Save не открыл запрошенный граф");
            if (sage::anim::LoadAnimGraph(otherFull).Nodes.size() != otherNodes) fail("Save не записал документ");
            fs::remove(made, ec);
        }
    }

    // Удаление ресурса из слота не трогает соседний путь (логика слота: Changed → один путь).
    {
        auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        c.GraphPath.clear();
        if (c.ClipPath != "Character.sageanim") fail("очистка Graph задела ClipPath");
        c.GraphPath = m_project.AssetRef(created);
        c.ClipPath.clear();
        if (c.GraphPath.empty()) fail("очистка Clip задела GraphPath");
    }

    m_animStage.SetTarget(*this, -1);
    m_scene->RemoveObject(probe.Id());
    SetWorkspace(EditorWorkspace::Scene);
    fs::remove(created, ec);
    if (ok) LOG_INFO("Editor") << "ANIM_GRAPH: OK — ассет, окно, компонент, источник, защита правок";
    return ok;
}
