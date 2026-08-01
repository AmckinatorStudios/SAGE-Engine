#pragma once
#include <string>
#include <sstream>
#include <mutex>
#include <fstream>
#include <functional>

// ---------------------------------------------------------------------
// Система логирования SAGE Engine.
//
// Использование (нигде не нужно вручную собирать строку через sprintf —
// просто "стримим" в LOG_*, как в std::cout):
//
//   LOG_INFO("Render") << "Текстура загружена: " << path << " (" << w << "x" << h << ")";
//   LOG_WARN("Model")  << "Не удалось загрузить текстуру " << texPath;
//   LOG_ERROR("Core")  << "Не удалось создать окно GLFW";
//
// Каждый макрос создаёт временный LogMessageBuilder, который копит текст
// через operator<< и одной строкой пишет его в консоль (с цветом по
// уровню) и, если Log::Init() был вызван, в лог-файл — в момент своего
// разрушения (конец выражения). Отдельно вызывать flush не нужно.
// ---------------------------------------------------------------------

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

class Log {
public:
    // Включает запись в файл (в дополнение к консоли). Безопасно вызывать
    // из main() один раз при старте движка. Если не вызывать вообще —
    // логгер просто пишет только в консоль.
    static void Init(const std::string& filePath = "sage_engine.log");

    // Сообщения уровнем ниже — не печатаются вообще. По умолчанию Debug: Trace
    // отведён под поштучные события (создание каждой сущности) и включается
    // явно; в релизной сборке имеет смысл поднять порог до Info.
    static void SetMinLevel(LogLevel level);
    static LogLevel MinLevel();

    // Низкоуровневая запись одной готовой строки — используется
    // LogMessageBuilder'ом, обычно напрямую вызывать не нужно.
    static void WriteLine(LogLevel level, const std::string& category, const std::string& message);

    // Дополнительный приёмник сообщений (помимо консоли/файла) — например,
    // панель Console в редакторе. Вызывается под внутренним мьютексом логгера,
    // поэтому сам приёмник НЕ должен логировать (иначе дедлок). nullptr — снять.
    using Sink = std::function<void(LogLevel, const std::string& category, const std::string& message)>;
    static void SetSink(Sink sink);

private:
    static std::mutex s_mutex;
    static std::ofstream s_file;
    static LogLevel s_minLevel;
    static bool s_fileEnabled;
    static Sink s_sink;
};

// RAII-построитель одного сообщения: копит поток через operator<<,
// пишет всё разом в деструкторе. Так LOG_INFO("cat") << a << b << c;
// работает как обычный std::ostream, без промежуточного форматирования.
class LogMessageBuilder {
public:
    LogMessageBuilder(LogLevel level, const char* category) : m_level(level), m_category(category) {}
    ~LogMessageBuilder() { Log::WriteLine(m_level, m_category, m_stream.str()); }

    LogMessageBuilder(const LogMessageBuilder&) = delete;
    LogMessageBuilder& operator=(const LogMessageBuilder&) = delete;

    template <typename T>
    LogMessageBuilder& operator<<(const T& value) {
        m_stream << value;
        return *this;
    }

private:
    LogLevel m_level;
    std::string m_category;
    std::ostringstream m_stream;
};

#define LOG_TRACE(category) LogMessageBuilder(LogLevel::Trace, category)
#define LOG_DEBUG(category) LogMessageBuilder(LogLevel::Debug, category)
#define LOG_INFO(category)  LogMessageBuilder(LogLevel::Info, category)
#define LOG_WARN(category)  LogMessageBuilder(LogLevel::Warn, category)
#define LOG_ERROR(category) LogMessageBuilder(LogLevel::Error, category)
