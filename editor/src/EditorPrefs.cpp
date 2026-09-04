#include "EditorPrefs.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "sage/core/Paths.h"

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
    // sage::EnvPath, а не getenv: на Windows «C:\Users\Вова\AppData\Roaming»
    // приходит в узкое окружение байтами ANSI, и std::filesystem::path на них
    // бросает исключение (см. Paths.h). Именно здесь редактор и умирал на
    // русской Windows — до создания окна, ещё до первой панели.
    if (const fs::path xdg = sage::EnvPath("XDG_CONFIG_HOME"); !xdg.empty()) base = xdg;
    else if (const fs::path home = sage::EnvPath("HOME"); !home.empty()) base = home / ".config";
    else if (const fs::path appdata = sage::EnvPath("APPDATA"); !appdata.empty()) base = appdata;
    else { std::error_code ec; base = fs::current_path(ec); }
    return sage::PathToUtf8(base / "sage" / "editor_prefs.json");
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

float GetFloat(const std::string& key, float fallback) {
    const json& r = Root();
    auto it = r.find(key);
    if (it == r.end() || !it->is_number()) return fallback;
    return it->get<float>();
}

void SetFloat(const std::string& key, float value) {
    if (Root().contains(key) && GetFloat(key, value + 1.0f) == value) return;
    Root()[key] = value;
    Flush();
}

std::string GetString(const std::string& key, const std::string& fallback) {
    const json& r = Root();
    auto it = r.find(key);
    if (it == r.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

void SetString(const std::string& key, const std::string& value) {
    if (GetString(key, value + "\x01") == value) return; // без лишней записи
    Root()[key] = value;
    Flush();
}

} // namespace sage::editor::prefs
