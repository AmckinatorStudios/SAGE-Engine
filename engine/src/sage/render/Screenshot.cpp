#include "Screenshot.h"
#include "sage/rhi/GraphicsDevice.h"
#include <fstream>
#include <vector>
#include <cstring>
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

#include <cstdio>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool SaveScreenshot(const std::string& path, int width, int height) {
    if (width < 1 || height < 1) return false;
    // Папка снимков появляется с первым снимком, а не заранее.
    std::error_code ec;
    const std::filesystem::path file = sage::PathFromUtf8(path);
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    // Чтение кадра — через графическое устройство (оно само дожидается
    // завершения всех команд GPU перед чтением).
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    sage::rhi::GraphicsDevice::Get().ReadPixelsRGB(0, 0, width, height, pixels.data());

    // GPU отдаёт строки снизу вверх, PNG ожидает сверху вниз — переворачиваем
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < height; ++y) {
        std::memcpy(&flipped[static_cast<size_t>(y) * width * 3],
                    &pixels[static_cast<size_t>(height - 1 - y) * width * 3],
                    static_cast<size_t>(width) * 3);
    }

    // Запись через буфер в памяти и файл по пути UTF-8: stbi_write_png
    // открывает файл узкой строкой, и на Windows кириллица в пути (имя
    // пользователя в AppData) дала бы «не удалось сохранить».
    std::vector<unsigned char> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(ctx);
            out->insert(out->end(), (unsigned char*)data, (unsigned char*)data + size);
        },
        &png, width, height, 3, flipped.data(), width * 3);
    bool ok = !png.empty();
    if (ok) {
        std::ofstream f(file, std::ios::binary);
        f.write((const char*)png.data(), (std::streamsize)png.size());
        ok = (bool)f;
    }
    if (ok) LOG_INFO("Screenshot") << "Скриншот сохранён: " << path;
    else LOG_ERROR("Screenshot") << "Не удалось сохранить скриншот: " << path;
    return ok;
}

namespace sage::render {

std::string ScreenshotFileName(const std::string& dir, const std::tm& when) {
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d.%02d.%02d", when.tm_year + 1900, when.tm_mon + 1,
                  when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec);
    const std::string base = dir.empty() ? std::string(stamp) : dir + "/" + stamp;
    std::error_code ec;
    for (int i = 1;; ++i) {
        const std::string path = base + (i == 1 ? "" : "_" + std::to_string(i)) + ".png";
        if (!std::filesystem::exists(sage::PathFromUtf8(path), ec)) return path;
    }
}

std::string TimestampedScreenshotPath(const std::string& dir) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return ScreenshotFileName(dir, local);
}

} // namespace sage::render
