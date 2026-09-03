#include "EditorPrefs.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sage::editor::prefs {

namespace {

// Файл читается ОДИН РАЗ за запуск и дальше живёт в памяти. Иначе каждое
// обращение к настройке (а состав тулбара спрашивается каждый кадр) означало бы
// открытие файла и разбор JSON — по разу на кнопку, шестьдесят раз в секунду.
json& Root() {
    static json root = [] {
        json r = json::object();
        std::ifstream in(Path());
        if (in.is_open()) {
            try { in >> r; } catch (const std::exception&) { r = json::object(); }
            if (!r.is_object()) r = json::object();
        }
        return r;
    }();
    return root;
}

// Запись — сразу на диск: редактор закрывают крестиком и убивают из
// диспетчера задач, и «сохраним при выходе» означало бы «обычно не сохраним».
void Flush() {
    const fs::path path = Path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (out.is_open()) out << Root().dump(2) << "\n";
}

} // namespace

std::string Path() {
    // Тот же путь, что у языка и списка недавних проектов: настройки редактора
    // лежат рядом друг с другом, а не по файлу на функцию.
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) base = xdg;
    else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".config";
    else if (const char* appdata = std::getenv("APPDATA")) base = appdata;
    else base = fs::current_path();
    return (base / "sage" / "editor_prefs.json").string();
}

bool GetBool(const std::string& key, bool fallback) {
    const json& r = Root();
    auto it = r.find(key);
    if (it == r.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

void SetBool(const std::string& key, bool value) {
    if (GetBool(key, !value) == value && Root().contains(key)) return; // без лишней записи
    Root()[key] = value;
    Flush();
}

int GetInt(const std::string& key, int fallback) {
    const json& r = Root();
    auto it = r.find(key);
    if (it == r.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

void SetInt(const std::string& key, int value) {
    if (Root().contains(key) && GetInt(key, value + 1) == value) return;
    Root()[key] = value;
    Flush();
}

} // namespace sage::editor::prefs
