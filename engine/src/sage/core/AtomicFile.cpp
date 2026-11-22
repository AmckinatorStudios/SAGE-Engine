#include "sage/core/AtomicFile.h"

#include <atomic>
#include <filesystem>
#include <fstream>

#include "sage/core/Paths.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sage {
namespace {

#ifdef _WIN32
unsigned long ProcessId() { return (unsigned long)GetCurrentProcessId(); }
#else
unsigned long ProcessId() { return (unsigned long)getpid(); }
#endif

// Имя временного файла уникально в процессе и между процессами: две записи
// одного файла (автосохранение и ручное, два окна редактора) с общим «.tmp»
// писали бы в один временный файл и подменяли цель чужими байтами.
std::string TempSuffix() {
    static std::atomic<unsigned> counter{0};
    return ".tmp." + std::to_string(ProcessId()) + "." + std::to_string(counter.fetch_add(1));
}

// Сбросить данные временного файла на диск ДО подмены: иначе после сбоя
// питания rename мог оказаться на диске раньше содержимого, и вместо прежней
// сцены лежал бы файл нулевой длины.
bool FlushToDisk(const std::filesystem::path& file, std::string& why) {
#ifdef _WIN32
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "код " + std::to_string((unsigned long)GetLastError());
        return false;
    }
    const BOOL ok = FlushFileBuffers(h);
    if (!ok) why = "код " + std::to_string((unsigned long)GetLastError());
    CloseHandle(h);
    return ok != 0;
#else
    const int fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0) {
        why = std::strerror(errno);
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    if (!ok) why = std::strerror(errno);
    ::close(fd);
    return ok;
#endif
}

// Атомарная подмена: цель либо прежняя, либо новая — промежуточного
// состояния «старого уже нет, нового ещё нет» не бывает. Прежний путь на
// Windows сначала УДАЛЯЛ цель и потом переименовывал временный: сбой между
// двумя шагами (антивирус держит файл, нет прав) оставлял пользователя вовсе
// без сцены.
bool SwapIntoPlace(const std::filesystem::path& tmp, const std::filesystem::path& target, std::string& why) {
#ifdef _WIN32
    if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    why = "код " + std::to_string((unsigned long)GetLastError());
    return false;
#else
    // rename(2) подменяет существующий файл атомарно.
    if (std::rename(tmp.c_str(), target.c_str()) == 0) return true;
    why = std::strerror(errno);
    return false;
#endif
}

void RemoveTemp(const std::filesystem::path& tmp) {
    std::error_code ec;   // временный мог и не создаться — это не ошибка
    std::filesystem::remove(tmp, ec);
}

} // namespace

bool WriteFileAtomic(const std::string& utf8Path, const std::string& bytes, std::string& err) {
    const std::filesystem::path path = PathFromUtf8(utf8Path);
    const std::filesystem::path tmp = PathFromUtf8(utf8Path + TempSuffix());
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            err = "не удалось открыть на запись: " + utf8Path;
            RemoveTemp(tmp);
            return false;
        }
        file.write(bytes.data(), (std::streamsize)bytes.size());
        file.close();
        if (!file) {
            err = "не удалось записать: " + utf8Path;
            RemoveTemp(tmp);
            return false;
        }
    }
    std::string why;
    if (!FlushToDisk(tmp, why)) {
        err = "не удалось сбросить на диск: " + utf8Path + " (" + why + ")";
        RemoveTemp(tmp);
        return false;
    }
    if (!SwapIntoPlace(tmp, path, why)) {
        err = "не удалось заменить файл: " + utf8Path + " (" + why + ")";
        RemoveTemp(tmp);
        return false;
    }
    return true;
}

} // namespace sage
