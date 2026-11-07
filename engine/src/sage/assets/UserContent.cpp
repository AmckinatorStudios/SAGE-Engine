#include "sage/assets/UserContent.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>

#include "sage/assets/SaveGame.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace sage::assets::usercontent {

namespace {

std::mutex g_mutex;
std::vector<UserFolderSpec> g_folders;
fs::path g_gameDir;      // папка собранной игры; пусто — запуск из проекта
fs::path g_projectDir;
const char* const kReadme = "README.txt";

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool Writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / ".sage-write-test";
    {
        std::ofstream f(probe);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

// Примеры из src в dst — только тех файлов, которых там ещё нет: игрок мог
// поменять пример, и следующая сборка или запуск не вправе его затереть.
void CopySeeds(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = PathToUtf8(it->path().filename());
        if (name.size() > 5 && Lower(name.substr(name.size() - 5)) == ".meta") continue;
        const fs::path to = dst / fs::relative(it->path(), src, ec);
        if (fs::exists(to, ec)) continue;
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(it->path(), to, ec);
    }
}

void WriteReadme(const fs::path& dir, const UserFolderSpec& spec) {
    std::error_code ec;
    const fs::path p = dir / kReadme;
    if (fs::exists(p, ec)) return;
    std::ofstream f(p, std::ios::binary);
    f << Readme(spec);
}

// Создать папку при первом обращении: README и примеры из проекта.
void Prepare(const fs::path& dir, const UserFolderSpec& spec, const fs::path& seeds) {
    std::error_code ec;
    const bool fresh = !fs::exists(dir, ec);
    fs::create_directories(dir, ec);
    WriteReadme(dir, spec);
    if (fresh && !seeds.empty()) CopySeeds(seeds, dir);
}

bool Accepts(const UserFolderSpec& spec, const fs::path& file) {
    if (spec.Extensions.empty()) return true;
    const std::string ext = Lower(PathToUtf8(file.extension()));
    for (const std::string& e : spec.Extensions) {
        std::string want = Lower(e);
        if (!want.empty() && want[0] != '.') want = "." + want;
        if (ext == want) return true;
    }
    return false;
}

} // namespace

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    return true;
}

std::string Readme(const UserFolderSpec& spec) {
    std::string out = (spec.Title.empty() ? spec.Name : spec.Title) + "\n";
    out += std::string(out.size() - 1, '=') + "\n\n";
    if (!spec.Description.empty()) out += spec.Description + "\n\n";
    if (spec.Extensions.empty()) {
        out += "Any files placed here are visible to the game.\n";
    } else {
        out += "Files the game reads:";
        for (const std::string& e : spec.Extensions) out += " " + e;
        out += "\n";
    }
    out += "Subfolders are read too. Changes are picked up while the game is running.\n";
    return out;
}

void Configure(const std::vector<UserFolderSpec>& folders, const fs::path& builtGameDir,
               const fs::path& projectDir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_folders.clear();
    for (const UserFolderSpec& f : folders) {
        if (ValidName(f.Name)) g_folders.push_back(f);
        else LOG_WARN("UserContent") << "Пользовательская папка с недопустимым именем пропущена: «" << f.Name << "»";
    }
    g_gameDir = builtGameDir;
    g_projectDir = projectDir;
}

const std::vector<UserFolderSpec>& Folders() { return g_folders; }

const UserFolderSpec* Find(const std::string& name) {
    for (const UserFolderSpec& f : g_folders)
        if (f.Name == name) return &f;
    return nullptr;
}

fs::path Resolve(const std::string& name) {
    const UserFolderSpec* spec = Find(name);
    if (!spec) return {};
    const fs::path seeds = g_projectDir.empty() ? fs::path() : g_projectDir / "userfolders" / name;
    // Рядом с игрой — только у собранной игры и только если там можно писать
    // (Program Files, образ диска на macOS — нельзя): иначе папка данных.
    if (spec->NextToGame && !g_gameDir.empty()) {
        const fs::path dir = g_gameDir / name;
        if (Writable(dir)) {
            Prepare(dir, *spec, seeds);
            return dir;
        }
    }
    const fs::path dir = PathFromUtf8(sage::save::GameDirectory()) / name;
    Prepare(dir, *spec, seeds);
    return dir;
}

std::vector<Entry> List(const std::string& name, bool recursive) {
    std::vector<Entry> out;
    const UserFolderSpec* spec = Find(name);
    if (!spec) return out;
    const fs::path root = Resolve(name);
    std::error_code ec;
    auto visit = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file(ec)) return;
        const std::string fname = PathToUtf8(e.path().filename());
        if (fname.empty() || fname[0] == '.' || fname == kReadme) return;
        if (!Accepts(*spec, e.path())) return;
        Entry en;
        en.Name = fname;
        en.Path = PathToUtf8(e.path());
        std::string rel = PathToUtf8(fs::relative(e.path(), root, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        en.Relative = rel;
        en.Size = (uint64_t)e.file_size(ec);
        const auto t = e.last_write_time(ec);
        en.Modified = (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        out.push_back(en);
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            visit(*it);
    } else {
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            visit(*it);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return Lower(a.Relative) < Lower(b.Relative); });
    return out;
}

std::string Signature(const std::string& name) {
    std::string s;
    for (const Entry& e : List(name)) {
        s += e.Relative + "|" + std::to_string(e.Size) + "|" + std::to_string(e.Modified) + "\n";
    }
    return s;
}

bool OpenInSystem(const std::string& name) {
    const fs::path dir = Resolve(name);
    if (dir.empty()) return false;
#ifdef _WIN32
    // ShellExecuteW: узкий system() на Windows идёт через ANSI, и папка с
    // кириллицей в пути до проводника не доезжает.
    const std::wstring wide = dir.wstring();
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)rc > 32;
#else
    std::string quoted = "'";
    for (char c : PathToUtf8(dir)) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
#ifdef __APPLE__
    const std::string cmd = "open " + quoted + " >/dev/null 2>&1 &";
#else
    const std::string cmd = "xdg-open " + quoted + " >/dev/null 2>&1 &";
#endif
    return std::system(cmd.c_str()) == 0;
#endif
}

size_t PrepareForBuild(const fs::path& gameDir, const fs::path& projectDir,
                       const std::vector<UserFolderSpec>& folders) {
    size_t made = 0;
    for (const UserFolderSpec& f : folders) {
        if (!f.NextToGame || !ValidName(f.Name)) continue;
        const fs::path dir = gameDir / f.Name;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            LOG_WARN("Build") << "Не создать пользовательскую папку " << PathToUtf8(dir) << ": " << ec.message();
            continue;
        }
        WriteReadme(dir, f);
        CopySeeds(projectDir / "userfolders" / f.Name, dir);
        ++made;
    }
    return made;
}

} // namespace sage::assets::usercontent
