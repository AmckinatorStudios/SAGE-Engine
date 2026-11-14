#include "GameBuilder.h"

#include <functional>

#include "Localization.h"
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
    // Редактор закрывают посреди сборки — задача дорабатывает до отмены, а не
    // обрывается вместе с процессом на полуслове записи пакета.
    m_cancel = true;
    Join();
}

void GameBuilder::Join() {
    m_job.Wait();
    m_job = {};
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
    m_job = sage::jobs::Run([this, request] {
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
    }, sage::jobs::JobOptions("game.build", sage::jobs::Category::Editor, sage::jobs::Lane::IO));
    if (m_job.IsDone() && !m_job.Succeeded()) {
        // Система задач не приняла сборку (редактор закрывается).
        std::lock_guard<std::mutex> lk(m_mx);
        m_error = T("the job was canceled");
        m_finished = true;
        m_running = false;
    }
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

namespace {

// Ключ движка -> подпись на языке интерфейса (движок языка не знает).
const char* BuildText(const std::string& key) {
    const std::pair<const char*, const char*> table[] = {
        {"Preparing the game folder", T("Preparing the game folder")},
        {"Copying the player", T("Copying the player")},
        {"Collecting files", T("Collecting files")},
        {"Packing and encrypting: ", T("Packing and encrypting: ")},
        {"Finishing", T("Finishing")},
        {"Done", T("Done")},
        {"Build cancelled", T("Build cancelled")},
        {"Cannot create the game folder: ", T("Cannot create the game folder: ")},
        {"Player copy failed: ", T("Player copy failed: ")},
        {"Could not write the game package: ", T("Could not write the game package: ")},
        {"Could not copy the project file: ", T("Could not copy the project file: ")},
    };
    for (const auto& [k, text] : table)
        if (key == k) return text;
    return key.c_str();
}

} // namespace

bool GameBuilder::Run(const Request& req, Shared& sh) {
    sage::assets::GameBuildHooks hooks;
    hooks.Progress = sh.Progress;
    hooks.Stage = [&sh](const std::string& key, const std::string& detail) {
        sh.Stage(std::string(BuildText(key)) + detail);
    };
    hooks.Cancelled = sh.Cancelled;
    const sage::assets::GameBuildResult r = sage::assets::BuildGame(req, hooks);
    sh.GameDir = r.GameDir;
    if (!r.Ok) sh.Error = std::string(BuildText(r.ErrorKey)) + r.ErrorDetail;
    return r.Ok;
}
