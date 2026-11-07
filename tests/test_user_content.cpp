// Пользовательские папки игры: описание в проекте, путь, фильтр файлов,
// примеры из проекта, подготовка при сборке.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/assets/GameInfo.h"
#include "sage/assets/UserContent.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;
namespace uc = sage::assets::usercontent;

namespace {
fs::path TempDir(const char* name) {
    fs::path d = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}
void Touch(const fs::path& p, const char* text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}
sage::assets::UserFolderSpec Skins() {
    sage::assets::UserFolderSpec s;
    s.Name = "skins";
    s.Title = "Skins";
    s.Description = "Put 64x64 PNG skins here.";
    s.Extensions = {".png"};
    return s;
}
} // namespace

TEST(UserContent_folders_roundtrip_through_project_file) {
    sage::assets::GameInfo g;
    g.Name = "Game";
    sage::assets::UserFolderSpec m = Skins();
    m.Name = "music";
    m.Extensions = {".ogg", ".wav"};
    m.NextToGame = false;
    g.UserFolders = {Skins(), m};
    nlohmann::json project = nlohmann::json::object();
    g.WriteTo(project);
    sage::assets::GameInfo back = sage::assets::GameInfo::FromProject(project);
    CHECK_EQ(back.UserFolders.size(), (size_t)2);
    CHECK_TRUE(back.UserFolders[0].Name == "skins" && back.UserFolders[0].NextToGame);
    CHECK_TRUE(back.UserFolders[1].Name == "music" && !back.UserFolders[1].NextToGame);
    CHECK_EQ(back.UserFolders[1].Extensions.size(), (size_t)2);
}

TEST(UserContent_built_game_folder_lists_only_declared_types) {
    const fs::path game = TempDir("sage_uc_game");
    const fs::path project = TempDir("sage_uc_project");
    uc::Configure({Skins()}, game, project);
    const fs::path dir = uc::Resolve("skins");
    CHECK_TRUE(dir == game / "skins");
    CHECK_TRUE(fs::exists(dir / "README.txt"));      // игроку объяснено, что сюда класть
    Touch(dir / "steve.png");
    Touch(dir / "Alex.PNG");                         // расширение — без учёта регистра
    Touch(dir / "notes.txt");                        // чужой тип — не виден
    Touch(dir / "sub" / "hat.png");                  // подпапки читаются
    Touch(dir / ".hidden.png");                      // скрытые — нет
    std::vector<uc::Entry> files = uc::List("skins");
    CHECK_EQ(files.size(), (size_t)3);
    CHECK_TRUE(files[0].Name == "Alex.PNG");
    CHECK_TRUE(files[2].Relative == "sub/hat.png");
    CHECK_EQ(uc::List("skins", false).size(), (size_t)2);
    // Чужое имя не открывается: произвольный путь диска так не прочитать.
    CHECK_TRUE(uc::Resolve("../etc").empty());
    CHECK_TRUE(uc::List("music").empty());
    CHECK_FALSE(uc::ValidName("a/b"));
    CHECK_FALSE(uc::ValidName(".."));
}

TEST(UserContent_signature_changes_with_files) {
    const fs::path game = TempDir("sage_uc_sig");
    uc::Configure({Skins()}, game, {});
    const std::string before = uc::Signature("skins");
    Touch(uc::Resolve("skins") / "new.png");
    CHECK_TRUE(uc::Signature("skins") != before);
}

TEST(UserContent_build_creates_folder_with_examples_not_overwriting) {
    const fs::path project = TempDir("sage_uc_build_project");
    const fs::path game = TempDir("sage_uc_build_game");
    Touch(project / "userfolders" / "skins" / "example.png", "example");
    Touch(project / "userfolders" / "skins" / "example.png.meta");
    sage::assets::UserFolderSpec data = Skins();
    data.Name = "saves_extra";
    data.NextToGame = false;
    CHECK_EQ(uc::PrepareForBuild(game, project, {Skins(), data}), (size_t)1);
    CHECK_TRUE(fs::exists(game / "skins" / "example.png"));
    CHECK_FALSE(fs::exists(game / "skins" / "example.png.meta"));
    CHECK_FALSE(fs::exists(game / "saves_extra"));   // «в данных игрока» — не рядом с игрой
    // Игрок поменял пример — повторная подготовка его не затирает.
    Touch(game / "skins" / "example.png", "mine");
    uc::PrepareForBuild(game, project, {Skins()});
    std::ifstream f(game / "skins" / "example.png");
    std::string text;
    f >> text;
    CHECK_TRUE(text == "mine");
}
