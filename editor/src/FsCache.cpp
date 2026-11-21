#include "FsCache.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <chrono>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace sage::editor::fscache {

namespace {

double Now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

long long DiskStamp(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}

struct StampEntry {
    long long Stamp = 0;
    double Checked = -1e9;
};
struct InfoEntry {
    FileInfo Data;
    double Checked = -1e9;
};
struct ListEntry {
    Listing Data;
    long long DirStamp = -1;
    double Checked = -1e9;
};

std::unordered_map<std::string, StampEntry>& Stamps() {
    static std::unordered_map<std::string, StampEntry> m;
    return m;
}
std::unordered_map<std::string, InfoEntry>& Infos() {
    static std::unordered_map<std::string, InfoEntry> m;
    return m;
}
uint64_t& Generations() {
    static uint64_t g = 0;
    return g;
}
std::unordered_map<std::string, ListEntry>& Lists() {
    static std::unordered_map<std::string, ListEntry> m;
    return m;
}
std::unordered_map<std::string, fs::path>& Canonicals() {
    static std::unordered_map<std::string, fs::path> m;
    return m;
}

} // namespace

long long Stamp(const fs::path& path, double maxAge) {
    StampEntry& e = Stamps()[path.string()];
    const double now = Now();
    if (now - e.Checked >= maxAge) {
        e.Stamp = DiskStamp(path);
        e.Checked = now;
    }
    return e.Stamp;
}

FileInfo Info(const fs::path& path, double maxAge) {
    InfoEntry& e = Infos()[path.string()];
    const double now = Now();
    if (now - e.Checked >= maxAge) {
        e.Checked = now;
        e.Data = {};
        std::error_code ec;
        const fs::file_status st = fs::status(path, ec);
        if (!ec && fs::exists(st)) {
            e.Data.Stamp = DiskStamp(path);
            if (fs::is_regular_file(st)) {
                const uintmax_t n = fs::file_size(path, ec);
                e.Data.Size = ec ? 0 : n;
            }
        }
    }
    return e.Data;
}

const Listing& List(const fs::path& dir, double maxAge) {
    ListEntry& e = Lists()[dir.string()];
    const double now = Now();
    if (now - e.Checked < maxAge) return e.Data;
    e.Checked = now;
    const long long stamp = DiskStamp(dir);
    if (stamp == e.DirStamp && stamp != 0) return e.Data;
    e.DirStamp = stamp;
    e.Data = {};
    e.Data.Generation = ++Generations();
    std::error_code ec;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code tec;
        const bool isDir = entry.is_directory(tec);
        // Сайдкары .meta — служебная запись движка на каждый файл (GUID), их не
        // показывают и за содержимое не считают.
        if (!isDir && entry.path().extension() == ".meta") continue;
        // Служебные файлы с точки в начале имени (индекс общих картинок
        // импорта, .gitignore) — тоже не ассеты.
        if (!isDir && entry.path().filename().string().rfind('.', 0) == 0) continue;
        (isDir ? e.Data.Dirs : e.Data.Files).push_back(entry.path());
    }
    // Без учёта регистра: «b.png» стоит рядом с «B.png», а не после всех
    // заглавных. Равные без регистра — по байтам, чтобы порядок был полным.
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    auto byName = [&](const fs::path& a, const fs::path& b) {
        const std::string na = a.filename().string(), nb = b.filename().string();
        const std::string la = lower(na), lb = lower(nb);
        return la != lb ? la < lb : na < nb;
    };
    std::sort(e.Data.Dirs.begin(), e.Data.Dirs.end(), byName);
    std::sort(e.Data.Files.begin(), e.Data.Files.end(), byName);
    return e.Data;
}

const fs::path& Canonical(const fs::path& path) {
    auto& cache = Canonicals();
    auto it = cache.find(path.string());
    if (it != cache.end()) return it->second;
    std::error_code ec;
    fs::path c = fs::weakly_canonical(path, ec);
    if (ec) c = path.lexically_normal();
    // Кэш не растёт бесконечно: папок в проекте — сотни, а не миллионы, но
    // блуждание по чужим дискам в диалоге не должно копить память без конца.
    if (cache.size() > 4096) cache.clear();
    return cache.emplace(path.string(), std::move(c)).first->second;
}

void Invalidate(const fs::path& dir) {
    Lists().erase(dir.string());
    Stamps().erase(dir.string());
    Infos().erase(dir.string());
    // Файлы этой папки — тоже: переименованный файл мог получить имя
    // удалённого, и старые время и размер легли бы на новый.
    const std::string prefix = (dir / "").string();
    for (auto it = Stamps().begin(); it != Stamps().end();)
        it = it->first.rfind(prefix, 0) == 0 ? Stamps().erase(it) : std::next(it);
    for (auto it = Infos().begin(); it != Infos().end();)
        it = it->first.rfind(prefix, 0) == 0 ? Infos().erase(it) : std::next(it);
}

void Clear() {
    Infos().clear();
    Stamps().clear();
    Lists().clear();
    Canonicals().clear();
}

} // namespace sage::editor::fscache
