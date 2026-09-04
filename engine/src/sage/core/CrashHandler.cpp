#include "sage/core/CrashHandler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <vector>

#include "sage/core/Log.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace sage {

namespace {

constexpr int kNoteCount = 40;      // сколько последних строк лога уходит в отчёт
constexpr int kNoteLength = 220;

struct State {
    CrashHandler::Config Cfg;
    bool Installed = false;
    std::string LastReport;

    // Кольцевой буфер заметок: фиксированный размер и никаких выделений при
    // записи. В момент падения куча может быть повреждена, и std::string тут
    // повесил бы процесс вместо того, чтобы дать отчёт.
    std::vector<char> Notes;        // kNoteCount * kNoteLength
    std::atomic<int> NoteHead{0};
    std::atomic<int> NoteFilled{0};
    std::mutex Mutex;               // только для записи заметок ВНЕ падения
};

State& S() {
    static State s;
    return s;
}

// Пишет в дескриптор без буферизации: в обработчике сигнала можно только это.
void RawWrite(int fd, const char* text, size_t len) {
#if defined(_WIN32)
    (void)fd;
    (void)text;
    (void)len;
#else
    ssize_t written = 0;
    while ((size_t)written < len) {
        const ssize_t n = ::write(fd, text + written, len - (size_t)written);
        if (n <= 0) break;
        written += n;
    }
#endif
}

std::string Timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

#if defined(_WIN32)
// Имя модуля и смещение внутри него для адреса кадра.
//
// ЗАЧЕМ ЭТО, КОГДА ЕСТЬ SymFromAddr. Потому что имени от него верить нельзя.
// Без .pdb он резолвит адрес в БЛИЖАЙШИЙ ЭКСПОРТИРУЕМЫЙ символ, а редактор
// собран с --export-all-symbols — экспортов много, и «ближайший» оказывается
// любым. В присланном отчёте о падении так и вышло: три верхних кадра
// назывались CrashHandler::BuildReport +0x2262, следующий — strncpy +0x2c90,
// то есть стек читался как заведомая чушь и не указывал ни на что.
//
// Модуль и смещение — величина, которую не надо угадывать: она не зависит ни
// от экспортов, ни от ASLR (база вычитается), и по ней ТА ЖЕ сборка
// разворачивается в строку исходника одной командой addr2line. Поэтому
// печатается она ПЕРВОЙ и всегда, а имя от SymFromAddr идёт следом и помечено
// как приблизительное.
bool ModuleAndOffset(void* addr, char* out, size_t outSize) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) ||
        !mod)
        return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(mod, path, MAX_PATH)) return false;
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    const unsigned long long off =
        (unsigned long long)((const unsigned char*)addr - (const unsigned char*)mod);
    std::snprintf(out, outSize, "%s+0x%llx", name, off);
    return true;
}
#endif

std::string Backtrace() {
    std::string out;
#if defined(_WIN32)
    void* frames[62];
    const USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);
    char symbolBuf[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    for (USHORT i = 0; i < n; ++i) {
        char where[MAX_PATH + 32];
        if (!ModuleAndOffset(frames[i], where, sizeof(where)))
            std::snprintf(where, sizeof(where), "0x%p", frames[i]);

        DWORD64 disp = 0;
        char line[768];
        if (SymFromAddr(proc, (DWORD64)frames[i], &disp, symbol)) {
            std::snprintf(line, sizeof(line), "  #%02u %s   (~%s +0x%llx)\n", i, where,
                          symbol->Name, (unsigned long long)disp);
        } else {
            std::snprintf(line, sizeof(line), "  #%02u %s\n", i, where);
        }
        out += line;
    }
    out += "  (~имя — ближайший экспорт, ему верить нельзя; адрес в строку\n"
           "   исходника разворачивается ТОЙ ЖЕ сборкой:\n"
           "   x86_64-w64-mingw32-addr2line -f -C -e SageEditor.exe <смещение>)\n";
#else
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    // backtrace_symbols выделяет память — в обработчике сигнала это риск, но
    // без имён отчёт бесполезен, а падение уже произошло. Если повиснем —
    // потеряем только отчёт, который иначе всё равно был бы нечитаемым.
    char** symbols = ::backtrace_symbols(frames, n);
    for (int i = 0; i < n; ++i) {
        char line[512];
        std::snprintf(line, sizeof(line), "  #%02d %s\n", i,
                      symbols && symbols[i] ? symbols[i] : "?");
        out += line;
    }
    if (symbols) std::free(symbols);
#endif
    return out;
}

void WriteReport(const char* reason) {
    State& s = S();
    const std::string report = CrashHandler::BuildReport(reason);

    const std::string path =
        s.Cfg.ReportDir + "/sage-crash-" + Timestamp() + ".txt";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fwrite(report.data(), 1, report.size(), f);
        std::fclose(f);
        s.LastReport = path;
    }

    // В консоль — тоже: если каталог недоступен для записи, отчёт останется
    // хотя бы в выводе процесса.
#if !defined(_WIN32)
    RawWrite(2, report.data(), report.size());
#else
    std::fwrite(report.data(), 1, report.size(), stderr);
