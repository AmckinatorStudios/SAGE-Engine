#include "sage/assets/ModelData.h"

#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace sage::assets {

const char* ModelDataFolder(ModelData kind) {
    switch (kind) {
        case ModelData::Materials: return "Materials";
        case ModelData::Textures: return "Textures";
        case ModelData::Animations: return "Animations";
    }
    return "Data";
}

fs::path ModelHomeDir(const fs::path& modelPath) {
    const fs::path dir = modelPath.parent_path();
    // Модель уже в своей папке (так её кладёт импорт: assets/Steve/Steve.gltf)
    // — её данные рядом. Иначе — в папке с её именем рядом с файлом.
    if (dir.filename() == modelPath.stem()) return dir;
    return dir / modelPath.stem();
}

fs::path ModelDataDir(const fs::path& modelPath, ModelData kind) {
    return ModelHomeDir(modelPath) / ModelDataFolder(kind);
}

fs::path ModelDataPath(const fs::path& modelPath, ModelData kind, const std::string& fileName) {
    std::error_code ec;
    // Файлы, разложенные по-старому, остаются на месте и используются — на них
    // ссылаются сцены: сперва рядом с моделью, потом в общей на папку
    // Materials/ (раскладка, у которой данные моделей перемешивались).
    const fs::path legacy = modelPath.parent_path() / fileName;
    if (fs::exists(legacy, ec)) return legacy;
    const fs::path shared = modelPath.parent_path() / ModelDataFolder(kind) / fileName;
    if (fs::exists(shared, ec) && ModelHomeDir(modelPath) != modelPath.parent_path()) return shared;
    return ModelDataDir(modelPath, kind) / fileName;
}

uint64_t PixelHash(const unsigned char* pixels, int w, int h, int channels) {
    // FNV-1a по размерам и байтам: детерминирован между запусками и машинами,
    // а этого и ждут от индекса, лежащего в проекте.
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&hsh](uint64_t v) {
        hsh ^= v;
        hsh *= 1099511628211ull;
    };
    mix((uint64_t)w);
    mix((uint64_t)h);
    mix((uint64_t)channels);
    const size_t n = pixels ? (size_t)w * (size_t)h * (size_t)channels : 0;
    for (size_t i = 0; i < n; ++i) mix(pixels[i]);
    return hsh;
}

namespace {
// Индекс читается и пишется под замком: импорт идёт и из превью ассетов в
// фоновом потоке.
std::mutex& IndexMutex() {
    static std::mutex m;
    return m;
}
} // namespace

fs::path FindSharedImage(const fs::path& dir, uint64_t hash) {
    std::lock_guard<std::mutex> lock(IndexMutex());
    std::ifstream in(dir / kSharedImageIndex);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        uint64_t h = 0;
        std::string name;
        if (!(ls >> std::hex >> h)) continue;
        std::getline(ls >> std::ws, name);
        if (h != hash || name.empty()) continue;
        const fs::path file = dir / fs::u8path(name);
        std::error_code ec;
        if (fs::exists(file, ec)) return file;   // файл удалили — запись мертва, пишем заново
    }
    return {};
}

void RememberSharedImage(const fs::path& dir, uint64_t hash, const fs::path& file) {
    std::lock_guard<std::mutex> lock(IndexMutex());
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / kSharedImageIndex, std::ios::app);
    out << std::hex << hash << " " << file.filename().u8string() << "\n";
}

} // namespace sage::assets
