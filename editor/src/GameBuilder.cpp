#include "GameBuilder.h"

#include <functional>

#include "Localization.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

struct GameBuilder::Shared {
    std::function<void(float)> Progress;
    std::function<void(const std::string&)> Stage;
    std::function<bool()> Cancelled;
    std::string Error;
    fs::path GameDir;
};

GameBuilder::~GameBuilder() {
    // Редактор закрывают посреди сборки — поток дорабатывает до отмены, а не
    // обрывается вместе с процессом на полуслове записи пакета.
    m_cancel = true;
    Join();
}

void GameBuilder::Join() {
    if (m_thread.joinable()) m_thread.join();
}

std::string GameBuilder::Stage() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_stage;
}

std::string GameBuilder::Error() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_error;
}

fs::path GameBuilder::GameDir() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_gameDir;
}

bool GameBuilder::Start(const Request& request) {
    if (m_running) return false;
    Join();
    m_cancel = false;
    m_finished = false;
    m_ok = false;
    m_progress = 0.0f;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stage.clear();
        m_error.clear();
        m_gameDir.clear();
    }
    m_running = true;
    m_thread = std::thread([this, request] {
        Shared shared;
        shared.Progress = [this](float p) { m_progress = p; };
        shared.Stage = [this](const std::string& s) {
            std::lock_guard<std::mutex> lk(m_mx);
            m_stage = s;
        };
        shared.Cancelled = [this] { return m_cancel.load(); };
        const bool ok = Run(request, shared);
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_error = shared.Error;
            m_gameDir = shared.GameDir;
        }
        m_ok = ok;
        m_finished = true;
        m_running = false;
    });
    return true;
}

bool GameBuilder::RunSync(const Request& request, std::string& err) {
    Shared shared;
    shared.Progress = [](float) {};
    shared.Stage = [](const std::string&) {};
    shared.Cancelled = [] { return false; };
    const bool ok = Run(request, shared);
    err = shared.Error;
    return ok;
}

bool GameBuilder::Run(const Request& req, Shared& sh) {
    std::error_code ec;
    const std::string exeSuffix = req.Player.extension().string();

    // 1. Папка игры. Пересборка затирает прошлую: это артефакт, не данные.
    sh.Stage(T("Preparing the game folder"));
    sh.Progress(0.02f);
    const fs::path gameDir = req.OutputDir / sage::PathFromUtf8(req.ProjectName);
    sh.GameDir = gameDir;
    fs::remove_all(gameDir, ec);
    ec.clear();
    fs::create_directories(gameDir, ec);
    if (ec) {
        sh.Error = std::string(T("Cannot create the game folder: ")) + sage::PathToUtf8(gameDir) +
                   " (" + ec.message() + ")";
        return false;
    }

    // 2. Исполняемый файл игры и его библиотеки. На Windows плеер, собранный
    // MinGW, тянет рядом с собой .dll (libstdc++ и прочие): без них игра у
    // игрока не запускалась бы вовсе — с сообщением системы, а не движка.
    sh.Stage(T("Copying the player"));
    fs::copy_file(req.Player, gameDir / sage::PathFromUtf8(req.ProjectName + exeSuffix),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        sh.Error = std::string(T("Player copy failed: ")) + ec.message();
        return false;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(req.Player.parent_path(), ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (e.is_regular_file(ec) && ext == ".dll")
            fs::copy_file(e.path(), gameDir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (sh.Cancelled()) { sh.Error = T("Build cancelled"); return false; }

    // 3. Пакет: ресурсы движка, затем проект (его файл того же имени главнее —
    // PackWriter заменяет запись), затем оглавление базы ассетов.
    sh.Stage(T("Collecting files"));
    sage::assets::PackWriter pack;
    const fs::path runtimeAssets = req.Player.parent_path() / "assets";
    pack.AddDirectory(runtimeAssets, {}, "assets");
    // Папка сборок ВНУТРИ проекта (по умолчанию <проект>/Builds) в пакет не
    // идёт: иначе каждая следующая сборка везла бы в себе предыдущую игру —
    // её exe и пакет, — и размер рос бы с каждым нажатием «Собрать».
    const size_t projectFiles =
        pack.AddDirectory(req.ProjectDir, {".meta"}, "", {req.OutputDir, req.ProjectDir / "Builds"});
    if (!req.AssetIndex.empty())
        pack.Add("assetdb.json", std::vector<uint8_t>(req.AssetIndex.begin(), req.AssetIndex.end()));
    LOG_INFO("Editor") << "Сборка игры: в пакет идут " << pack.Count() << " файлов (проекта — "
                       << projectFiles << ")";

    const size_t total = pack.Count();
    const bool saved = pack.Save(gameDir / "game.sagepak", [&](size_t done, size_t all) {
        // 5..95 % шкалы — пакет: это почти всё время сборки.
        sh.Progress(0.05f + 0.9f * (all ? (float)done / (float)all : 1.0f));
        sh.Stage(std::string(T("Packing and encrypting: ")) + std::to_string(done) + " / " +
                 std::to_string(total));
        return !sh.Cancelled();
    });
    if (!saved) {
        sh.Error = sh.Cancelled() ? std::string(T("Build cancelled"))
                                  : std::string(T("Could not write the game package: ")) +
                                        pack.LastError();
        if (sh.Cancelled()) fs::remove_all(gameDir, ec);
        return false;
    }

    // 4. Настройки игры — рядом с exe: их игрок вправе править (разрешение,
    // качество), и прятать их в пакет значило бы отнять у него меню «настройки».
    sh.Stage(T("Finishing"));
    const fs::path projCfg = req.ProjectDir / "sage.cfg";
    if (fs::exists(projCfg, ec))
        fs::copy_file(projCfg, gameDir / "sage.cfg", fs::copy_options::overwrite_existing, ec);
    if (req.CopyProjectFile) {
        fs::copy_file(req.ProjectDir / "project.sageproj", gameDir / "project.sageproj",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            sh.Error = std::string(T("Could not copy the project file: ")) + ec.message();
            return false;
        }
    }
    sh.Progress(1.0f);
    sh.Stage(T("Done"));
    LOG_INFO("Editor") << "Game built: " << sage::PathToUtf8(gameDir);
    return true;
}
