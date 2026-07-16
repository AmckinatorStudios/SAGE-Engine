#include "Screenshot.h"
#include "sage/rhi/GraphicsDevice.h"
#include <vector>
#include <cstring>
#include "sage/core/Log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void SaveScreenshot(const std::string& path, int width, int height) {
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

    if (stbi_write_png(path.c_str(), width, height, 3, flipped.data(), width * 3)) {
        LOG_INFO("Screenshot") << "Скриншот сохранён: " << path;
    } else {
        LOG_ERROR("Screenshot") << "Не удалось сохранить скриншот: " << path;
    }
}
