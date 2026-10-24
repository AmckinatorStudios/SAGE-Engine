// С КАКОЙ СЦЕНЫ НАЧИНАЕТСЯ ИГРА — проверка сквозная, от проекта до плеера.
//
// До сих пор этого выбора не существовало: собранная игра брала
// scenes/main.sage, а если её нет — ПЕРВУЮ ПО АЛФАВИТУ. Уровень, названный
// «arena», молча становился началом игры, переименование файла меняло то, что
// увидит игрок, и заметить это можно было только собрав игру и запустив её.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "Project.h"

namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string("sage_startscene_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

} // namespace

TEST(Project_start_scene_is_written_and_read_back) {
    const fs::path base = TempDir("roundtrip");
    std::error_code ec;
    fs::create_directories(base, ec);

    Project project;
    std::string err;
    CHECK_TRUE(project.CreateNew(base, "Игра", err));
    // Новый проект начинается с main — редактор создаёт эту сцену сам, и
    // оставить выбор пустым значило бы, что первая же сборка снова угадывает.
    CHECK_TRUE(project.StartScene() == "main");

    CHECK_TRUE(project.SetStartScene("level2", err));
    CHECK_TRUE(project.StartScene() == "level2");

    // Записано НА ДИСК, а не только в память: настройка, живущая до закрытия
    // редактора, теряется ровно тогда, когда человек уверен, что всё настроил.
    Project reopened;
    CHECK_TRUE(reopened.Open(base / "Игра", err));
    CHECK_TRUE(reopened.StartScene() == "level2");

    fs::remove_all(base, ec);
}

// Чужие поля манифеста НЕ ТЕРЯЮТСЯ: в project.sageproj может лежать то, чего
// эта версия редактора не знает, и переписать файл «своими» полями значило бы
// молча стереть чужую работу.
TEST(Project_start_scene_keeps_unknown_manifest_fields) {
    const fs::path base = TempDir("fields");
    std::error_code ec;
    fs::create_directories(base, ec);

    Project project;
    std::string err;
    CHECK_TRUE(project.CreateNew(base, "Игра", err));

    {
        nlohmann::json j;
        {
            std::ifstream in(project.ProjectFile());
            in >> j;
        }
        j["buildSettings"] = {{"compression", "high"}};
        std::ofstream out(project.ProjectFile());
        out << j.dump(2);
    }

    CHECK_TRUE(project.SetStartScene("arena", err));

    nlohmann::json after;
    {
        std::ifstream in(project.ProjectFile());
        in >> after;
    }
    CHECK_TRUE(after.value("startScene", std::string()) == "arena");
    CHECK_TRUE(after.contains("buildSettings"));
    CHECK_TRUE(after["buildSettings"].value("compression", std::string()) == "high");

    fs::remove_all(base, ec);
}

// СЦЕНЫ — В assets/Scenes. Новый проект кладёт их туда, а старый (сцены в
// <проект>/scenes) при открытии переносится сам: сцены вместе с .meta (GUID —
// ссылки на сцену не рвутся) и вложенными папками.
TEST(Project_scenes_live_in_assets_and_old_projects_move_there) {
    const fs::path base = TempDir("scenes_dir");
    std::error_code ec;
    fs::create_directories(base, ec);
    Project fresh;
    std::string err;
    CHECK_TRUE(fresh.CreateNew(base, "New", err));
    CHECK_TRUE(fresh.ScenesDir() == base / "New" / "assets" / "Scenes");
    CHECK_TRUE(fs::is_directory(fresh.ScenesDir(), ec));
    CHECK_FALSE(fs::exists(base / "New" / "scenes", ec));

    // Старый проект: сцены лежат в scenes/.
    const fs::path old = base / "Old";
    fs::create_directories(old / "scenes" / "levels", ec);
    std::ofstream(old / "project.sageproj") << R"({"sage_project_version":1,"name":"Old","startScene":"main"})";
    std::ofstream(old / "scenes" / "main.sage") << "{}";
    std::ofstream(old / "scenes" / "main.sage.meta") << R"({"guid":"abc"})";
    std::ofstream(old / "scenes" / "levels" / "cave.sage") << "{}";
    Project project;
    CHECK_TRUE(project.Open(old, err));
    CHECK_TRUE(fs::exists(old / "assets" / "Scenes" / "main.sage", ec));
    CHECK_TRUE(fs::exists(old / "assets" / "Scenes" / "main.sage.meta", ec));
    CHECK_TRUE(fs::exists(old / "assets" / "Scenes" / "levels" / "cave.sage", ec));
    CHECK_FALSE(fs::exists(old / "scenes", ec));   // пустая старая папка убрана
    fs::remove_all(base, ec);
}
