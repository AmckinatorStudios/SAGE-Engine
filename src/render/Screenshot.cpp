#include "Screenshot.h"
#include <glad/glad.h>
#include <vector>
#include <cstring>
#include "../core/Log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void SaveScreenshot(const std::string& path, int width, int height) {
    // glReadPixels обязан неявно дождаться завершения всех предыдущих команд
    // рисования по спецификации OpenGL, но явный glFinish() здесь — дешёвая
    // (один раз на скриншот, не каждый кадр) защита от того, чтобы читать
    // кадр раньше, чем его дорисовал асинхронный/программный рендерер.
    glFinish();

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL отдаёт строки снизу вверх, PNG ожидает сверху вниз — переворачиваем
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
