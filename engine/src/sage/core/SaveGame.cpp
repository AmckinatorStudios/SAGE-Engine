#include "sage/core/SaveGame.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <nlohmann/json.hpp>

#include "sage/assets/format/Blob.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "sage/core/Log.h"
#include "sage/core/Paths.h"

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
    // Через EnvPath, а не getenv: на Windows узкое окружение приходит в ANSI, и
    // «C:\Users\Вова\AppData\Roaming» такими байтами убивает конструктор
    // std::filesystem::path (см. Paths.h). Сохранения игрока — не то место, где
    // можно позволить себе падение из-за имени пользователя.
#ifdef _WIN32
    if (const fs::path appdata = EnvPath("APPDATA"); !appdata.empty())
        return PathToUtf8(appdata);
    if (const fs::path profile = EnvPath("USERPROFILE"); !profile.empty())
        return PathToUtf8(profile / "AppData" / "Roaming");
#else
    if (const fs::path xdg = EnvPath("XDG_DATA_HOME"); !xdg.empty())
        return PathToUtf8(xdg);
    if (const fs::path home = EnvPath("HOME"); !home.empty())
        return PathToUtf8(home / ".local" / "share");
#endif
    // Ни одной переменной нет — редкий случай (служба, урезанное окружение).
    // Пишем рядом, но НЕ молча: иначе игрок не поймёт, куда делся прогресс.
    LOG_WARN("Save") << "Каталог пользователя не определён — сохранения лягут рядом с игрой";
    return ".";
}

// ПУТЁМ, а не строкой: сохранения лежат в папке пользователя, а её имя на
// Windows почти всегда содержит имя человека — сплошь и рядом кириллицей.
// Узкая строка там читается как ANSI, и прогресс молча не сохранялся
// (см. scripts/check_paths.py).
fs::path SlotFile(const std::string& slot) {
    return sage::PathFromUtf8(Directory()) / (SafeSlot(slot) + ".sagesave");
}

std::string SlotPath(const std::string& slot) {
    return Directory() + "/" + SafeSlot(slot) + ".sagesave";
}

// Резервная копия лежит РЯДОМ и с тем же именем: так её видно человеку, и
// «откатиться на прошлое сохранение» не требует объяснять, где оно.
fs::path BackupFile(const std::string& slot) {
    fs::path p = SlotFile(slot);
    p += ".bak";
    return p;
}

std::string BackupPath(const std::string& slot) { return SlotPath(slot) + ".bak"; }

// --- Контейнер --------------------------------------------------------------
//
// Заголовок фиксированной длины, за ним JSON заголовка, за ним прогресс. Всё
// целое — little-endian: формат читают только программы, а разбирать порядок
// байтов по платформе значило бы, что сохранение с Windows не откроется на
// Linux (а оно ездит вместе с облачным профилем).
constexpr char kMagic[8] = {'S', 'A', 'G', 'E', 'S', 'A', 'V', 'E'};
constexpr uint32_t kContainerVersion = 1;
constexpr uint32_t kFlagCompressed = 1u << 0;
constexpr size_t kHeaderSize = 8 + 4 + 4 + 4 + 4 + 8;  // magic, формат, флаги, длина, размер, сумма

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
uint32_t GetU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Читает первые bytes байт файла. Ради заголовка слота читать файл целиком
// незачем — в этом вся разница между «меню открылось» и «меню думает».
bool ReadPrefix(const std::string& path, size_t bytes, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.resize(bytes);
    f.read((char*)out.data(), (std::streamsize)bytes);
    out.resize((size_t)f.gcount());
    return !out.empty();
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
    WriteOptions o;
    o.Version = version;
    return Write(slot, payloadJson, o);
}

