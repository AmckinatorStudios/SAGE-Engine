#include "FolderColors.h"
#include "Localization.h"

#include <map>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace sage::editor::foldercolors {

namespace {

fs::path g_projectDir;
// Ключ — путь ОТНОСИТЕЛЬНО проекта, в UTF-8: абсолютный путь развалился бы от
// первого же переезда проекта в другую папку (или на другую машину).
std::map<std::string, glm::vec3> g_colors;

fs::path FilePath() { return g_projectDir / "folder_colors.json"; }

std::string KeyOf(const fs::path& folder) {
    if (g_projectDir.empty()) return {};
    std::error_code ec;
    fs::path rel = fs::relative(folder, g_projectDir, ec);
    if (ec || rel.empty()) return {};
    // Разделитель ВСЕГДА '/': файл переезжает между Windows и Linux вместе с
    // проектом, и обратный слэш в ключе сделал бы половину меток невидимой.
    std::string key = sage::PathToUtf8(rel.generic_string());
    if (key.rfind("..", 0) == 0) return {};   // не наша папка
    return key;
}

void Save() {
    if (g_projectDir.empty()) return;
    json root = json::object();
    for (const auto& [key, c] : g_colors) root[key] = {c.r, c.g, c.b};
    std::ofstream out(FilePath());
    if (!out) {
        LOG_WARN("Editor") << "Цвета папок не записались: " << sage::PathToUtf8(FilePath());
        return;
    }
    out << root.dump(2) << '\n';
}

void Load() {
    g_colors.clear();
    std::ifstream in(FilePath());
    if (!in) return;   // файла нет — это норма, просто меток ещё не ставили
    try {
        json root = json::parse(in);
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!it.value().is_array() || it.value().size() != 3) continue;
            g_colors[it.key()] = glm::vec3(it.value()[0].get<float>(), it.value()[1].get<float>(),
                                           it.value()[2].get<float>());
        }
    } catch (const std::exception& e) {
        // Испорченный файл меток — не повод не открыть проект: цвета это
        // украшение, а не содержимое.
        LOG_WARN("Editor") << "Цвета папок не прочитались: " << e.what();
    }
}

} // namespace

std::vector<Tint> Palette() {
    return {
        {T("Grey"),   {0.62f, 0.72f, 0.85f}},
        {T("Red"),    {0.90f, 0.35f, 0.35f}},
        {T("Orange"), {0.95f, 0.60f, 0.25f}},
        {T("Yellow"), {0.93f, 0.83f, 0.30f}},
        {T("Green"),  {0.45f, 0.80f, 0.45f}},
        {T("Cyan"),   {0.35f, 0.78f, 0.85f}},
        {T("Blue"),   {0.40f, 0.60f, 0.95f}},
        {T("Purple"), {0.72f, 0.50f, 0.90f}},
    };
}

void SetProject(const fs::path& projectDir) {
    if (projectDir == g_projectDir) return;
    g_projectDir = projectDir;
    Load();
}

bool Get(const fs::path& folder, glm::vec3& out) {
    const std::string key = KeyOf(folder);
    if (key.empty()) return false;
    auto it = g_colors.find(key);
    if (it == g_colors.end()) return false;
    out = it->second;
    return true;
}

void Set(const fs::path& folder, const glm::vec3& color) {
    const std::string key = KeyOf(folder);
    if (key.empty()) return;
    g_colors[key] = color;
    Save();
}

void Clear(const fs::path& folder) {
    const std::string key = KeyOf(folder);
    if (key.empty()) return;
    if (g_colors.erase(key)) Save();
}

void Rename(const fs::path& from, const fs::path& to) {
    const std::string oldKey = KeyOf(from);
    const std::string newKey = KeyOf(to);
    if (oldKey.empty() || newKey.empty()) return;
    auto it = g_colors.find(oldKey);
    if (it == g_colors.end()) return;
    const glm::vec3 color = it->second;
    g_colors.erase(it);
    g_colors[newKey] = color;
    Save();
}

} // namespace sage::editor::foldercolors
