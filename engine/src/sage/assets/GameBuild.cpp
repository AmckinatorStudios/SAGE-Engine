#include "sage/assets/GameBuild.h"

#include <cctype>

#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::assets {

namespace {

GameBuildResult Fail(GameBuildResult r, const char* key, std::string detail = {}) {
    r.Ok = false;
    r.ErrorKey = key;
    r.ErrorDetail = std::move(detail);
    return r;
}

} // namespace

GameBuildResult BuildGame(const GameBuildRequest& req, const GameBuildHooks& hooks) {
    auto progress = [&](float p) { if (hooks.Progress) hooks.Progress(p); };
    auto stage = [&](const char* key, const std::string& detail = {}) {
        if (hooks.Stage) hooks.Stage(key, detail);
    };
    auto cancelled = [&] { return hooks.Cancelled && hooks.Cancelled(); };

    GameBuildResult res;
    std::error_code ec;
    const std::string exeSuffix = req.Player.extension().string();

    // 1. Папка игры. Пересборка затирает прошлую: это артефакт, не данные.
    stage("Preparing the game folder");
    progress(0.02f);
    const fs::path gameDir = req.OutputDir / sage::PathFromUtf8(req.ProjectName);
    res.GameDir = gameDir;
    fs::remove_all(gameDir, ec);
    ec.clear();
    fs::create_directories(gameDir, ec);
    if (ec)
        return Fail(res, "Cannot create the game folder: ", sage::PathToUtf8(gameDir) + " (" + ec.message() + ")");

    // 2. Исполняемый файл игры и его библиотеки. На Windows плеер, собранный
    // MinGW, тянет рядом с собой .dll (libstdc++ и прочие): без них игра у
    // игрока не запускалась бы вовсе — с сообщением системы, а не движка.
    stage("Copying the player");
    fs::copy_file(req.Player, gameDir / sage::PathFromUtf8(req.ProjectName + exeSuffix),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return Fail(res, "Player copy failed: ", ec.message());
    for (const fs::directory_entry& e : fs::directory_iterator(req.Player.parent_path(), ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (e.is_regular_file(ec) && ext == ".dll")
            fs::copy_file(e.path(), gameDir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (cancelled()) return Fail(res, "Build cancelled");

    // 3. Пакет: ресурсы движка, затем проект (его файл того же имени главнее —
    // PackWriter заменяет запись), затем оглавление базы ассетов.
    stage("Collecting files");
    PackWriter pack;
    pack.AddDirectory(req.Player.parent_path() / "assets", {}, "assets");
    // Папка сборок ВНУТРИ проекта (по умолчанию <проект>/Builds) в пакет не
    // идёт: иначе каждая следующая сборка везла бы в себе предыдущую игру.
    const size_t projectFiles =
        pack.AddDirectory(req.ProjectDir, {".meta"}, "", {req.OutputDir, req.ProjectDir / "Builds"});
    if (!req.AssetIndex.empty())
        pack.Add("assetdb.json", std::vector<uint8_t>(req.AssetIndex.begin(), req.AssetIndex.end()));
    LOG_INFO("Build") << "Сборка игры: в пакет идут " << pack.Count() << " файлов (проекта — " << projectFiles
                      << ")";

    const size_t total = pack.Count();
    res.Files = total;
    const bool saved = pack.Save(gameDir / "game.sagepak", [&](size_t done, size_t all) {
        // 5..95 % шкалы — пакет: это почти всё время сборки.
        progress(0.05f + 0.9f * (all ? (float)done / (float)all : 1.0f));
        stage("Packing and encrypting: ", std::to_string(done) + " / " + std::to_string(total));
        return !cancelled();
    });
    if (!saved) {
        if (cancelled()) {
            fs::remove_all(gameDir, ec);
            return Fail(res, "Build cancelled");
        }
        return Fail(res, "Could not write the game package: ", pack.LastError());
    }

    // 4. Настройки игры — рядом с exe: их игрок вправе править (разрешение,
    // качество), и прятать их в пакет значило бы отнять у него меню «настройки».
    stage("Finishing");
    const fs::path projCfg = req.ProjectDir / "sage.cfg";
    if (fs::exists(projCfg, ec))
        fs::copy_file(projCfg, gameDir / "sage.cfg", fs::copy_options::overwrite_existing, ec);
    if (req.CopyProjectFile) {
        fs::copy_file(req.ProjectDir / "project.sageproj", gameDir / "project.sageproj",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return Fail(res, "Could not copy the project file: ", ec.message());
    }
    progress(1.0f);
    stage("Done");
    LOG_INFO("Build") << "Игра собрана: " << sage::PathToUtf8(gameDir);
    res.Ok = true;
    return res;
}

std::string ExportProjectAssetIndex(const fs::path& projectDir) {
    AssetDatabase db;
    db.ScanProject(sage::PathToUtf8(projectDir));
    return db.ExportIndex();
}

} // namespace sage::assets