bool Write(const std::string& slot, const std::string& payloadJson, const WriteOptions& options) {
    std::error_code ec;
    const std::string dir = Directory();
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Save") << "Не удалось создать каталог сохранений " << dir << ": "
                          << ec.message();
        return false;
    }

    // Прогресс разбирается ПЕРЕД записью, а не после. Кривой JSON, записанный в
    // файл, обнаружится при загрузке — то есть у игрока, и уже как «сохранение
    // повреждено». Проверить его здесь стоит один разбор, а не потерянную игру.
    json payload;
    try {
        payload = json::parse(payloadJson);
    } catch (const std::exception& e) {
        LOG_ERROR("Save") << "Прогресс не является корректным JSON: " << e.what();
        return false;
    }

    json header;
    header["version"] = options.Version;
    header["savedAt"] = NowUnix();
    if (!options.MetaJson.empty()) {
        try {
            header["meta"] = json::parse(options.MetaJson);
        } catch (const std::exception& e) {
            // Метка — украшение меню, и ронять из-за неё сохранение нельзя:
            // прогресс важнее подписи к нему.
            LOG_WARN("Save") << "Метка слота не разобралась, сохраняю без неё: " << e.what();
        }
    }

    // Прогресс — БЕЗ отступов. Он не предназначен для чтения глазами, а отступы
    // в файле из тысяч коротких чисел занимают больше места, чем сами числа.
    const std::string payloadText = payload.dump();
    std::vector<uint8_t> body(payloadText.begin(), payloadText.end());
    const uint64_t rawSize = body.size();
    uint32_t flags = 0;
    if (options.Compress) {
        std::vector<uint8_t> packed;
        // Жмём, только если СТАЛО МЕНЬШЕ: у крошечного сохранения заголовок
        // deflate больше выигрыша, и платить за него распаковкой не за что.
        if (assets::DeflateBytes(body, packed) && packed.size() < body.size()) {
            body = std::move(packed);
            flags |= kFlagCompressed;
        }
    }
    const std::string headerText = header.dump();

    std::vector<uint8_t> file;
    file.reserve(kHeaderSize + headerText.size() + body.size());
    file.insert(file.end(), kMagic, kMagic + 8);
    PutU32(file, kContainerVersion);
    PutU32(file, flags);
    PutU32(file, (uint32_t)headerText.size());
    PutU32(file, (uint32_t)rawSize);   // размер РАСПАКОВАННОГО прогресса
    PutU64(file, assets::HashBytes(body.data(), body.size()));
    file.insert(file.end(), headerText.begin(), headerText.end());
    file.insert(file.end(), body.begin(), body.end());

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
        f.write((const char*)file.data(), (std::streamsize)file.size());
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

    // Прошлое сохранение отходит в .bak ДО замены. Атомарная замена спасает от
    // падения ПОСРЕДИ записи, но от самой записи не спасает: игра, сохранившаяся
    // в состояние, из которого не выбраться, уносила с собой единственную
    // рабочую копию. Копия — одна: хранить их десяток значит съесть диск
    // игрока ради случая, который бывает раз в игру.
    if (options.KeepBackup && fs::exists(finalPath, ec)) {
        fs::remove(BackupFile(slot), ec);
        fs::rename(finalPath, BackupFile(slot), ec);
        if (ec) {
            // Не вышло — не повод не сохраниться: копия это подстраховка, а не
            // условие. Но сказать надо, иначе игрок будет думать, что она есть.
            LOG_WARN("Save") << "Резервная копия не сделана: " << ec.message();
            ec.clear();
        }
    }

    fs::rename(tempPath, finalPath, ec);
    if (ec) {
        LOG_ERROR("Save") << "Не удалось заменить " << finalPath << ": " << ec.message();
        fs::remove(tempPath, ec);
        return false;
    }
    LOG_INFO("Save") << "Прогресс сохранён: " << finalPath << " (" << file.size() << " Б"
                     << ((flags & kFlagCompressed)
                             ? ", сжато с " + std::to_string(rawSize) + " Б"
                             : "")
                     << ")";
    return true;
}

