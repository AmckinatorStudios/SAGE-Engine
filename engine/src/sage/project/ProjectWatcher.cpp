#include "sage/project/ProjectWatcher.h"

#include <algorithm>

#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace sage::project {

namespace {

// Время последней записи числом. file_time_type у разных реализаций имеет
// разную эпоху, и сравнивать его напрямую между запусками нельзя — а вот
// «изменилось ли с прошлого обхода В ЭТОМ запуске» по числу тактов сравнивается
// прекрасно, и это всё, что нам нужно.
int64_t WriteTicks(const fs::directory_entry& entry, std::error_code& ec) {
    const fs::file_time_type t = entry.last_write_time(ec);
    if (ec) return 0;
    return t.time_since_epoch().count();
}

} // namespace

void ProjectWatcher::Watch(const Project& project) {
    m_watching = false;
    m_known.clear();
    if (!project.Loaded()) return;

    m_root = project.Dir();
    if (m_extensions.empty()) {
        // По умолчанию — то, что осмысленно правят снаружи: сцены, скрипты,
        // материалы, префабы и сам файл проекта.
        m_extensions = {".sage", ".lua", ".sagemat", ".sageprefab", ".sageproj"};
    }
    Snapshot(m_known);
    m_watching = true;
    m_lastPoll = 0.0;
}

void ProjectWatcher::Stop() {
    m_watching = false;
    m_known.clear();
    m_root.clear();
}

void ProjectWatcher::SetExtensions(std::vector<std::string> extensions) {
    m_extensions = std::move(extensions);
}

bool ProjectWatcher::Interesting(const fs::path& path) const {
    const std::string ext = path.extension().string();
    return std::find(m_extensions.begin(), m_extensions.end(), ext) != m_extensions.end();
}

std::string ProjectWatcher::RelPath(const fs::path& path) const {
    std::error_code ec;
    const fs::path rel = fs::relative(path, m_root, ec);
    if (ec) return path.generic_string();
    return rel.generic_string();
}

void ProjectWatcher::Snapshot(std::unordered_map<std::string, Stamp>& out) const {
    out.clear();
    if (m_root.empty()) return;

    std::error_code ec;
    // skip_permission_denied: в дереве проекта может оказаться чужая папка без
    // прав, и падать из-за неё обходу незачем — он про наши файлы.
    fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;

    for (const fs::directory_entry& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        if (!Interesting(entry.path())) continue;

        Stamp stamp;
        stamp.WriteTime = WriteTicks(entry, entryEc);
        stamp.Size = entry.file_size(entryEc);
        if (entryEc) continue;
        out[RelPath(entry.path())] = stamp;
    }
}

void ProjectWatcher::MarkOwnWrite(const fs::path& path) {
    if (!m_watching) return;
    std::error_code ec;
    const fs::directory_entry entry(path, ec);
    if (ec) return;
    if (!entry.is_regular_file(ec)) {
        // Файла нет — значит мы его удалили. Убираем из снимка, иначе
        // следующий опрос сообщит о «чужом» удалении.
        m_known.erase(RelPath(path));
        return;
    }
    Stamp stamp;
    stamp.WriteTime = WriteTicks(entry, ec);
    stamp.Size = entry.file_size(ec);
    if (ec) return;
    m_known[RelPath(path)] = stamp;
}

std::vector<Change> ProjectWatcher::Poll(double nowSeconds) {
    std::vector<Change> changes;
    if (!m_watching) return changes;
    if (nowSeconds - m_lastPoll < m_interval) return changes;
    m_lastPoll = nowSeconds;

    std::unordered_map<std::string, Stamp> fresh;
    Snapshot(fresh);

    for (const auto& [path, stamp] : fresh) {
        auto known = m_known.find(path);
        if (known == m_known.end()) {
            changes.push_back({ChangeKind::Added, path});
        } else if (known->second.WriteTime != stamp.WriteTime ||
                   known->second.Size != stamp.Size) {
            // И время, и размер: время записи у некоторых файловых систем
            // огрубляется до секунды, и правка внутри той же секунды по нему
            // не видна. Размер ловит как раз такие случаи.
            changes.push_back({ChangeKind::Modified, path});
        }
    }
    for (const auto& [path, stamp] : m_known) {
        (void)stamp;
        if (fresh.find(path) == fresh.end()) changes.push_back({ChangeKind::Removed, path});
    }

    m_known.swap(fresh);

    // Порядок обхода хеш-таблицы не определён — сортируем, иначе один и тот же
    // набор правок показывался бы человеку каждый раз в новом порядке.
    std::sort(changes.begin(), changes.end(),
              [](const Change& a, const Change& b) { return a.Path < b.Path; });

    if (!changes.empty()) {
        LOG_INFO("Project") << "снаружи изменено файлов: " << changes.size()
                            << " (первый: " << changes.front().Path << ")";
    }
    return changes;
}

} // namespace sage::project
