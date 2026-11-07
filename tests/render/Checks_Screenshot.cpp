// ---------------------------------------------------------------------------
// Снимок экрана (render/Screenshot.h): кадр пишется PNG-файлом, строки сверху
// вниз, папка создаётся сама, путь — UTF-8 (кириллица в имени пользователя).
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <stb_image.h>

#include "sage/core/Paths.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Screenshot.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rendertest {

void RunScreenshotChecks() {
    std::printf("=== Снимок экрана ===\n");
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    Framebuffer fb(kW, kH);
    fb.Bind();
    device.SetViewport(0, 0, kW, kH);
    device.SetClearColor(0.0f, 0.0f, 1.0f, 1.0f);   // низ — синий
    device.Clear(true, true);
    device.SetClearColor(1.0f, 0.0f, 0.0f, 1.0f);   // верхняя половина — красная
    device.SetScissor(true, 0, kH / 2, kW, kH / 2);
    device.Clear(true, false);
    device.SetScissor(false);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / sage::PathFromUtf8("снимки_sage");
    std::filesystem::remove_all(dir);
    const std::string path = sage::PathToUtf8(dir / "кадр.png");
    const bool ok = SaveScreenshot(path, kW, kH);
    const Image expected = Capture(kW, kH);   // тот же кадр проверенным путём тестов
    device.BindDefaultFramebuffer();
    Check(ok, "снимок записан (папка с кириллицей создана сама)");

    std::vector<unsigned char> bytes;
    {
        std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load_thread(false);   // файл как есть, сверху вниз
    unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 3);
    Check(px && w == kW && h == kH, "PNG читается, размер кадра");
    if (px) {
        // Два участка разного цвета обязаны оказаться на тех же строках, что
        // и в проверенном снимке тестов: перевёрнутый файл здесь и ловится.
        bool same = true;
        for (int y : {kH / 4, kH * 3 / 4})
            for (int c = 0; c < 3; ++c)
                same = same && px[((size_t)y * kW + kW / 2) * 3 + c] == expected.Pixels[((size_t)y * kW + kW / 2) * 3 + c];
        const bool twoColors = px[((size_t)(kH / 4) * kW + kW / 2) * 3] != px[((size_t)(kH * 3 / 4) * kW + kW / 2) * 3];
        Check(same && twoColors, "строки сверху вниз, как в кадре");
        stbi_image_free(px);
    }
    std::filesystem::remove_all(dir);
}

} // namespace sage::rendertest
