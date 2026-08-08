#include "sage/assets/AssetCache.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace sage::assets {
namespace {

// 'SGMC' — Sage Model Cache. Версию поднимаем при ЛЮБОМ изменении раскладки:
// кэш от другой версии читать нельзя, а молча прочитанный мусор дал бы модель
// с перепутанными вершинами, и искать причину пришлось бы в рендере.
constexpr unsigned int kMagic = 0x434D4753u;
// Версия ФОРМАТА И ЗАГРУЗЧИКА. Поднимать её обязан любой, кто чинит разбор
// исходника, а не только тот, кто меняет раскладку полей: кэш хранит РЕЗУЛЬТАТ
// разбора, и после исправления загрузчика старая запись продолжает отдавать
// сломанные данные — молча, потому что файл-исходник не изменился. Ровно так
// пережила правку перевёрнутая палитра glTF (см. GltfSkinImageLoader).
constexpr unsigned int kVersion = 2;

struct State {
    std::string Directory = ".sage-cache";
    bool Enabled = true;
    Stats CacheStats;
};

State& Get() {
    static State state;
    return state;
}

// --- Запись ----------------------------------------------------------------
struct Writer {
    std::ofstream File;
    bool Ok = true;

    void Raw(const void* data, size_t bytes) {
        if (!Ok || bytes == 0) return;
        File.write(static_cast<const char*>(data), (std::streamsize)bytes);
        if (!File) Ok = false;
    }
    template <typename T> void Pod(const T& value) { Raw(&value, sizeof(T)); }
    template <typename T> void Array(const std::vector<T>& v) {
        Pod((unsigned long long)v.size());
        Raw(v.data(), v.size() * sizeof(T));
    }
    void Text(const std::string& s) {
        Pod((unsigned long long)s.size());
        Raw(s.data(), s.size());
    }
};

// --- Чтение ----------------------------------------------------------------
// Читаем из ОДНОГО буфера, а не потоком: файл кэша целиком помещается в память
// (он и так весь нужен), а разбор из буфера убирает тысячи мелких read'ов —
// ради этого кэш и затевался.
struct Reader {
    const unsigned char* Data = nullptr;
    size_t Size = 0;
    size_t At = 0;
    bool Ok = true;

    bool Raw(void* out, size_t bytes) {
        if (!Ok || At + bytes > Size) { Ok = false; return false; }
        std::memcpy(out, Data + At, bytes);
        At += bytes;
        return true;
    }
    template <typename T> bool Pod(T& value) { return Raw(&value, sizeof(T)); }
    template <typename T> bool Array(std::vector<T>& v) {
        unsigned long long count = 0;
        if (!Pod(count)) return false;
        // Защита от повреждённого файла: без неё «размер» из мусора уводит
        // resize в аварию по памяти ещё до того, как мы поймём, что файл битый.
        if (count > (unsigned long long)(Size - At) / sizeof(T) + 1) { Ok = false; return false; }
        v.resize((size_t)count);
        return Raw(v.data(), v.size() * sizeof(T));
    }
    bool Text(std::string& s) {
        unsigned long long count = 0;
        if (!Pod(count)) return false;
        if (count > (unsigned long long)(Size - At)) { Ok = false; return false; }
        s.resize((size_t)count);
        return Raw(s.data(), s.size());
    }
};

// Отпечаток исходника: размер и время правки. Хэш содержимого был бы надёжнее,
// но чтение всего файла ради проверки съело бы часть той самой экономии, ради
// которой кэш и нужен.
struct Fingerprint {
    unsigned long long Size = 0;
    long long Time = 0;
};

bool FingerprintOf(const std::string& path, Fingerprint& out) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return false;
    out.Size = (unsigned long long)size;
    out.Time = (long long)time.time_since_epoch().count();
    return true;
}

} // namespace

