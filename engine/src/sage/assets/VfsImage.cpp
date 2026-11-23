#include "sage/assets/Pack.h"

#include <stb_image.h>   // реализация — в sage/core/StbImage.cpp

// Картинки через vfs — отдельным файлом, а не в Pack.cpp: сам пакет декодера
// картинок не требует, и Pack.cpp не тянет stb_image.
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