namespace {

// Разбирает файл слота. onlyHeader — не трогать прогресс вовсе (для меню).
//
// Старый формат — голый JSON {"sage_save_version","savedAt","data"} — читается
// здесь же: у игроков уже есть такие файлы, и первое же обновление игры не
// имеет права их потерять.
bool ParseSlotFile(const std::string& path, bool onlyHeader, SlotInfo* info,
                   std::string* outPayload) {
    std::vector<uint8_t> prefix;
    if (!ReadPrefix(path, kHeaderSize, prefix) || prefix.size() < kHeaderSize ||
        std::memcmp(prefix.data(), kMagic, 8) != 0) {
        // Не наш контейнер — пробуем прежний формат.
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        json root;
        try {
            f >> root;
        } catch (const std::exception&) {
            return false;
        }
        if (!root.contains("data")) return false;
        if (info) {
            info->Version = root.value("sage_save_version", 1);
            info->SavedAtUnix = root.value("savedAt", 0LL);
            info->Compressed = false;
        }
        if (outPayload) *outPayload = root["data"].dump();
        return true;
    }

    const uint32_t container = GetU32(prefix.data() + 8);
    if (container > kContainerVersion) {
        LOG_ERROR("Save") << "Сохранение записано более новой версией движка: " << path;
        return false;
    }
    const uint32_t flags = GetU32(prefix.data() + 12);
    const uint32_t headerLen = GetU32(prefix.data() + 16);
    const uint32_t rawSize = GetU32(prefix.data() + 20);
    const uint64_t hash = GetU64(prefix.data() + 24);

    // Заголовок — единственное, что читает меню. Читаем ровно его.
    std::vector<uint8_t> headerBytes;
    if (!ReadPrefix(path, kHeaderSize + headerLen, headerBytes) ||
        headerBytes.size() < kHeaderSize + headerLen)
        return false;
    json header;
    try {
        header = json::parse(std::string((const char*)headerBytes.data() + kHeaderSize, headerLen));
    } catch (const std::exception& e) {
        LOG_ERROR("Save") << "Заголовок сохранения повреждён (" << path << "): " << e.what();
        return false;
    }
    if (info) {
        info->Version = header.value("version", 1);
        info->SavedAtUnix = header.value("savedAt", 0LL);
        info->Compressed = (flags & kFlagCompressed) != 0;
        info->Meta = header.contains("meta") ? header["meta"].dump() : "{}";
    }
    if (onlyHeader || !outPayload) return true;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)(kHeaderSize + headerLen));
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

    // Контрольная сумма — ДО разбора: испорченный файл честнее назвать
    // испорченным, чем отдать игре прогресс с половиной значений.
    if (assets::HashBytes(body.data(), body.size()) != hash) {
        LOG_ERROR("Save") << "Сохранение повреждено (не сходится контрольная сумма): " << path;
        return false;
    }

    if (flags & kFlagCompressed) {
        std::vector<uint8_t> raw;
        if (!assets::InflateBytes(body, rawSize, raw)) {
            LOG_ERROR("Save") << "Сохранение не распаковалось: " << path;
            return false;
        }
        body = std::move(raw);
    }
    *outPayload = std::string((const char*)body.data(), body.size());
    return true;
}

} // namespace

bool Read(const std::string& slot, std::string& outPayloadJson, int* outVersion) {
    SlotInfo info;
    if (!ParseSlotFile(SlotPath(slot), /*onlyHeader=*/false, &info, &outPayloadJson)) return false;
    if (outVersion) *outVersion = info.Version;
    return true;
}

bool ReadInfo(const std::string& slot, SlotInfo& out) {
    out.Name = SafeSlot(slot);
    std::error_code ec;
    out.Bytes = (size_t)fs::file_size(SlotFile(slot), ec);
    if (ec) return false;
    out.Broken = !ParseSlotFile(SlotPath(slot), /*onlyHeader=*/true, &out, nullptr);
    return !out.Broken;
}

bool HasBackup(const std::string& slot) {
    std::error_code ec;
    return fs::exists(BackupFile(slot), ec);
}

bool RestoreBackup(const std::string& slot) {
    std::error_code ec;
    const std::string backup = BackupPath(slot);
    if (!fs::exists(backup, ec)) return false;
    // КОПИЕЙ, а не переименованием: откат не должен быть односторонним. Иначе
    // человек, откатившийся по ошибке, теряет то, на что откатился бы обратно.
    fs::copy_file(backup, SlotFile(slot), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        LOG_ERROR("Save") << "Откат на резервную копию не удался: " << ec.message();
        return false;
    }
    LOG_INFO("Save") << "Слот восстановлен из резервной копии: " << slot;
    return true;
}

bool Exists(const std::string& slot) {
    std::error_code ec;
    return fs::exists(SlotFile(slot), ec);
}

bool Delete(const std::string& slot) {
    std::error_code ec;
    // Вместе с резервной копией: «удалить сохранение» и значит удалить его, а
    // оставшийся .bak воскрес бы при следующем откате и выглядел бы как
    // «удаление не сработало».
    fs::remove(BackupFile(slot), ec);
    return fs::remove(SlotFile(slot), ec);
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
        // ТОЛЬКО ЗАГОЛОВОК. Ради строчки в меню читается сотня байт, а не весь
        // файл: восемь слотов по пять мегабайт означали сорок мегабайт разбора
        // на каждое открытие меню, и весь результат выбрасывался.
        info.Broken = !ParseSlotFile(e.path().string(), /*onlyHeader=*/true, &info, nullptr);
        // Битый слот всё равно показываем: игрок должен видеть, что он есть, и
        // иметь возможность его удалить. Молча прятать хуже.
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const SlotInfo& a, const SlotInfo& b) { return a.SavedAtUnix > b.SavedAtUnix; });
    return out;
}

} // namespace sage::save
