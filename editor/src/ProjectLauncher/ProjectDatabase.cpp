#include "ProjectDatabase.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "../Localization.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Version.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Sage::Launcher {

namespace {

// Имя файла-дескриптора. Одно на весь редактор (см. Project.h); здесь оно
// повторено ровно потому, что стартовое окно читает ПРОЕКТЫ, КОТОРЫЕ НЕ
// ОТКРЫТЫ, — класса Project для них не существует.
constexpr const char* kDescriptor = "project.sageproj";

// Время файловой системы -> unix-время. Ходить через system_clock приходится
// потому, что у file_time_type своя эпоха и до C++20 перевода стандартом нет.
long long ToUnix(fs::file_time_type stamp) {
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        stamp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (long long)std::chrono::system_clock::to_time_t(sys);
}

long long NowUnix() {
    return (long long)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

// Путь в каноничном виде для СРАВНЕНИЯ записей. Один и тот же проект приходит
// то с «/», то с «\», то с хвостовым разделителем (из файлового диалога, из
// перетаскивания, из аргументов), и без приведения список копил бы дубликаты
// одного проекта.
std::string NormalizePath(const fs::path& p) {
    std::error_code ec;
    fs::path clean = fs::weakly_canonical(p, ec);
    if (ec || clean.empty()) clean = p;
    std::string text = clean.generic_string();
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return text;
}

// Когда проект последний раз трогали. Дескриптор пишется редко, поэтому
// смотрим ещё и на папку сцен: работа в проекте — это правка сцен.
long long ProjectModifiedTime(const fs::path& dir) {
    std::error_code ec;
    long long newest = 0;
    for (const fs::path& probe : {dir / kDescriptor, dir / "scenes", dir}) {
        const auto stamp = fs::last_write_time(probe, ec);
        if (ec) { ec.clear(); continue; }
        newest = std::max(newest, ToUnix(stamp));
    }
    return newest;
}

// Обложка проекта: своя картинка, если автор её положил. Порядок фиксирован,
// первая найденная и выигрывает.
std::string FindThumbnail(const fs::path& dir) {
    std::error_code ec;
    for (const char* name : {"preview.png", "preview.jpg", "thumbnail.png",
                             ".sage/thumbnail.png"}) {
        const fs::path p = dir / name;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) return sage::PathToUtf8(p);
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
//  Типы проектов
// ---------------------------------------------------------------------------

const char* ProjectKindId(ProjectKind kind) {
    switch (kind) {
        case ProjectKind::Scene: return "scene";
        case ProjectKind::Sample: return "sample";
        case ProjectKind::Template: return "template";
        case ProjectKind::Game:
        default: return "game";
    }
}

ProjectKind ProjectKindFromId(const std::string& id) {
    if (id == "scene") return ProjectKind::Scene;
    if (id == "sample") return ProjectKind::Sample;
    if (id == "template") return ProjectKind::Template;
    return ProjectKind::Game;
}

// Подписи ПЕРЕВОДЯТСЯ ЗДЕСЬ, а не на месте показа.
//
// Раньше эти функции отдавали английский ключ, а вызывающий оборачивал его в
// T(). Выглядело чище, а на экране давало английские вкладки при русском
// интерфейсе — и проверка переводов молчала: она видит ЛИТЕРАЛЫ, а литерала в
// месте показа не было, там стоял вызов функции. Ключ и T() обязаны стоять
// рядом, иначе строка выпадает из каталога незаметно.
const char* ProjectKindLabel(ProjectKind kind) {
    switch (kind) {
        case ProjectKind::Scene: return T("Scene");
        case ProjectKind::Sample: return T("Sample");
        case ProjectKind::Template: return T("Template");
        case ProjectKind::Game:
        default: return T("Game");
    }
}

const char* ProjectFilterLabel(ProjectFilter filter) {
    switch (filter) {
        case ProjectFilter::Games: return T("Games");
        case ProjectFilter::Scenes: return T("Scenes");
        case ProjectFilter::Samples: return T("Samples");
        case ProjectFilter::All:
        default: return T("All");
    }
}

const char* ProjectSortLabel(ProjectSort sort) {
    switch (sort) {
        case ProjectSort::Name: return T("By name");
        case ProjectSort::Modified: return T("By date modified");
        case ProjectSort::Recent:
        default: return T("Recently opened");
    }
}

// ---------------------------------------------------------------------------
//  Дескриптор проекта
// ---------------------------------------------------------------------------

bool ProjectDatabase::ReadDescriptor(const fs::path& dir, ProjectEntry& out) {
    const fs::path file = dir / kDescriptor;
    std::ifstream in(file);
    if (!in.is_open()) return false;
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        LOG_WARN("Launcher") << "Дескриптор проекта не читается: " << sage::PathToUtf8(file)
                             << " (" << e.what() << ")";
        return false;
    }
    if (!j.is_object()) return false;

    out.Name = j.value("name", dir.filename().string());
    out.Description = j.value("description", std::string{});
    out.Kind = ProjectKindFromId(j.value("type", std::string{"game"}));
    out.EngineVersion = j.value("engine_version", std::string{});
    // Времени создания у старых проектов нет — тогда его заменяет время файла
    // дескриптора: он пишется ровно один раз, при создании.
    out.Created = j.value("created", (long long)0);
    if (out.Created == 0) {
        std::error_code ec;
        const auto stamp = fs::last_write_time(file, ec);
        if (!ec) out.Created = ToUnix(stamp);
    }
    return true;
}

bool ProjectDatabase::WriteMetadata(const fs::path& dir, ProjectKind kind,
                                    const std::string& description, std::string& err) {
    const fs::path file = dir / kDescriptor;
    json j;
    {
        std::ifstream in(file);
        if (!in.is_open()) { err = T("Project file not found"); return false; }
        try {
            in >> j;
        } catch (const std::exception& e) {
            err = std::string(T("Invalid project file: ")) + e.what();
            return false;
        }
    }
    j["type"] = ProjectKindId(kind);
    if (!description.empty()) j["description"] = description;
    if (!j.contains("created")) j["created"] = NowUnix();
    if (!j.contains("engine_version")) j["engine_version"] = kSageEngineVersion;

    std::ofstream outFile(file);
    if (!outFile.is_open()) { err = T("Failed to write the project file"); return false; }
    outFile << j.dump(2) << "\n";
    return true;
}

bool ProjectDatabase::IsProject(const fs::path& fileOrDir) {
    std::error_code ec;
    fs::path dir = fileOrDir;
    if (fs::is_regular_file(dir, ec)) dir = dir.parent_path();
    ProjectEntry probe;
    return ReadDescriptor(dir, probe);
}

// ---------------------------------------------------------------------------
//  Файл базы
// ---------------------------------------------------------------------------

std::string ProjectDatabase::StoragePath() {
    // Тот же каталог, где лежат настройки редактора и список языков: одно
    // место на все «привычки одного человека» (см. EditorPrefs.h).
    //
    // sage::EnvPath, а не getenv: на русской Windows %APPDATA% приходит в узкое
    // окружение байтами ANSI, и std::filesystem::path на них БРОСАЕТ (см.
    // sage/core/Paths.h). Ровно на этом редактор когда-то не запускался вовсе.
    fs::path base;
    if (const fs::path xdg = sage::EnvPath("XDG_CONFIG_HOME"); !xdg.empty()) base = xdg;
    else if (const fs::path home = sage::EnvPath("HOME"); !home.empty()) base = home / ".config";
    else if (const fs::path appdata = sage::EnvPath("APPDATA"); !appdata.empty()) base = appdata;
    else { std::error_code ec; base = fs::current_path(ec); }
    return sage::PathToUtf8(base / "sage" / "projects.json");
}

void ProjectDatabase::Load() {
    m_items.clear();

    std::vector<std::pair<std::string, long long>> stored; // путь + когда открывали
    {
        std::ifstream in(StoragePath());
        if (in.is_open()) {
            try {
                json root;
                in >> root;
                for (const auto& p : root.value("projects", json::array())) {
                    if (p.is_object()) {
                        stored.emplace_back(p.value("path", std::string{}),
                                            p.value("opened", (long long)0));
                    } else if (p.is_string()) {
                        stored.emplace_back(p.get<std::string>(), 0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("Launcher") << "projects.json повреждён, начинаю с пустого списка: "
                                     << e.what();
                stored.clear();
            }
        }
    }

    // ПЕРЕЕЗД СО СТАРОГО СПИСКА НЕДАВНИХ. У человека, обновившего редактор,
    // список проектов не имеет права оказаться пустым: он читает это как
    // «программа потеряла мои проекты», а не как «формат сменился».
    if (stored.empty()) {
        const fs::path legacy = fs::path(StoragePath()).parent_path() / "recent_projects.json";
        std::ifstream in(legacy);
        if (in.is_open()) {
            try {
                json root;
                in >> root;
                for (const auto& p : root.value("recent", json::array())) {
                    if (p.is_string()) stored.emplace_back(p.get<std::string>(), 0);
                }
                LOG_INFO("Launcher") << "Список проектов перенесён из recent_projects.json: "
                                     << stored.size();
            } catch (const std::exception&) {
                // Испорченный старый файл — не повод не запуститься.
            }
        }
    }

    for (const auto& [path, opened] : stored) {
        if (path.empty()) continue;
        const std::string key = NormalizePath(path);
        if (IndexOf(key) >= 0) continue;   // дубликат: один проект — одна карточка

        ProjectEntry e;
        e.Path = key;
        e.Opened = opened;
        const fs::path dir(path);
        if (!ReadDescriptor(dir, e)) {
            // Папки нет или дескриптор испорчен. Запись остаётся — с пометкой:
            // внешний диск отключают чаще, чем удаляют проекты.
            e.Missing = true;
            e.Name = dir.filename().string();
            if (e.Name.empty()) e.Name = key;
        } else {
            e.Modified = ProjectModifiedTime(dir);
            e.Thumbnail = FindThumbnail(dir);
        }
        e.SearchKey = Lower(e.Name + " " + e.Path + " " + ProjectKindId(e.Kind));
        m_items.push_back(std::move(e));
    }
}

void ProjectDatabase::Save() const {
    const fs::path path = StoragePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Launcher") << "Список проектов не сохранился: " << sage::PathToUtf8(path);
        return;
    }
    json root;
    root["version"] = 1;
    json list = json::array();
    for (const ProjectEntry& e : m_items) {
        json item;
        item["path"] = e.Path;
        item["opened"] = e.Opened;
        list.push_back(std::move(item));
    }
    root["projects"] = std::move(list);
    file << root.dump(2) << "\n";
}

// ---------------------------------------------------------------------------
//  Поиск, отбор, порядок
// ---------------------------------------------------------------------------

int ProjectDatabase::IndexOf(const std::string& path) const {
    const std::string key = NormalizePath(path);
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].Path == key) return (int)i;
    }
    return -1;
}

const ProjectEntry* ProjectDatabase::Find(const std::string& path) const {
    const int index = IndexOf(path);
    return index < 0 ? nullptr : &m_items[(size_t)index];
}

std::vector<int> ProjectDatabase::Query(ProjectFilter filter, const std::string& search,
                                        ProjectSort sort) const {
    const std::string needle = Lower(search);
    std::vector<int> out;
    out.reserve(m_items.size());
    for (size_t i = 0; i < m_items.size(); ++i) {
        const ProjectEntry& e = m_items[i];
        switch (filter) {
            case ProjectFilter::Games:   if (e.Kind != ProjectKind::Game) continue; break;
            case ProjectFilter::Scenes:  if (e.Kind != ProjectKind::Scene) continue; break;
            case ProjectFilter::Samples: if (e.Kind != ProjectKind::Sample) continue; break;
            default: break;
        }
        // Поиск идёт ПО ПАМЯТИ и по заранее приведённой строке: на каждое
        // нажатие клавиши обходить диск (или приводить регистр пятисот путей)
        // нельзя — именно из этого получаются стартовые окна, в которых
        // «поиск тормозит».
        if (!needle.empty() && e.SearchKey.find(needle) == std::string::npos) continue;
        out.push_back((int)i);
    }

    const std::vector<ProjectEntry>& items = m_items;
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
        const ProjectEntry& x = items[(size_t)a];
        const ProjectEntry& y = items[(size_t)b];
        switch (sort) {
            case ProjectSort::Name: return Lower(x.Name) < Lower(y.Name);
            case ProjectSort::Modified: return x.Modified > y.Modified;
            case ProjectSort::Recent:
            default:
                // Никогда не открытые (импортированные только что) идут по
                // времени правки: ноль в «открывали» не означает «старее всех».
                return std::max(x.Opened, x.Modified) > std::max(y.Opened, y.Modified);
        }
    });
    return out;
}

