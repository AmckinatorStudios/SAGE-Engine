// ===========================================================================
//  ExternalGame — жизненный цикл отдельного процесса игры (режим Window).
//
//  Проверяется то, на чём держится «закрыл окно — Play остановился» и «Stop —
//  и ни одного висящего процесса»: запуск, окружение только дочернему,
//  естественное завершение, принудительная остановка, повторный запуск.
//  Вместо SagePlayer — маленький sh-скрипт (только POSIX).
// ===========================================================================
#include "TestFramework.h"

#ifndef _WIN32
#include "../editor/src/ExternalGame.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

namespace {
fs::path MakeFakePlayer(const fs::path& dir, const std::string& body) {
    fs::create_directories(dir);
    const fs::path p = dir / "FakePlayer";
    std::ofstream(p) << "#!/bin/sh\n" << body << "\n";
    chmod(p.c_str(), 0755);
    return p;
}
bool WaitFor(ExternalGame& g, bool running, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        if (g.Running() == running) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return g.Running() == running;
}
} // namespace

TEST(ExternalGame_env_goes_to_child_only_and_stop_kills_it) {
    const fs::path dir = fs::temp_directory_path() / "sage_ext_game_test1";
    fs::remove_all(dir);
    const fs::path out = dir / "out.txt";
    const fs::path player = MakeFakePlayer(dir, "echo \"$SAGE_PLAY_SCENE|$1\" > '" + out.string() + "'\nexec sleep 60");
    ExternalGame g;
    std::string err;
    CHECK_TRUE(g.Start(player, "/proj", {{"SAGE_PLAY_SCENE", "scene.sage"}}, err));
    CHECK_TRUE(g.Running());
    // Редактор своё окружение не получил.
    CHECK_TRUE(std::getenv("SAGE_PLAY_SCENE") == nullptr);
    for (int i = 0; i < 200 && !fs::exists(out); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string line;
    std::getline(std::ifstream(out), line);
    CHECK_EQ(line, std::string("scene.sage|/proj"));
    g.Stop();
    CHECK_FALSE(g.Running());
    fs::remove_all(dir);
}

TEST(ExternalGame_natural_exit_is_noticed_and_restart_works) {
    const fs::path dir = fs::temp_directory_path() / "sage_ext_game_test2";
    fs::remove_all(dir);
    const fs::path player = MakeFakePlayer(dir, "exit 0");
    ExternalGame g;
    std::string err;
    CHECK_TRUE(g.Start(player, "/proj", {}, err));
    CHECK_TRUE(WaitFor(g, false, 3000)); // «закрыли окно»
    CHECK_TRUE(g.Start(player, "/proj", {}, err)); // повторный запуск
    CHECK_TRUE(WaitFor(g, false, 3000));
    ExternalGame bad;
    // Несуществующий плеер: процесс умирает сразу, и редактор это видит так же,
    // как закрытие окна, — Play не зависает «запущенным».
    CHECK_TRUE(bad.Start(dir / "nope", "/proj", {}, err));
    CHECK_TRUE(WaitFor(bad, false, 3000));
    fs::remove_all(dir);
}
#endif
