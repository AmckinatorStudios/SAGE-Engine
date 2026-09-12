#include "CodeEditorApp.h"

#include <cstdlib>
#include <mutex>

#include "EditorPrefs.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sage::editor::codeapp {
namespace {

// Кандидаты. Порядок — это порядок в меню; сверху то, чем пишут на Lua чаще.
//
// Команда у всех — ИМЯ в PATH, а не полный путь: установщики этих программ
// прописывают себя в PATH сами, а зашитый путь («C:\Program Files\...») ломается
// от одной нестандартной установки и от каждого обновления.
struct Candidate {
    const char* Id;
    const char* Name;
    const char* Command;
    const char* ArgsWithLine;
    const char* Args;
};

// {file} и {line} подставляются, кавычки расставляет ExpandArgs: в пути
// пробелы — норма («SAGE Projects»), а кириллица — тем более.
const Candidate kCandidates[] = {
    // VS Code: -g file:line — единственный способ попросить строку; без -g он
    // открыл бы файл с именем «hero.lua:42», которого нет.
    {"vscode",   "Visual Studio Code", "code",     "-g {file}:{line}", "{file}"},
    {"vscodium", "VSCodium",           "codium",   "-g {file}:{line}", "{file}"},
    {"cursor",   "Cursor",             "cursor",   "-g {file}:{line}", "{file}"},
    {"sublime",  "Sublime Text",       "subl",     "{file}:{line}",    "{file}"},
    {"zed",      "Zed",                "zed",      "{file}:{line}",    "{file}"},
    // Семейство JetBrains: --line N файл.
    {"clion",    "CLion",              "clion",    "--line {line} {file}", "{file}"},
    {"rider",    "Rider",              "rider",    "--line {line} {file}", "{file}"},
    {"idea",     "IntelliJ IDEA",      "idea",     "--line {line} {file}", "{file}"},
    {"pycharm",  "PyCharm",            "pycharm",  "--line {line} {file}", "{file}"},
    {"npp",      "Notepad++",          "notepad++", "-n{line} {file}",  "{file}"},
    {"kate",     "Kate",               "kate",     "-l {line} {file}", "{file}"},
    {"gedit",    "gedit",              "gedit",    "+{line} {file}",   "{file}"},
};

// Есть ли такая программа. Спрашиваем СИСТЕМУ, а не гадаем по путям установки.
bool Exists(const std::string& command) {
#ifdef _WIN32
    // SearchPathW ищет ровно там же, где будет искать запуск: PATH, текущая
    // папка, системные каталоги. Широкая версия — потому что PATH у человека с
    // русским именем пользователя содержит кириллицу, и узкая вернула бы мусор.
    const std::wstring wide(command.begin(), command.end());
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, wide.c_str(), L".exe", MAX_PATH, found, nullptr) > 0) return true;
    return SearchPathW(nullptr, wide.c_str(), L".cmd", MAX_PATH, found, nullptr) > 0;
#else
    // command -v, а не which: which есть не везде, а command -v — встроенная
    // команда оболочки. Вывод гасится: это проверка, а не сообщение человеку.
    const std::string probe = "command -v " + command + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
#endif
}

std::vector<App>& Cache() {
    static std::vector<App> cache;
    return cache;
}

bool& Scanned() {
    static bool scanned = false;
    return scanned;
}

void Scan() {
    std::vector<App>& out = Cache();
    out.clear();
    for (const Candidate& c : kCandidates) {
        if (!Exists(c.Command)) continue;
        App app;
        app.Id = c.Id;
        app.Name = c.Name;
        app.Command = c.Command;
        app.ArgsWithLine = c.ArgsWithLine;
        app.Args = c.Args;
        out.push_back(std::move(app));
    }
    Scanned() = true;
    LOG_INFO("Editor") << "Редакторы кода на этой машине: " << out.size();
    for (const App& a : out) LOG_DEBUG("Editor") << "  " << a.Id << " -> " << a.Command;
}

// Путь в кавычках для командной строки. Причина та же, по которой в движке
// запрещён getenv для путей: «C:\Users\Владимир\SAGE Projects\hero.lua» без
// кавычек разъезжается по пробелу на три аргумента.
std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

} // namespace

std::string ExpandArgs(const std::string& pattern, const fs::path& file, int line) {
    const std::string path = Quote(sage::PathToUtf8(file));
    const std::string lineText = std::to_string(line > 0 ? line : 1);
    std::string out;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern.compare(i, 6, "{file}") == 0) { out += path; i += 6; continue; }
        if (pattern.compare(i, 6, "{line}") == 0) { out += lineText; i += 6; continue; }
        out += pattern[i++];
    }
    return out;
}

const std::vector<App>& Available() {
    if (!Scanned()) Scan();
    return Cache();
}

void Rescan() { Scan(); }

std::string CurrentId() { return prefs::GetString("code.editor", ""); }

void SetCurrent(const std::string& id) { prefs::SetString("code.editor", id); }

std::string CurrentName() {
    const std::string id = CurrentId();
    if (id.empty()) return {};
    for (const App& a : Available())
        if (a.Id == id) return a.Name;
    // Выбранного больше нет (IDE удалили). Молча падать на системную ассоциацию
    // нельзя: человек обязан понимать, почему открывается не то, что он выбрал.
    return id + " (?)";
}

bool Open(const fs::path& file, int line) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        LOG_WARN("Editor") << "Нечего открывать, файла нет: " << file.string();
        return false;
    }

    const std::string id = CurrentId();
    const App* chosen = nullptr;
    if (!id.empty()) {
        for (const App& a : Available())
            if (a.Id == id) { chosen = &a; break; }
        if (!chosen) {
            LOG_WARN("Editor") << "Выбранный редактор '" << id
                               << "' на этой машине не найден — открываю системной ассоциацией";
        }
    }

    if (chosen) {
        const std::string pattern = line > 0 && !chosen->ArgsWithLine.empty() ? chosen->ArgsWithLine
                                                                             : chosen->Args;
        const std::string cmd = chosen->Command + " " + ExpandArgs(pattern, file, line);
#ifdef _WIN32
        // start "" — чтобы редактор не держал консоль редактора; кавычки после
        // start обязательны, иначе первый же путь в кавычках станет заголовком
        // окна, а не аргументом.
        const std::string full = "start \"\" " + cmd;
#else
        // & — не ждать выхода IDE: она живёт часами, а редактор должен
        // продолжить рисовать кадр.
        const std::string full = cmd + " >/dev/null 2>&1 &";
#endif
        LOG_INFO("Editor") << "Открываю " << chosen->Name << ": " << cmd;
        if (std::system(full.c_str()) == 0) return true;
        // Не сдаёмся молча: программа могла быть в PATH при поиске и исчезнуть
        // к запуску. Системная ассоциация — честный запасной путь, но о нём
        // надо сказать.
        LOG_WARN("Editor") << chosen->Name << " не запустился, пробую системную ассоциацию";
    }
    return false;   // вызывающий пробует системную ассоциацию (см. EditorLayer)
}

} // namespace sage::editor::codeapp
