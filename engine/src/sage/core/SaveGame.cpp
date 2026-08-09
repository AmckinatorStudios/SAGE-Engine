#include "sage/core/SaveGame.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "sage/core/Log.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sage::save {
namespace {

std::string g_gameName;

// Имя слота приходит из игры и может прийти откуда угодно — из поля ввода, из
// имени персонажа, из сети. Пускать его в путь как есть нельзя: "../../.bashrc"
// — это запись за пределы папки сохранений. Оставляем только заведомо
// безопасные символы.
std::string SafeSlot(const std::string& slot) {
    std::string out;
    out.reserve(slot.size());
    for (unsigned char c : slot) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c >= 0x80;
        out.push_back(ok ? (char)c : '_');
    }
    if (out.empty()) out = "slot";
    return out;
}

std::string UserDataRoot() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) return appdata;
    if (const char* profile = std::getenv("USERPROFILE"))
        return std::string(profile) + "/AppData/Roaming";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] != '\0') return xdg;
    }
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/.local/share";
#endif
    // Ни одной переменной нет — редкий случай (служба, урезанное окружение).
    // Пишем рядом, но НЕ молча: иначе игрок не поймёт, куда делся прогресс.
    LOG_WARN("Save") << "Каталог пользователя не определён — сохранения лягут рядом с игрой";
    return ".";
}

std::string SlotPath(const std::string& slot) {
    return Directory() + "/" + SafeSlot(slot) + ".sagesave";
}

// Сбросить содержимое файла на носитель. Платформенное, потому что
// стандартной библиотеке такого понятия не известно вовсе: ofstream::flush()
// доводит байты только до ядра.
void SyncToDisk(const std::string& path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
#endif
}

long long NowUnix() {
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

void SetGameName(const std::string& name) { g_gameName = name; }
const std::string& GameName() { return g_gameName; }

std::string Directory() {
    std::string game = g_gameName.empty() ? "sage-game" : SafeSlot(g_gameName);
    return UserDataRoot() + "/" + game + "/saves";
}

bool Write(const std::string& slot, const std::string& payloadJson, int version) {
    std::error_code ec;
    const std::string dir = Directory();
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Save") << "Не удалось создать каталог сохранений " << dir << ": "
                          << ec.message();
        return false;
    }

    json root;
    root["sage_save_version"] = version;
    root["savedAt"] = NowUnix();
    try {
        root["data"] = json::parse(payloadJson);
    } catch (const std::exception& e) {
        LOG_ERROR("Save") << "Прогресс не является корректным JSON: " << e.what();
        return false;
    }

    // Пишем во ВРЕМЕННЫЙ файл и переименовываем. Прямая запись поверх старого
    // означает, что падение посреди неё уносит и новое сохранение, и прошлое:
    // файл остаётся обрезанным, а восстановить его неоткуда.
    const std::string finalPath = SlotPath(slot);
    const std::string tempPath = finalPath + ".tmp";
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOG_ERROR("Save") << "Не удалось открыть на запись " << tempPath;
            return false;
        }
        f << root.dump(2) << "\n";
        f.flush();
        if (!f) {
            LOG_ERROR("Save") << "Ошибка записи " << tempPath << " (нет места?)";
            f.close();
            fs::remove(tempPath, ec);
            return false;
        }
    }
    // Содержимое — НА ДИСК, и только потом переименование.
    //
    // Без этого «атомарная замена» атомарна лишь наполовину: flush() отдаёт
    // байты ядру, а не носителю. Ядро вправе держать их в кэше минуты, и при
    // отключении питания сразу после сохранения на диск успевает лечь только
    // переименование — то есть на месте прошлого сохранения оказывается ПУСТОЙ
    // файл. Это хуже, чем не сохраниться: старое уже стёрто, нового нет.
    SyncToDisk(tempPath);
    fs::rename(tempPath, finalPath, ec);
    if (ec) {
        LOG_ERROR("Save") << "Не удалось заменить " << finalPath << ": " << ec.message();
        fs::remove(tempPath, ec);
        return false;
    }
    LOG_INFO("Save") << "Прогресс сохранён: " << finalPath;
    return true;
}

bool Read(const std::string& slot, std::string& outPayloadJson, int* outVersion) {
    std::ifstream f(SlotPath(slot), std::ios::binary);
    if (!f) return false;
    json root;
    try {
        f >> root;
    } catch (const std::exception& e) {
        // Битый файл — не повод падать: игрок увидит «сохранение повреждено» и
        // начнёт заново, а не вылет при входе в меню.
        LOG_ERROR("Save") << "Сохранение повреждено (" << slot << "): " << e.what();
        return false;
    }
    if (!root.contains("data")) return false;
    if (outVersion) *outVersion = root.value("sage_save_version", 1);
    outPayloadJson = root["data"].dump();
    return true;
}

bool Exists(const std::string& slot) {
    std::error_code ec;
    return fs::exists(SlotPath(slot), ec);
}

bool Delete(const std::string& slot) {
    std::error_code ec;
    return fs::remove(SlotPath(slot), ec);
}

std::vector<SlotInfo> Slots() {
    std::vector<SlotInfo> out;
    std::error_code ec;
    const std::string dir = Directory();
    if (!fs::is_directory(dir, ec)) return out;

    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".sagesave") continue;
        SlotInfo info;
        info.Name = e.path().stem().string();
        info.Bytes = (size_t)fs::file_size(e.path(), ec);
        std::ifstream f(e.path(), std::ios::binary);
        json root;
        try {
            f >> root;
            info.SavedAtUnix = root.value("savedAt", 0LL);
            info.Version = root.value("sage_save_version", 1);
        } catch (const std::exception&) {
            // Битый слот всё равно показываем: игрок должен видеть, что он есть,
            // и иметь возможность его удалить. Молча прятать хуже.
            info.Version = 0;
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const SlotInfo& a, const SlotInfo& b) { return a.SavedAtUnix > b.SavedAtUnix; });
    return out;
}

} // namespace sage::save
