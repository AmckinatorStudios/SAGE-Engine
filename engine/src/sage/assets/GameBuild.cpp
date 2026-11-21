#include "sage/assets/GameBuild.h"

#include <atomic>
#include <cctype>
#include <chrono>

#include "sage/assets/AssetDatabase.h"
#include "sage/assets/GameInfo.h"
#include "sage/assets/Pack.h"
#include "sage/assets/UserContent.h"
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

    // Сведения об игре (раздел "game" файла проекта): имя папки и exe —
    // название игры, а не папки проекта; иконка и сведения о файле — в exe.
    GameInfo info;
    const bool hasInfo = LoadGameInfo(req.ProjectDir, info);
    if (!hasInfo || info.Name.empty()) info.Name = req.ProjectName;
    const std::string fileName = req.ProjectName.empty() || hasInfo ? info.FileName() : req.ProjectName;

    // 1. Папка игры. СОБИРАЕМ ВО ВРЕМЕННОЙ папке рядом с итоговой (тот же том —
    // переименование атомарно) и подменяем только после успеха: раньше
    // remove_all стоял ДО сборки, и ошибка или отмена уничтожали прошлую
    // рабочую игру. Имя уникально (время + счётчик), чтобы параллельные и
    // оборванные прежние сборки не мешали.
    stage("Preparing the game folder");
    progress(0.02f);
    const fs::path gameDir = req.OutputDir / sage::PathFromUtf8(fileName);
    res.GameDir = gameDir;
    static std::atomic<unsigned> counter{0};
    const std::string uniq = std::to_string(
        (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(counter++);
    const fs::path tmpDir = req.OutputDir / sage::PathFromUtf8(fileName + ".build-" + uniq);
    // Любой отказ убирает ТОЛЬКО временную папку.
    auto Abort = [&](const char* key, std::string detail = {}) {
        std::error_code rmEc;
        fs::remove_all(tmpDir, rmEc);
        return Fail(res, key, std::move(detail));
    };
    fs::create_directories(tmpDir, ec);
    if (ec)
        return Abort("Cannot create the game folder: ", sage::PathToUtf8(tmpDir) + " (" + ec.message() + ")");

    // 2. Исполняемый файл игры и его библиотеки. На Windows плеер, собранный
    // MinGW, тянет рядом с собой .dll (libstdc++ и прочие): без них игра у
    // игрока не запускалась бы вовсе — с сообщением системы, а не движка.
    stage("Copying the player");
    const fs::path exe = tmpDir / sage::PathFromUtf8(fileName + exeSuffix);
    fs::copy_file(req.Player, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) return Abort("Player copy failed: ", ec.message());
    res.Exe = gameDir / sage::PathFromUtf8(fileName + exeSuffix);
    // Иконка и «Свойства» файла (версия, автор, описание) — в сам exe. Это
    // украшение, а не условие запуска: не вышло — предупреждение, игра
    // собирается дальше.
    if (exeSuffix == ".exe" || exeSuffix == ".EXE") {
        IconImage icon;
        const bool haveIcon = !info.Icon.empty() &&
                              LoadIconImage(sage::PathToUtf8(req.ProjectDir / sage::PathFromUtf8(info.Icon)), icon);
        if (!info.Icon.empty() && !haveIcon)
            LOG_WARN("Build") << "Иконка игры не прочиталась: " << info.Icon;
        std::string why;
        if (EmbedWindowsResources(exe, info, haveIcon ? &icon : nullptr, why))
            LOG_INFO("Build") << "В exe вшиты " << (haveIcon ? "иконка и " : "") << "сведения о файле";
        else
            LOG_WARN("Build") << "Сведения о файле не вшиты: " << why;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(req.Player.parent_path(), ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (e.is_regular_file(ec) && ext == ".dll")
            fs::copy_file(e.path(), tmpDir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (cancelled()) return Abort("Build cancelled");

    // 3. Пакет: ресурсы движка, затем проект (его файл того же имени главнее —
    // PackWriter заменяет запись), затем оглавление базы ассетов.
    stage("Collecting files");
    PackWriter pack;
    pack.AddDirectory(req.Player.parent_path() / "assets", {}, "assets");
    // Папка сборок ВНУТРИ проекта (по умолчанию <проект>/Builds) в пакет не
    // идёт: иначе каждая следующая сборка везла бы в себе предыдущую игру.
    const size_t projectFiles =
        pack.AddDirectory(req.ProjectDir, {".meta"}, "", {req.OutputDir, req.ProjectDir / "Builds",
                                                         // примеры пользовательских папок — рядом с игрой, не в пакете
                                                         req.ProjectDir / "userfolders"});
    if (!req.AssetIndex.empty())
        pack.Add("assetdb.json", std::vector<uint8_t>(req.AssetIndex.begin(), req.AssetIndex.end()));
    LOG_INFO("Build") << "Сборка игры: в пакет идут " << pack.Count() << " файлов (проекта — " << projectFiles
                      << ")";

    const size_t total = pack.Count();
    res.Files = total;
    const bool saved = pack.Save(tmpDir / "game.sagepak", [&](size_t done, size_t all) {
        // 5..95 % шкалы — пакет: это почти всё время сборки.
        progress(0.05f + 0.9f * (all ? (float)done / (float)all : 1.0f));
        stage("Packing and encrypting: ", std::to_string(done) + " / " + std::to_string(total));
        return !cancelled();
    });
    if (!saved) {
        if (cancelled()) {
            return Abort("Build cancelled");
        }
        return Abort("Could not write the game package: ", pack.LastError());
    }

    // 4. Настройки игры — рядом с exe: их игрок вправе править (разрешение,
    // качество), и прятать их в пакет значило бы отнять у него меню «настройки».
    stage("Finishing");
    const fs::path projCfg = req.ProjectDir / "sage.cfg";
    if (fs::exists(projCfg, ec))
        fs::copy_file(projCfg, tmpDir / "sage.cfg", fs::copy_options::overwrite_existing, ec);
    if (req.CopyProjectFile) {
        fs::copy_file(req.ProjectDir / "project.sageproj", tmpDir / "project.sageproj",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return Abort("Could not copy the project file: ", ec.message());
    }
    // 5. Пользовательские папки (скины, музыка, моды): открытые папки рядом с
    // игрой с README и примерами из <проект>/userfolders/<имя>.
    const size_t folders = usercontent::PrepareForBuild(tmpDir, req.ProjectDir, info.UserFolders);
    if (folders > 0) LOG_INFO("Build") << "Пользовательских папок рядом с игрой: " << folders;

    // 6. Подмена: старая сборка — в резервную папку, новая — на её место; не
    // вышло — старая возвращается. Резерв удаляется только после успеха.
    if (cancelled()) return Abort("Build cancelled");
    const fs::path backup = req.OutputDir / sage::PathFromUtf8(fileName + ".old-" + uniq);
    bool hadOld = false;
    if (fs::exists(gameDir, ec)) {
        fs::rename(gameDir, backup, ec);
        if (ec) return Abort("Cannot replace the previous build: ", ec.message());
        hadOld = true;
    }
    fs::rename(tmpDir, gameDir, ec);
    if (ec) {
        const std::string why = ec.message();
        std::error_code rbEc;
        if (hadOld) fs::rename(backup, gameDir, rbEc);
        return Abort("Cannot replace the previous build: ", why);
    }
    if (hadOld) fs::remove_all(backup, ec);
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
