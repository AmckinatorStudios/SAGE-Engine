#include "sage/assets/Pack.h"

#include <stb_image.h>   // реализация — в render/Texture.cpp

// Картинки через vfs — отдельным файлом, а не в Pack.cpp: пакет собирает и
// упаковщик шаблонов (tools/sage_template), которому декодер картинок не нужен
// и не подключён.
namespace sage::assets::vfs {

unsigned char* LoadImage(const std::string& path, int* width, int* height, int* channels,
                         int desiredChannels) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes) || bytes.empty()) return nullptr;
    return stbi_load_from_memory(bytes.data(), (int)bytes.size(), width, height, channels,
                                 desiredChannels);
}

bool ImageInfo(const std::string& path, int* width, int* height, int* channels) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes) || bytes.empty()) return false;
    return stbi_info_from_memory(bytes.data(), (int)bytes.size(), width, height, channels) != 0;
}

} // namespace sage::assets::vfs