// ---------------------------------------------------------------------------
//  Изменение списка
// ---------------------------------------------------------------------------

void ProjectDatabase::Touch(const std::string& path) {
    const std::string key = NormalizePath(path);
    int index = IndexOf(key);
    if (index < 0) {
        ProjectEntry e;
        e.Path = key;
        const fs::path dir(key);
        if (!ReadDescriptor(dir, e)) {
            e.Missing = true;
            e.Name = dir.filename().string();
        }
        m_items.push_back(std::move(e));
        index = (int)m_items.size() - 1;
    }
    ProjectEntry& e = m_items[(size_t)index];
    e.Opened = NowUnix();
    e.Missing = false;
    e.Modified = ProjectModifiedTime(fs::path(e.Path));
    e.Thumbnail = FindThumbnail(fs::path(e.Path));
    e.SearchKey = Lower(e.Name + " " + e.Path + " " + ProjectKindId(e.Kind));
    Save();
}

void ProjectDatabase::Forget(const std::string& path) {
    const int index = IndexOf(path);
    if (index < 0) return;
    m_items.erase(m_items.begin() + index);
    Save();
}

void ProjectDatabase::Refresh(const std::string& path) {
    const int index = IndexOf(path);
    if (index < 0) return;
    ProjectEntry& e = m_items[(size_t)index];
    const fs::path dir(e.Path);
    if (!ReadDescriptor(dir, e)) {
        e.Missing = true;
    } else {
        e.Missing = false;
        e.Modified = ProjectModifiedTime(dir);
        e.Thumbnail = FindThumbnail(dir);
    }
    e.SearchKey = Lower(e.Name + " " + e.Path + " " + ProjectKindId(e.Kind));
}

