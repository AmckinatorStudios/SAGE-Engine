#include "FsCache.h"

#include <algorithm>
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
struct ListEntry {
    Listing Data;
    long long DirStamp = -1;
    double Checked = -1e9;
};

std::unordered_map<std::string, StampEntry>& Stamps() {
    static std::unordered_map<std::string, StampEntry> m;
    return m;
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

const Listing& List(const fs::path& dir, double maxAge) {
    ListEntry& e = Lists()[dir.string()];
    const double now = Now();
    if (now - e.Checked < maxAge) return e.Data;
    e.Checked = now;
    const long long stamp = DiskStamp(dir);
    if (stamp == e.DirStamp && stamp != 0) return e.Data;
    e.DirStamp = stamp;
    e.Data = {};
    std::error_code ec;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code tec;
        const bool isDir = entry.is_directory(tec);
        // Сайдкары .meta — служебная запись движка на каждый файл (GUID), их не
        // показывают и за содержимое не считают.
        if (!isDir && entry.path().extension() == ".meta") continue;
        (isDir ? e.Data.Dirs : e.Data.Files).push_back(entry.path());
    }
    auto byName = [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); };
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

void Clear() {
    Stamps().clear();
    Lists().clear();
    Canonicals().clear();
}

} // namespace sage::editor::fscache
