#include "RecentProjects.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string RecentProjects::StoragePath() {
    // XDG-стиль на Unix, APPDATA на Windows, cwd как последний шанс.
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) base = xdg;
    else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".config";
    else if (const char* appdata = std::getenv("APPDATA")) base = appdata;
    else base = fs::current_path();
    return (base / "sage" / "recent_projects.json").string();
}

void RecentProjects::Load() {
    m_paths.clear();
    std::ifstream file(StoragePath());
    if (!file.is_open()) return; // первого запуска файла ещё нет — норма
    try {
        json root;
        file >> root;
        for (const auto& p : root.value("recent", json::array())) {
            if (p.is_string()) m_paths.push_back(p.get<std::string>());
        }
    } catch (const std::exception& e) {
        LOG_WARN("Editor") << "recent_projects.json повреждён, игнорирую: " << e.what();
        m_paths.clear();
    }
}

void RecentProjects::Save() const {
    fs::path path = StoragePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Editor") << "Не удалось сохранить список недавних проектов: " << path.string();
        return;
    }
    json root;
    root["recent"] = m_paths;
    file << root.dump(2) << "\n";
}

void RecentProjects::Add(const std::string& projectDir) {
    m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), projectDir), m_paths.end());
    m_paths.insert(m_paths.begin(), projectDir);
    if (m_paths.size() > 10) m_paths.resize(10);
    Save();
}

void RecentProjects::Remove(const std::string& projectDir) {
    m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), projectDir), m_paths.end());
    Save();
}