bool ProjectDatabase::Import(const fs::path& fileOrDir, std::string& err,
                             std::string* outPath) {
    std::error_code ec;
    fs::path dir = fileOrDir;
    if (fs::is_regular_file(dir, ec)) dir = dir.parent_path();
    if (!fs::exists(dir, ec)) {
        err = std::string(T("There is no such folder: ")) + sage::PathToUtf8(dir);
        return false;
    }
    ProjectEntry probe;
    if (!ReadDescriptor(dir, probe)) {
        // Сообщение называет ФАЙЛ, которого не хватает: «это не проект SAGE»
        // не подсказывает, что делать, а «нет project.sageproj» — подсказывает.
        err = std::string(T("This is not a SAGE project: no project.sageproj in ")) +
              sage::PathToUtf8(dir);
        return false;
    }
    const std::string key = NormalizePath(dir);
    if (outPath) *outPath = key;
    if (IndexOf(key) >= 0) {
        Refresh(key);
        return true;   // уже в списке — не ошибка, просто нечего добавлять
    }

    ProjectEntry e = probe;
    e.Path = key;
    e.Modified = ProjectModifiedTime(dir);
    e.Thumbnail = FindThumbnail(dir);
    e.SearchKey = Lower(e.Name + " " + e.Path + " " + ProjectKindId(e.Kind));
    m_items.push_back(std::move(e));
    Save();
    LOG_INFO("Launcher") << "Проект добавлен в список: " << key;
    return true;
}

