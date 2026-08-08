#include "Localization.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sage::editor {
namespace {

// Встроенный русский каталог: пары {ключ, перевод}. Отдельным файлом, потому
// что это ДАННЫЕ — их правят переводом, а не кодом, и мешать их с логикой
// значит каждый раз просматривать пятьсот строк текста ради десяти строк кода.
#include "lang_ru.inl"

struct Catalog {
    LanguageInfo Info;
    std::unordered_map<std::string, std::string> Strings;
};

std::vector<Catalog>& Catalogs() {
    static std::vector<Catalog> catalogs;
    return catalogs;
}

std::string& CurrentCode() {
    static std::string code = "en";
    return code;
}

const Catalog* CurrentCatalog() {
    for (const Catalog& c : Catalogs()) {
        if (c.Info.Code == CurrentCode()) return &c;
    }
    return nullptr;
}

// Готовые строки: ключ кэша — «код языка + \x1f + исходный ключ».
//
// Кэш НЕ ЧИСТИТСЯ при смене языка, и это осознанно. ImGui берёт подписи как
// const char*, а вызывающие вправе положить результат в static; очистка
// оставила бы им висячий указатель, и падение случилось бы не при смене языка,
// а когда-нибудь позже, в чужом месте. Память тут — пара сотен килобайт на все
// языки сразу.
std::unordered_map<std::string, std::string>& ResolvedCache() {
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

std::vector<std::string>& Missing() {
    static std::vector<std::string> missing;
    return missing;
}

std::unordered_set<std::string>& MissingSeen() {
    static std::unordered_set<std::string> seen;
    return seen;
}

void NoteMissing(const std::string& key) {
    if (key.empty()) return;
    if (!MissingSeen().insert(key).second) return;
    Missing().push_back(key);
}

Catalog& CatalogFor(const std::string& code, const std::string& name) {
    for (Catalog& c : Catalogs()) {
        if (c.Info.Code == code) return c;
    }
    Catalogs().push_back(Catalog{LanguageInfo{code, name}, {}});
    return Catalogs().back();
}

// Внешние каталоги: assets/lang/<код>.json вида
//   { "name": "Deutsch", "strings": { "Open project": "Projekt öffnen" } }
//
// Ими можно и поправить встроенный перевод (строки накладываются поверх), и
// добавить язык целиком. Отсутствие каталога — норма, а не ошибка: редактор
// обязан работать без ассетов.
void LoadExternalCatalogs() {
    std::error_code ec;
    const fs::path dir = "assets/lang";
    if (!fs::is_directory(dir, ec)) return;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const std::string code = entry.path().stem().string();
        std::ifstream file(entry.path());
        if (!file.is_open()) continue;
        try {
            json root;
            file >> root;
            Catalog& catalog = CatalogFor(code, root.value("name", code));
            if (root.contains("name")) catalog.Info.Name = root["name"].get<std::string>();
            for (const auto& [key, value] : root.value("strings", json::object()).items()) {
                if (value.is_string()) catalog.Strings[key] = value.get<std::string>();
            }
            LOG_INFO("Editor") << "Каталог перевода: " << entry.path().string() << " ("
                               << catalog.Strings.size() << " строк)";
        } catch (const std::exception& e) {
            LOG_WARN("Editor") << "Каталог перевода " << entry.path().string()
                               << " повреждён, игнорирую: " << e.what();
        }
    }
}

std::string PrefsPath() {
    // Тот же путь, что у списка недавних проектов (см. RecentProjects.cpp):
    // настройки редактора живут рядом друг с другом, а не по файлу на функцию.
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) base = xdg;
    else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".config";
    else if (const char* appdata = std::getenv("APPDATA")) base = appdata;
    else base = fs::current_path();
    return (base / "sage" / "editor_prefs.json").string();
}

std::string LoadSavedLanguage() {
    std::ifstream file(PrefsPath());
    if (!file.is_open()) return {};
    try {
        json root;
        file >> root;
        return root.value("language", std::string());
    } catch (const std::exception&) {
        return {}; // повреждённые настройки — не повод не запуститься
    }
}

