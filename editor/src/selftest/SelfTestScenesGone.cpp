#include "SelfTestCommon.h"

#include <fstream>
#include <sstream>

#include "LogicKinds.h"
#include "sage/ai/Graph.h"

// ---------------------------------------------------------------------------
// УДАЛИЛИ ВСЕ СЦЕНЫ — РЕДАКТОР ЖИВЁТ ДАЛЬШЕ.
//
// Жалоба: «удалю все сцены — редактор и движок падают». Проверяется то, что
// после этого делает человек: Play, повторное открытие проекта, сохранение,
// назначение стартовой сцены — и то, что редактор отпускает ссылки на
// удалённое (путь открытой сцены, стартовая сцена проекта). Отдельно — файл
// проекта, которого нет: SetStartScene звал value() у null и ронял редактор.
//
// И ВИДЫ ЛОГИКИ: окно «Создать…» берёт языки и графы из одного списка
// (LogicKinds.h), каждый встроенный вид создаёт свой файл, слот компонента
// Script предлагает только скрипты.
// ---------------------------------------------------------------------------
bool EditorLayer::SelfTestScenesGone() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: scenes-gone: " << why;
        ok = false;
    };
    if (!m_project.Loaded()) {
        fail("нет проекта");
        return false;
    }
    std::error_code ec;
    const fs::path projectFile = m_project.ProjectFile();
    std::string projectText;
    {
        std::ifstream in(projectFile, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        projectText = ss.str();
    }
    const std::string startBefore = m_project.StartScene();
    // Сцена, на которой работают следующие проверки (кадровые VIEWPORT_INPUT
    // и другие), — снимком: вернуть её ровно такой, а не стартовую проекта.
    const std::string sceneSnapshot = SceneSerializer::SaveToString(*m_scene);
    const fs::path scenePathBefore = m_scenePath;
    const bool dirtyBefore = m_sceneDirty;
    const fs::path scenes = m_project.ScenesDir();
    // Спрятать сцены проекта на время проверки и вернуть потом: проверка
    // удаляет всё, что лежит в папке сцен.
    const fs::path stash = m_project.Dir() / ".sage" / "selftest_scenes_stash";
    fs::remove_all(stash, ec);
    fs::create_directories(stash.parent_path(), ec);
    if (fs::exists(scenes, ec)) fs::rename(scenes, stash, ec);
    fs::create_directories(scenes, ec);

    SaveSceneToFile(scenes / "gone_a.sage");
    SaveSceneToFile(scenes / "gone_b.sage");
    std::string err;
    if (!m_project.SetStartScene("gone_a", err)) fail("стартовая сцена не назначилась: " + err);

    // Удаление, как его делает панель Assets: файлы, затем сама папка.
    std::vector<fs::path> victims = {scenes / "gone_a.sage", scenes / "gone_b.sage"};
    for (const fs::path& v : victims) AssetsPanel::DeleteAsset(v);
    AssetsDeleted(victims);
    if (!m_scenePath.empty()) fail("путь открытой сцены ведёт в удалённый файл");
    if (!m_sceneDirty) fail("сцена без файла не отмечена несохранённой");
    if (!m_project.StartScene().empty()) fail("стартовой осталась удалённая сцена");
    AssetsPanel::DeleteAsset(scenes);
    AssetsDeleted({scenes});

    // Что делает человек дальше.
    StartPlay();
    for (int i = 0; i < 3; ++i) m_systems.Run(*m_scene, 0.1f);
    StopPlay();
    if (!OpenProject(m_project.Dir().string(), err)) fail("проект без сцен не открылся: " + err);
    StartPlay();
    m_systems.Run(*m_scene, 0.1f);
    StopPlay();
    SaveCurrentScene();   // без файла — окно «Сохранить как», а не запись в пустоту
    if (fs::exists(scenes / "gone_a.sage", ec)) fail("Ctrl+S воскресил удалённую сцену");
    if (!BuildGame(m_project.Dir() / ".sage" / "selftest_nobuild", err) && err.empty())
        fail("сборка без сцен молча не удалась");

    // Файла проекта нет — назначение стартовой сцены не роняет редактор.
    fs::remove(projectFile, ec);
    try {
        if (!m_project.SetStartScene("x", err)) fail("стартовая сцена без файла проекта: " + err);
    } catch (const std::exception& e) {
        fail(std::string("SetStartScene без файла проекта бросил исключение: ") + e.what());
    }

    // Вернуть всё как было.
    m_pendingDialog = nullptr;
    {
        std::ofstream out(projectFile, std::ios::binary);
        out << projectText;
    }
    fs::remove_all(scenes, ec);
    if (fs::exists(stash, ec)) fs::rename(stash, scenes, ec);
    fs::create_directories(scenes, ec);
    m_project.SetStartScene(startBefore, err);
    fs::remove_all(m_project.Dir() / ".sage" / "selftest_nobuild", ec);
    if (!OpenProject(m_project.Dir().string(), err)) fail("проект не вернулся: " + err);
    if (!RestoreSceneFromString(sceneSnapshot)) fail("сцена проверок не вернулась");
    m_scenePath = scenePathBefore;
    m_sceneDirty = dirtyBefore;
    m_history.Clear();
    m_selection.Clear();
    UpdateWindowTitle();

    // --- Виды логики ---------------------------------------------------------
    namespace lg = sage::editor::logic;
    for (const char* id : {"lua", "slua", "visual", "task", "ai"})
        if (!lg::Find(id)) fail(std::string("нет встроенного вида логики ") + id);
    const auto forScript = lg::ForSlot("script");
    if (forScript.size() != 2) fail("слот Script предлагает не только скрипты");
    if (lg::ForSlot("").size() < 5) fail("окно «Создать…» видит не все виды логики");
    const fs::path tmp = m_project.Dir() / ".sage" / "selftest_logic";
    fs::remove_all(tmp, ec);
    for (const lg::Kind& k : lg::All()) {
        if (!k.Owner.empty()) continue;
        fs::path made;
        if (!k.Create || !k.Create("Probe_" + k.Id, tmp, made, err) || !fs::exists(made, ec)) {
            fail("вид логики " + k.Id + " не создал файл: " + err);
            continue;
        }
        if (made.extension() != k.Ext) fail("вид " + k.Id + " создал файл не с тем расширением");
        if (k.Ext == ".vgraph" || k.Ext == ".aigraph") {
            std::ifstream in(made, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            sage::ai::GraphDoc d;
            std::string gerr;
            if (!sage::ai::LoadGraph(ss.str(), d, gerr)) fail("граф вида " + k.Id + " не читается: " + gerr);
            const sage::ai::GraphKind want = k.Id == "visual" ? sage::ai::GraphKind::Script
                                             : k.Id == "task" ? sage::ai::GraphKind::Task
                                                              : sage::ai::GraphKind::Behavior;
            if (d.Kind != want) fail("граф вида " + k.Id + " не того вида");
        }
    }
    // Свой вид (как из плагина): регистрация, замена тем же Id, снятие владельца.
    lg::Kind mine;
    mine.Id = "selftest.fnl";
    mine.Owner = "selftest";
    mine.Ext = ".fnl";
    lg::Register(mine);
    lg::Register(mine);
    int count = 0;
    for (const lg::Kind& k : lg::All()) count += k.Id == "selftest.fnl";
    if (count != 1) fail("вид логики с тем же Id не заменился");
    lg::RemoveOwner("selftest");
    if (lg::Find("selftest.fnl")) fail("выгрузка владельца не убрала его вид логики");
    fs::remove_all(tmp, ec);

    if (ok) LOG_INFO("Editor") << "SELFTEST: scenes-gone OK";
    return ok;
}