bool ProjectDatabase::Duplicate(const std::string& path, const std::string& newName,
                                std::string& err, std::string* outPath) {
    if (newName.empty()) { err = T("Enter a project name"); return false; }
    const fs::path src(NormalizePath(path));
    std::error_code ec;
    if (!fs::exists(src / kDescriptor, ec)) {
        err = std::string(T("Project not found: ")) + sage::PathToUtf8(src);
        return false;
    }
    const fs::path dst = src.parent_path() / newName;
    if (fs::exists(dst, ec)) {
        err = std::string(T("That folder already exists: ")) + sage::PathToUtf8(dst);
        return false;
    }
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    if (ec) {
        err = std::string(T("Failed to copy the project: ")) + ec.message();
        return false;
    }
    // У копии СВОЁ имя: иначе в списке окажутся два проекта с одной вывеской
    // и разными путями — то есть выбор вслепую.
    std::string metaErr;
    {
        json j;
        std::ifstream in(dst / kDescriptor);
        if (in.is_open()) {
            try { in >> j; } catch (const std::exception&) { j = json::object(); }
        }
        j["name"] = newName;
        j["created"] = NowUnix();
        std::ofstream out(dst / kDescriptor);
        if (out.is_open()) out << j.dump(2) << "\n";
    }
    if (outPath) *outPath = NormalizePath(dst);
    if (!Import(dst, metaErr)) { err = metaErr; return false; }
    return true;
}

