// ---------------------------------------------------------------------------
// sage_build — собранная игра из проекта без редактора.
//
//   sage_build --project <папка проекта> --player <SagePlayer[.exe]> --out <папка>
//              [--name <имя игры>] [--project-file]
//
// Результат — <out>/<имя>/: исполняемый файл игры, его .dll, game.sagepak и
// sage.cfg. Ровно то, что делает «File > Build Game» (sage::assets::BuildGame),
// поэтому выпуск из CI и сборка руками не расходятся. Плеер может быть чужой
// платформы: собрать игру для Windows можно на Linux, взяв SagePlayer.exe и
// его assets/ из релиза движка.
//
// Имя игры по умолчанию — поле "name" из project.sageproj, иначе имя папки.
// Код возврата: 0 — собрано, 1 — ошибка сборки, 2 — неверные аргументы.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sage/assets/GameBuild.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace {

std::string ProjectName(const fs::path& projectDir) {
    std::ifstream f(projectDir / "project.sageproj");
    if (f) {
        const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object() && j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty())
            return j["name"].get<std::string>();
    }
    return sage::PathToUtf8(fs::absolute(projectDir).filename());
}

int Usage() {
    std::fprintf(stderr,
                 "использование: sage_build --project <папка> --player <SagePlayer> --out <папка> "
                 "[--name <имя>] [--project-file]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    sage::assets::GameBuildRequest req;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--project" && (v = next())) req.ProjectDir = sage::PathFromUtf8(v);
        else if (a == "--player" && (v = next())) req.Player = sage::PathFromUtf8(v);
        else if (a == "--out" && (v = next())) req.OutputDir = sage::PathFromUtf8(v);
        else if (a == "--name" && (v = next())) req.ProjectName = v;
        else if (a == "--project-file") req.CopyProjectFile = true;
        else return Usage();
    }
    if (req.ProjectDir.empty() || req.Player.empty() || req.OutputDir.empty()) return Usage();
    std::error_code ec;
    if (!fs::exists(req.ProjectDir / "project.sageproj", ec)) {
        std::fprintf(stderr, "sage_build: в %s нет project.sageproj\n", sage::PathToUtf8(req.ProjectDir).c_str());
        return 1;
    }
    if (!fs::exists(req.Player, ec)) {
        std::fprintf(stderr, "sage_build: нет плеера %s\n", sage::PathToUtf8(req.Player).c_str());
        return 1;
    }
    if (req.ProjectName.empty()) req.ProjectName = ProjectName(req.ProjectDir);
    req.AssetIndex = sage::assets::ExportProjectAssetIndex(req.ProjectDir);

    const sage::assets::GameBuildResult r = sage::assets::BuildGame(req);
    if (!r.Ok) {
        std::fprintf(stderr, "sage_build: %s%s\n", r.ErrorKey.c_str(), r.ErrorDetail.c_str());
        return 1;
    }
    std::printf("sage_build: %s (%zu файлов в пакете)\n", sage::PathToUtf8(r.GameDir).c_str(), r.Files);
    return 0;
}