void SetCacheDirectory(const std::string& directory) {
    // Каталог кэша запоминается АБСОЛЮТНЫМ. Относительный («.sage-cache»)
    // считается от текущего каталога, а его меняют на ходу: плеер уходит в
    // папку проекта сразу после старта. Кэш, записанный до смены, искался бы
    // после неё в другом месте — снова «пишется, но не читается».
    std::error_code ec;
    const fs::path absolute = fs::absolute(directory, ec);
    Get().Directory = ec ? directory : absolute.string();
}
const std::string& CacheDirectory() { return Get().Directory; }
void SetCacheEnabled(bool enabled) { Get().Enabled = enabled; }
bool CacheEnabled() { return Get().Enabled; }
const Stats& CacheStats() { return Get().CacheStats; }
void ResetCacheStats() { Get().CacheStats = Stats{}; }

std::string CachePathFor(const std::string& sourcePath) {
    std::error_code ec;
    // weakly_canonical, а НЕ absolute. absolute только приписывает текущий
    // каталог, оставляя «.», «..» и символические ссылки как есть — то есть
    // ключ получается от НАПИСАНИЯ пути, а не от файла.
    //
    // Один и тот же файл движок называет по-разному: сцена хранит путь
    // относительно проекта ("assets/hero.gltf"), файловый диалог отдаёт
    // абсолютный, склейка каталогов даёт "./assets/../assets/hero.gltf". С
    // ключом от написания запись и чтение попадали в РАЗНЫЕ файлы: кэш
    // исправно писался и никогда не читался, а на диске росло по копии на
    // каждое написание. Снаружи это выглядело как «кэш не работает».
    fs::path key = fs::weakly_canonical(sourcePath, ec);
    if (ec || key.empty()) {
        key = fs::absolute(sourcePath, ec);
        if (ec) key = sourcePath;
    }
    const std::string keyText = key.generic_string();

    // Имя файла = имя модели + хэш полного пути. Имя нужно человеку (в каталоге
    // кэша видно, что от чего), хэш — чтобы две модели с одинаковым именем из
    // разных папок не затирали друг друга.
    unsigned long long hash = 1469598103934665603ull; // FNV-1a
    for (unsigned char c : keyText) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "%016llx", hash);
    return (fs::path(Get().Directory) / (key.stem().string() + "_" + suffix + ".sagemc")).string();
}

bool ReadModelCache(const std::string& sourcePath, sage::render::ModelData& out) {
    if (!Get().Enabled) return false;

    Fingerprint source;
    if (!FingerprintOf(sourcePath, source)) return false;

    const std::string path = CachePathFor(sourcePath);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0);

    std::vector<unsigned char> bytes((size_t)size);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
    file.close();

    Reader r{bytes.data(), bytes.size(), 0, true};
    unsigned int magic = 0, version = 0;
    Fingerprint stored;
    r.Pod(magic);
    r.Pod(version);
    r.Pod(stored);
    if (!r.Ok || magic != kMagic || version != kVersion) return false;
    if (stored.Size != source.Size || stored.Time != source.Time) return false;

    // --- Скелет ---
    unsigned long long jointCount = 0;
    if (!r.Pod(jointCount) || jointCount > 100000ull) return false;
    out.Skeleton.Joints.resize((size_t)jointCount);
    for (auto& j : out.Skeleton.Joints) {
        r.Text(j.Name);
        r.Pod(j.Parent);
        r.Pod(j.Translation);
        r.Pod(j.Rotation);
        r.Pod(j.Scale);
        r.Pod(j.InverseBind);
    }

    // --- Клипы ---
    unsigned long long clipCount = 0;
    if (!r.Pod(clipCount) || clipCount > 100000ull) return false;
    out.Clips.resize((size_t)clipCount);
    for (auto& clip : out.Clips) {
        r.Text(clip.Name);
        r.Pod(clip.Duration);
        unsigned long long channels = 0;
        if (!r.Pod(channels) || channels > 1000000ull) return false;
        clip.Channels.resize((size_t)channels);
        for (auto& ch : clip.Channels) {
            r.Pod(ch.Joint);
            r.Pod(ch.Target);
            r.Pod(ch.Interp);
            r.Array(ch.Times);
            r.Array(ch.Values);
        }
    }

    // --- Изображения ---
    unsigned long long imageCount = 0;
    if (!r.Pod(imageCount) || imageCount > 4096ull) return false;
    out.Images.resize((size_t)imageCount);
    for (auto& img : out.Images) {
        r.Pod(img.Width);
        r.Pod(img.Height);
        r.Array(img.Pixels);
    }

    // --- Подмеши ---
    unsigned long long subCount = 0;
    if (!r.Pod(subCount) || subCount > 100000ull) return false;
    out.SubMeshes.resize((size_t)subCount);
    for (auto& sub : out.SubMeshes) {
        r.Array(sub.Vertices);
        r.Array(sub.Indices);
        r.Pod(sub.Image);
        r.Pod(sub.Tint);
        r.Pod(sub.Metallic);
        r.Pod(sub.Roughness);
        r.Pod(sub.MorphCount);
        r.Pod(sub.MorphWidth);
        r.Pod(sub.MorphRows);
        r.Array(sub.MorphPositions);
        r.Array(sub.MorphNormals);
    }

    // --- Морфы модели ---
    unsigned long long nameCount = 0;
    if (!r.Pod(nameCount) || nameCount > 100000ull) return false;
    out.MorphNames.resize((size_t)nameCount);
    for (auto& n : out.MorphNames) r.Text(n);
    r.Array(out.MorphDefaults);

    if (!r.Ok) {
        LOG_WARN("Assets") << "Кэш повреждён, читаю исходник: " << path;
        out = sage::render::ModelData{};
        return false;
    }

    ++Get().CacheStats.Hits;
    Get().CacheStats.BytesRead += (long long)bytes.size();
    return true;
}

