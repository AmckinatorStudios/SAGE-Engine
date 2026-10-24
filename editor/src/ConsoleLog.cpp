#include "ConsoleLog.h"

int ConsoleLog::Bucket(LogLevel level) {
    if (level >= LogLevel::Error) return 3;
    if (level == LogLevel::Warn) return 2;
    if (level == LogLevel::Info) return 1;
    return 0;
}

void ConsoleLog::Add(LogLevel level, const std::string& category, const std::string& message,
                     const std::string& stamp) {
    ++m_counts[Bucket(level)];
    // Сворачивание повторов. Один кадр может выдать одно и то же
    // предупреждение тысячу раз (шейдер не собрался — и так на каждый объект),
    // и без свёртки такая строка вытесняет из окна всё остальное, включая
    // причину.
    if (!m_entries.empty()) {
        Entry& last = m_entries.back();
        if (last.Level == level && last.Category == category && last.Message == message) {
            ++last.Repeats;
            last.Stamp = stamp;   // время последнего прихода: «ещё идёт или давно кончилось»
            return;
        }
    }
    if (m_entries.size() >= kLimit) {
        const size_t drop = kLimit / 4;
        for (size_t i = 0; i < drop; ++i) m_counts[Bucket(m_entries[i].Level)] -= m_entries[i].Repeats;
        m_entries.erase(m_entries.begin(), m_entries.begin() + (std::ptrdiff_t)drop);
    }
    m_entries.push_back({level, category, message, 1, stamp});
}

void ConsoleLog::Clear() {
    m_entries.clear();
    for (int& c : m_counts) c = 0;
}

int ConsoleLog::Count(LogLevel level) const { return m_counts[Bucket(level)]; }

bool ConsoleLog::Passes(const Entry& e, const Filter& f) const {
    switch (Bucket(e.Level)) {
        case 3: if (!f.Error) return false; break;
        case 2: if (!f.Warn) return false; break;
        case 1: if (!f.Info) return false; break;
        default: if (!f.Debug) return false; break;
    }
    if (!f.Text.empty()) {
        // Ищем и по источнику, и по тексту: искать «Shader» и не найти
        // сообщение источника Shader было бы неожиданностью. Без учёта
        // регистра — человек набирает «shader», а пишет движок «Shader».
        auto lower = [](std::string s) {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return s;
        };
        const std::string needle = lower(f.Text);
        if (lower(e.Message).find(needle) == std::string::npos &&
            lower(e.Category).find(needle) == std::string::npos)
            return false;
    }
    return true;
}

std::string ConsoleLog::LineText(const Entry& e) {
    std::string line = e.Stamp + " [" + e.Category + "] " + e.Message;
    if (e.Repeats > 1) line += " (x" + std::to_string(e.Repeats) + ")";
    return line;
}

std::string ConsoleLog::Text(const Filter& f) const {
    std::string out;
    for (const Entry& e : m_entries)
        if (Passes(e, f)) out += LineText(e) + "\n";
    return out;
}