void SaveLanguage(const std::string& code) {
    const fs::path path = PrefsPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // Читаем-меняем-пишем: в этом файле со временем окажется не только язык, и
    // перезапись целиком стирала бы соседние настройки.
    json root = json::object();
    {
        std::ifstream in(path);
        if (in.is_open()) {
            try { in >> root; } catch (const std::exception&) { root = json::object(); }
            if (!root.is_object()) root = json::object();
        }
    }
    root["language"] = code;
    std::ofstream out(path);
    if (out.is_open()) out << root.dump(2) << "\n";
}

} // namespace

void InitLocalization() {
    if (!Catalogs().empty()) return;

    // Английский — каталог БЕЗ строк: ключ и есть английский текст. Пустой
    // каталог здесь не заглушка, а точное описание: переводить не на что.
    CatalogFor("en", "English");

    Catalog& ru = CatalogFor("ru", "Русский");
    for (const auto& pair : kRussianStrings) ru.Strings[pair.Key] = pair.Value;

    LoadExternalCatalogs();

    std::string wanted = LoadSavedLanguage();
    if (wanted.empty()) {
        if (const char* env = std::getenv("SAGE_EDITOR_LANG")) wanted = env;
    }
    if (!wanted.empty()) {
        for (const Catalog& c : Catalogs()) {
            if (c.Info.Code == wanted) { CurrentCode() = wanted; break; }
        }
    }
}

const std::vector<LanguageInfo>& AvailableLanguages() {
    static std::vector<LanguageInfo> list;
    list.clear();
    for (const Catalog& c : Catalogs()) list.push_back(c.Info);
    return list;
}

const std::string& CurrentLanguageCode() { return CurrentCode(); }

bool SetLanguage(const std::string& code) {
    for (const Catalog& c : Catalogs()) {
        if (c.Info.Code != code) continue;
        CurrentCode() = code;
        SaveLanguage(code);
        return true;
    }
    return false;
}

const char* T(const char* key) {
    if (key == nullptr) return "";
    const Catalog* catalog = CurrentCatalog();
    // Английский (или неизвестный язык) — ключ и есть текст. Возврат самого
    // key'я, а не копии: указатель приходит из литерала в исходнике и живёт
    // столько же, сколько программа.
    if (catalog == nullptr || catalog->Strings.empty()) return key;

    const std::string full(key);
    const std::string cacheKey = CurrentCode() + '\x1f' + full;
    auto cached = ResolvedCache().find(cacheKey);
    if (cached != ResolvedCache().end()) return cached->second.c_str();

    // Видимая часть — до «##». Скрытый идентификатор ImGui переводить нельзя:
    // от него зависит, узнает ли библиотека виджет между кадрами (а значит —
    // размеры окон в imgui.ini и раскрытые секции).
    const size_t hidden = full.find("##");
    const std::string visible = (hidden == std::string::npos) ? full : full.substr(0, hidden);
    const std::string suffix = (hidden == std::string::npos) ? std::string() : full.substr(hidden);

    std::string result;
    auto it = catalog->Strings.find(visible);
    if (it != catalog->Strings.end()) {
        result = it->second + suffix;
    } else {
        // Пустая видимая часть — это чистый идентификатор («##list»), переводить
        // там нечего, и в список пропусков он не попадает.
        if (!visible.empty()) NoteMissing(visible);
        result = full;
    }
    return ResolvedCache().emplace(cacheKey, std::move(result)).first->second.c_str();
}

const std::vector<std::string>& MissingKeys() { return Missing(); }

void DumpMissingKeys(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Editor") << "не удалось записать список непереведённых строк: " << path;
        return;
    }
    std::vector<std::string> sorted = Missing();
    std::sort(sorted.begin(), sorted.end());
    for (const std::string& key : sorted) file << key << "\n";
    LOG_INFO("Editor") << "непереведённых строк: " << sorted.size() << " -> " << path;
}

} // namespace sage::editor