bool WriteModelCache(const std::string& sourcePath, const sage::render::ModelData& data) {
    if (!Get().Enabled) return false;

    Fingerprint source;
    if (!FingerprintOf(sourcePath, source)) return false;

    std::error_code ec;
    fs::create_directories(Get().Directory, ec);

    const std::string path = CachePathFor(sourcePath);
    // Пишем во временный файл и переименовываем. Если процесс упадёт на
    // середине записи, на месте кэша останется целый старый файл, а не
    // обрубок, который следующий запуск примет за настоящий.
    const std::string temp = path + ".tmp";

    Writer w;
    w.File.open(temp, std::ios::binary | std::ios::trunc);
    if (!w.File) return false;

    w.Pod(kMagic);
    w.Pod(kVersion);
    w.Pod(source);

    w.Pod((unsigned long long)data.Skeleton.Joints.size());
    for (const auto& j : data.Skeleton.Joints) {
        w.Text(j.Name);
        w.Pod(j.Parent);
        w.Pod(j.Translation);
        w.Pod(j.Rotation);
        w.Pod(j.Scale);
        w.Pod(j.InverseBind);
    }

    w.Pod((unsigned long long)data.Clips.size());
    for (const auto& clip : data.Clips) {
        w.Text(clip.Name);
        w.Pod(clip.Duration);
        w.Pod((unsigned long long)clip.Channels.size());
        for (const auto& ch : clip.Channels) {
            w.Pod(ch.Joint);
            w.Pod(ch.Target);
            w.Pod(ch.Interp);
            w.Array(ch.Times);
            w.Array(ch.Values);
        }
    }

    w.Pod((unsigned long long)data.Images.size());
    for (const auto& img : data.Images) {
        w.Pod(img.Width);
        w.Pod(img.Height);
        w.Array(img.Pixels);
    }

    w.Pod((unsigned long long)data.SubMeshes.size());
    for (const auto& sub : data.SubMeshes) {
        w.Array(sub.Vertices);
        w.Array(sub.Indices);
        w.Pod(sub.Image);
        w.Pod(sub.Tint);
        w.Pod(sub.Metallic);
        w.Pod(sub.Roughness);
        w.Pod(sub.MorphCount);
        w.Pod(sub.MorphWidth);
        w.Pod(sub.MorphRows);
        w.Array(sub.MorphPositions);
        w.Array(sub.MorphNormals);
    }

    w.Pod((unsigned long long)data.MorphNames.size());
    for (const auto& n : data.MorphNames) w.Text(n);
    w.Array(data.MorphDefaults);

    w.File.close();
    if (!w.Ok) {
        fs::remove(temp, ec);
        return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    ++Get().CacheStats.Misses;
    return true;
}

} // namespace sage::assets
