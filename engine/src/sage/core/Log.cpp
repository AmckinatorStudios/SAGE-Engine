#include "Log.h"
#include <iostream>
#include <iomanip>
#include <ctime>
#include <chrono>

std::mutex Log::s_mutex;
std::ofstream Log::s_file;
LogLevel Log::s_minLevel = LogLevel::Trace;
bool Log::s_fileEnabled = false;

namespace {
    const char* LevelName(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
        }
        return "?????";
    }

    // ANSI-цвета для терминала — молча не действуют в файле/на терминалах
    // без поддержки (просто печатаются как безобидные escape-последовательности).
    const char* LevelColor(LogLevel level) {
        switch (level) {
            case LogLevel::Trace: return "\033[90m"; // серый
            case LogLevel::Debug: return "\033[36m"; // голубой
            case LogLevel::Info:  return "\033[37m"; // белый
            case LogLevel::Warn:  return "\033[33m"; // жёлтый
            case LogLevel::Error: return "\033[31m"; // красный
        }
        return "\033[0m";
    }
    const char* kColorReset = "\033[0m";

    std::string Timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }
}

void Log::Init(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_file.open(filePath, std::ios::out | std::ios::trunc);
    s_fileEnabled = s_file.is_open();
    if (!s_fileEnabled) {
        std::cerr << "[Log] Не удалось открыть файл лога: " << filePath << std::endl;
    }
}

void Log::SetMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_minLevel = level;
}

LogLevel Log::MinLevel() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_minLevel;
}

void Log::WriteLine(LogLevel level, const std::string& category, const std::string& message) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (level < s_minLevel) return;

    std::string prefix = "[" + Timestamp() + "] [" + LevelName(level) + "] [" + category + "] ";

    // Warn/Error всегда идут в std::cerr, остальное — в std::cout, чтобы
    // перенаправление потоков (например 2>error.log) работало осмысленно.
    std::ostream& out = (level >= LogLevel::Warn) ? std::cerr : std::cout;
    out << LevelColor(level) << prefix << message << kColorReset << std::endl;

    if (s_fileEnabled) {
        s_file << prefix << message << std::endl;
        s_file.flush();
    }
}