bool ProjectDatabase::Rename(const std::string& path, const std::string& newName,
                             std::string& err) {
    if (newName.empty()) { err = T("Enter a project name"); return false; }
    const fs::path dir(NormalizePath(path));
    json j;
    {
        std::ifstream in(dir / kDescriptor);
        if (!in.is_open()) { err = T("Project file not found"); return false; }
        try { in >> j; } catch (const std::exception& e) {
            err = std::string(T("Invalid project file: ")) + e.what();
            return false;
        }
    }
    j["name"] = newName;
    std::ofstream out(dir / kDescriptor);
    if (!out.is_open()) { err = T("Failed to write the project file"); return false; }
    out << j.dump(2) << "\n";
    Refresh(path);
    return true;
}

bool ProjectDatabase::DeleteFromDisk(const std::string& path, std::string& err) {
    const fs::path dir(NormalizePath(path));
    std::error_code ec;
    // ПРОВЕРКА ПЕРЕД НЕОБРАТИМЫМ. Стереть «папку, которую кто-то записал в
    // список» нельзя: в списке мог оказаться корень диска или домашняя папка
    // (промах при импорте). Удаляем только то, у чего внутри лежит дескриптор
    // проекта, — то есть то, что этот редактор и создал.
    if (!fs::exists(dir / kDescriptor, ec)) {
        err = std::string(T("This is not a SAGE project folder: ")) + sage::PathToUtf8(dir);
        return false;
    }
    const std::uintmax_t removed = fs::remove_all(dir, ec);
    if (ec) {
        err = std::string(T("Failed to delete the project: ")) + ec.message();
        return false;
    }
    LOG_INFO("Launcher") << "Проект удалён с диска: " << sage::PathToUtf8(dir) << " ("
                         << removed << " файлов)";
    Forget(path);
    return true;
}

// ---------------------------------------------------------------------------
//  Время и система
// ---------------------------------------------------------------------------

std::string FormatStamp(long long unixTime) {
    if (unixTime <= 0) return {};
    const std::time_t t = (std::time_t)unixTime;
    const std::tm* lt = std::localtime(&t);
    if (!lt) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d, %02d:%02d", lt->tm_mday, lt->tm_mon + 1,
                  lt->tm_year + 1900, lt->tm_hour, lt->tm_min);
    return buf;
}

std::string HumanStamp(long long unixTime) {
    if (unixTime <= 0) return {};
    const std::time_t t = (std::time_t)unixTime;
    std::tm when{};
    if (const std::tm* lt = std::localtime(&t)) when = *lt;
    else return {};

    const std::time_t nowT = std::time(nullptr);
    std::tm today{};
    if (const std::tm* lt = std::localtime(&nowT)) today = *lt;

    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", when.tm_hour, when.tm_min);
    const bool sameYear = when.tm_year == today.tm_year;
    if (sameYear && when.tm_yday == today.tm_yday) return std::string(T("Today")) + ", " + clock;
    if (sameYear && when.tm_yday + 1 == today.tm_yday) return std::string(T("Yesterday")) + ", " + clock;
    return FormatStamp(unixTime);
}

bool OpenWithSystem(const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target, ec)) return false;
#ifdef _WIN32
    // ShellExecuteW, а не system(): узкий system() на Windows идёт через ANSI,
    // и папка «C:\Users\Владимир\...» до проводника просто не доезжает — ровно
    // та же беда, из-за которой в редакторе запрещён std::getenv для путей.
    const std::wstring wide = target.wstring();
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)rc > 32;
#else
    // Одинарные кавычки внутри пути ломают команду — заменяем их безопасной
    // склейкой '\''.
    std::string quoted = "'";
    for (char c : sage::PathToUtf8(target)) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    const std::string cmd = "xdg-open " + quoted + " >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

// Папка и файл открываются ОДНИМ И ТЕМ ЖЕ вызовом системы: и «показать папку», и
// «открыть скрипт» — это просьба «открой это тем, чем открываешь обычно». Две
// функции ради одной строки различия разошлись бы при первой же правке под
// Windows.
bool RevealInFileManager(const fs::path& dir) { return OpenWithSystem(dir); }

} // namespace Sage::Launcher
