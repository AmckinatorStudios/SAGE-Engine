#include "SelfTestCommon.h"

#include "AssetOpen.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/anim/AnimGraph.h"
#include "sage/scene/SceneSerializer.h"

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

    OpenAnimGraph(sage::PathToUtf8(created));
    if (!m_animGraph.HasGraph() || m_animGraph.Path() != sage::PathToUtf8(created)) fail("граф не открылся в окне");
    if (m_workspace != EditorWorkspace::Animation) fail("граф открылся не в пространстве анимации");

    // Объект с графом: путь — ссылкой проекта, как его кладёт слот.
    GameObject probe = m_scene->CreateObject("AnimGraph Probe");
    AnimationComponent am;
    am.GraphPath = m_project.AssetRef(created);
    m_scene->Registry().emplace<AnimationComponent>(probe.Entity(), am);
    sage::anim::UpdateAnimators(*m_scene, 0.016f);
    {
        const auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        if (!c.GraphAsset) fail("компонент не прочитал граф по пути");
        // Модели нет — анимировать нечего, и граф не привязан (а не упал).
        if (c.Graph.Bound()) fail("граф привязан без скелета");
    }
    // Сохранение графа в окне доезжает до объекта.
    const uint64_t before = sage::anim::AnimGraphRevision();
    if (!m_animGraph.Save(*this)) fail("окно не сохранило граф");
    if (sage::anim::AnimGraphRevision() == before) fail("сохранение не отмечено ревизией");
    {
        auto& c = m_scene->Registry().get<AnimationComponent>(probe.Entity());
        const void* old = c.GraphAsset.get();
        sage::anim::UpdateAnimators(*m_scene, 0.016f);
        if (c.GraphAsset.get() == old) fail("сохранённый граф не перечитан объектом");
    }
    // Путь графа переживает сохранение сцены.
    {
        const std::string text = SceneSerializer::SaveToString(*m_scene);
        if (text.find("\"graphPath\"") == std::string::npos) fail("путь графа не попал в сцену");
    }

    m_scene->RemoveObject(probe.Id());
    SetWorkspace(EditorWorkspace::Scene);
    fs::remove(created, ec);
    if (ok) LOG_INFO("Editor") << "ANIM_GRAPH: OK — ассет, окно, компонент и сохранение графа";
    return ok;
}