#endif

    // Аварийное сохранение — последним: оно самое рискованное (выделяет память,
    // трогает диск), и отчёт к этому моменту уже записан.
    if (s.Cfg.EmergencySave) {
        const std::string saved = s.Cfg.EmergencySave();
        if (!saved.empty()) {
            const std::string note = "\nEMERGENCY SAVE: " + saved + "\n";
            if (FILE* f = std::fopen(path.c_str(), "a")) {
                std::fwrite(note.data(), 1, note.size(), f);
                std::fclose(f);
            }
#if !defined(_WIN32)
            RawWrite(2, note.data(), note.size());
#endif
        }
    }
}

#if !defined(_WIN32)
const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (обращение по неверному адресу)";
        case SIGABRT: return "SIGABRT (abort / необработанное исключение)";
        case SIGFPE:  return "SIGFPE (арифметическая ошибка)";
        case SIGILL:  return "SIGILL (недопустимая инструкция)";
        case SIGBUS:  return "SIGBUS (ошибка шины)";
        default:      return "неизвестный сигнал";
    }
}

void SignalHandler(int sig) {
    // Снимаем себя ПЕРВОЙ строкой: если упадём внутри обработки, повторный
    // сигнал должен убить процесс, а не крутить рекурсию.
    CrashHandler::Uninstall();
    WriteReport(SignalName(sig));
    // Возвращаем поведение по умолчанию и добиваем себя тем же сигналом:
    // падение обязано остаться падением — иначе его не увидят ни отладчик, ни
    // системный сборщик отчётов, а код возврата соврёт скриптам сборки.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#else
LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* info) {
    CrashHandler::Uninstall();
    char reason[128];
    std::snprintf(reason, sizeof(reason), "исключение 0x%08lx",
                  info ? info->ExceptionRecord->ExceptionCode : 0UL);
    WriteReport(reason);
    return EXCEPTION_CONTINUE_SEARCH;   // пусть система тоже увидит падение
}
#endif

void TerminateHandler() {
    CrashHandler::Uninstall();
    // Причину необработанного исключения достаём, повторно его бросив: другого
    // способа узнать его текст из std::terminate нет.
    std::string reason = "std::terminate (необработанное исключение)";
    if (std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            reason += std::string(": ") + e.what();
        } catch (...) {
            reason += ": исключение неизвестного типа";
        }
    }
    WriteReport(reason.c_str());
    std::abort();
}

} // namespace

void CrashHandler::Install(const Config& config) {
    State& s = S();
    s.Cfg = config;
    s.Notes.assign((size_t)kNoteCount * kNoteLength, 0);

#if defined(_WIN32)
    SetUnhandledExceptionFilter(ExceptionHandler);
#else
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) std::signal(sig, SignalHandler);
#endif
    std::set_terminate(TerminateHandler);
    s.Installed = true;
    LOG_INFO("Crash") << "Обработчик падений установлен, отчёты в " << s.Cfg.ReportDir;
}

void CrashHandler::Uninstall() {
    State& s = S();
#if defined(_WIN32)
    SetUnhandledExceptionFilter(nullptr);
#else
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) std::signal(sig, SIG_DFL);
#endif
    s.Installed = false;
}

bool CrashHandler::Installed() { return S().Installed; }

void CrashHandler::Note(const std::string& line) {
    State& s = S();
    if (s.Notes.empty()) return;
    std::lock_guard<std::mutex> lock(s.Mutex);
    const int slot = s.NoteHead.load() % kNoteCount;
    char* dst = s.Notes.data() + (size_t)slot * kNoteLength;
    const size_t n = std::min(line.size(), (size_t)kNoteLength - 1);
    std::memcpy(dst, line.data(), n);
    dst[n] = 0;
    s.NoteHead.store((slot + 1) % kNoteCount);
    if (s.NoteFilled.load() < kNoteCount) s.NoteFilled.fetch_add(1);
}

std::string CrashHandler::BuildReport(const std::string& reason) {
    State& s = S();
    std::string out;
    out.reserve(8192);
    out += "=== ";
    out += s.Cfg.AppName;
    out += " CRASH REPORT ===\n";
    out += "Время:   " + Timestamp() + "\n";
    out += "Версия:  " + (s.Cfg.Version.empty() ? std::string("?") : s.Cfg.Version) + "\n";
    out += "Причина: " + reason + "\n";

    if (s.Cfg.Context) {
        // Контекст собирает вызывающий (открытый проект, сцена, видеокарта).
        // Он может упасть сам — процесс уже сломан; тогда обходимся без него.
        try {
            const std::string ctx = s.Cfg.Context();
            if (!ctx.empty()) out += "\n--- Контекст ---\n" + ctx + "\n";
        } catch (...) {
            out += "\n--- Контекст недоступен ---\n";
        }
    }

    out += "\n--- Стек вызовов ---\n";
    out += Backtrace();

    const int filled = s.NoteFilled.load();
    if (filled > 0 && !s.Notes.empty()) {
        out += "\n--- Последние " + std::to_string(filled) + " сообщений лога ---\n";
        const int head = s.NoteHead.load();
        for (int i = 0; i < filled; ++i) {
            const int slot = (head - filled + i + kNoteCount * 2) % kNoteCount;
            const char* line = s.Notes.data() + (size_t)slot * kNoteLength;
            if (line[0]) {
                out += "  ";
                out += line;
                out += "\n";
            }
        }
    }
    out += "\n=== конец отчёта ===\n";
    return out;
}

const std::string& CrashHandler::LastReportPath() { return S().LastReport; }

} // namespace sage
