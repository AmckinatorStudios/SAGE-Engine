#include "sage/scripting/Plugins.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"
#include "sage/scene/SceneValueJson.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace sage::plugins {

namespace {

std::string Str(const json& j, const char* key, const std::string& fallback = {}) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

// Настройка в файле — ПРОСТЫМ значением («"speed": 5»), если оно простое: файл
// пишут и руками, и «{"type":"float","value":5}» вместо пятёрки — порог, ради
// которого плагины на Lua и затевались. Вектор, цвет, ссылка — полной записью.
json SettingToJson(const sage::vars::Value& v) {
    switch (v.Type()) {
        case sage::vars::Kind::Bool: return v.AsBool();
        case sage::vars::Kind::Int: return v.AsInt();
        case sage::vars::Kind::Float: return v.AsFloat();
        case sage::vars::Kind::String: return v.AsString();
        default: return sage::scene::ValueToJson(v);
    }
}

sage::vars::Value SettingFromJson(const json& j) {
    if (j.is_boolean()) return sage::vars::Value(j.get<bool>());
    if (j.is_number_integer()) return sage::vars::Value(j.get<int>());
    if (j.is_number()) return sage::vars::Value(j.get<float>());
    if (j.is_string()) return sage::vars::Value(j.get<std::string>());
    return sage::scene::ValueFromJson(j);
}

} // namespace

Manifest ParseManifest(const std::string& text, const fs::path& dir) {
    Manifest m;
    m.Dir = dir;
    m.Id = sage::PathToUtf8(dir.filename());
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        m.Error = "plugin.json: не JSON-объект (проверьте запятые и кавычки)";
        return m;
    }
    m.Id = Str(j, "id", m.Id);
    m.Name = Str(j, "name", m.Id);
    m.Version = Str(j, "version", m.Version);
    m.Author = Str(j, "author");
    m.Description = Str(j, "description");
    m.Runtime = Str(j, "runtime");
    m.Editor = Str(j, "editor");
    if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) m.Enabled = it->get<bool>();
    if (auto it = j.find("settings"); it != j.end() && it->is_object())
        for (auto& [key, value] : it->items()) m.Settings.Set(key, SettingFromJson(value));
    if (!m.HasRuntime() && !m.HasEditor())
        m.Error = "plugin.json: не указан ни \"runtime\", ни \"editor\" — плагину нечего запускать";
    return m;
}

std::string ManifestToText(const Manifest& m) {
    // Порядок ключей — как их читает человек, открывший файл: сначала что это,
    // потом что запускать. ordered_json держит порядок вставки.
    nlohmann::ordered_json j;
    j["id"] = m.Id;
    j["name"] = m.Name.empty() ? m.Id : m.Name;
    j["version"] = m.Version;
    if (!m.Author.empty()) j["author"] = m.Author;
    if (!m.Description.empty()) j["description"] = m.Description;
    j["enabled"] = m.Enabled;
    if (m.HasRuntime()) j["runtime"] = m.Runtime;
    if (m.HasEditor()) j["editor"] = m.Editor;
    if (!m.Settings.Empty()) {
        nlohmann::ordered_json s = nlohmann::ordered_json::object();
        for (const sage::vars::Var& v : m.Settings.All()) s[v.Name] = SettingToJson(v.Data);
        j["settings"] = s;
    }
    return j.dump(2) + "\n";
}

bool ReadManifest(const fs::path& dir, Manifest& out) {
    std::string text;
    if (!sage::assets::vfs::ReadText(sage::PathToUtf8(dir / kManifestFile), text)) return false;
    out = ParseManifest(text, dir);
    return true;
}

bool WriteManifest(const Manifest& m, std::string& error) {
    std::error_code ec;
    fs::create_directories(m.Dir, ec);
    std::ofstream f(m.Dir / kManifestFile, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "не удалось записать " + sage::PathToUtf8(m.Dir / kManifestFile);
        return false;
    }
    f << ManifestToText(m);
    return (bool)f;
}

std::vector<Manifest> Scan(const fs::path& pluginsDir) {
    std::vector<Manifest> out;
    if (pluginsDir.empty()) return out;
    for (const std::string& name : sage::assets::vfs::ListDirs(sage::PathToUtf8(pluginsDir))) {
        Manifest m;
        if (ReadManifest(pluginsDir / sage::PathFromUtf8(name), m)) out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Manifest& a, const Manifest& b) { return a.Id < b.Id; });
    return out;
}

std::string MakeId(const std::string& name) {
    // Кириллица — латиницей по звучанию: «Счётчик монет» -> «schetchik_monet»,
    // а не «_______». Таблица по кодам UTF-8 двухбайтных букв.
    static const char* kRu[] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m",
                                "n", "o", "p", "r", "s", "t", "u", "f", "h", "ts", "ch", "sh",
                                "sch", "", "y", "", "e", "yu", "ya"};
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (std::isalnum(c)) {
            out += (char)std::tolower(c);
            continue;
        }
        if ((c == 0xD0 || c == 0xD1) && i + 1 < name.size()) {
            const unsigned char d = (unsigned char)name[i + 1];
            int index = -1;   // номер буквы а..я
            if (c == 0xD0 && d >= 0x90 && d <= 0xAF) index = d - 0x90;        // А..Я
            else if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) index = d - 0xB0;   // а..п
            else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) index = d - 0x80 + 16; // р..я
            else if ((c == 0xD0 && d == 0x81) || (c == 0xD1 && d == 0x91)) out += "e"; // Ё/ё
            if (index >= 0) out += kRu[index];
            ++i;
            continue;
        }
        if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "plugin";
    if (std::isdigit((unsigned char)out[0])) out = "p_" + out;
    return out;
}

} // namespace sage::plugins
