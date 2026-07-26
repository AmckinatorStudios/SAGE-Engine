#include "RenderTestHarness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

// Реализации stb уже собраны в движке (Texture.cpp и Screenshot.cpp) —
// подключаем только заголовки, иначе получили бы дубликаты символов.
#include "stb_image.h"
#include "stb_image_write.h"

#include "sage/rhi/GraphicsDevice.h"

namespace fs = std::filesystem;

namespace sage::rendertest {

namespace {
std::string g_referenceDir = "references";
bool g_update = false;

// Заметная для глаза разница. Ниже этого — шум точности разных драйверов, а не
// изменение картинки.
constexpr int kNoticeable = 2;
} // namespace

void SetReferenceDir(const std::string& dir) { g_referenceDir = dir; }
void SetUpdateMode(bool update) { g_update = update; }
bool UpdateMode() { return g_update; }

Image Capture(int width, int height) {
    Image img;
    img.Width = width;
    img.Height = height;
    img.Pixels.assign((size_t)width * height * 3u, 0);
    sage::rhi::GraphicsDevice::Get().ReadPixelsRGB(0, 0, width, height, img.Pixels.data());

    // GPU отдаёт строки снизу вверх — переворачиваем, чтобы PNG и сравнение
    // работали в обычной ориентации.
    const size_t stride = (size_t)width * 3u;
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top = img.Pixels.data() + (size_t)y * stride;
        unsigned char* bottom = img.Pixels.data() + (size_t)(height - 1 - y) * stride;
        std::swap_ranges(top, top + stride, bottom);
    }
    return img;
}

bool SavePng(const std::string& path, const Image& image) {
    if (image.Empty()) return false;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return stbi_write_png(path.c_str(), image.Width, image.Height, 3, image.Pixels.data(),
                          image.Width * 3) != 0;
}

bool LoadPng(const std::string& path, Image& out) {
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!data) return false;
    out.Width = w;
    out.Height = h;
    out.Pixels.assign(data, data + (size_t)w * h * 3u);
    stbi_image_free(data);
    return true;
}

Comparison CompareWithReference(const std::string& name, const Image& image) {
    Comparison result;
    const std::string refPath = (fs::path(g_referenceDir) / (name + ".png")).string();

    Image reference;
    const bool haveReference = !g_update && LoadPng(refPath, reference);
    if (!haveReference) {
        // Эталона нет (или его просят обновить) — записываем и честно говорим,
        // что сравнивать было не с чем: это НЕ «тест прошёл».
        SavePng(refPath, image);
        result.ReferenceExisted = false;
        return result;
    }
    result.ReferenceExisted = true;

    if (reference.Width != image.Width || reference.Height != image.Height) {
        // Другой размер — не с чем сравнивать попиксельно; отдаём заведомо
        // проваленный результат, а не пытаемся масштабировать.
        result.MaxDiff = 255;
        result.MeanDiff = 255.0;
        result.DiffFraction = 1.0;
        return result;
    }

    Image diff;
    diff.Width = image.Width;
    diff.Height = image.Height;
    diff.Pixels.assign(image.Pixels.size(), 0);

    double sum = 0.0;
    size_t noticeablePixels = 0;
    const size_t pixelCount = (size_t)image.Width * image.Height;
    for (size_t i = 0; i < pixelCount; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const size_t k = i * 3u + (size_t)c;
            const int d = std::abs((int)image.Pixels[k] - (int)reference.Pixels[k]);
            sum += d;
            worst = std::max(worst, d);
            // Карта различий — усиленная, иначе тонкая кромка в один-два тона
            // на глаз неотличима от чёрного поля.
            diff.Pixels[k] = (unsigned char)std::min(255, d * 8);
        }
        result.MaxDiff = std::max(result.MaxDiff, worst);
        if (worst > kNoticeable) ++noticeablePixels;
    }
    result.MeanDiff = sum / (double)(pixelCount * 3u);
    result.DiffFraction = (double)noticeablePixels / (double)pixelCount;

    // Артефакты кладём только при реальном расхождении: иначе каждый успешный
    // прогон засорял бы каталог эталонов.
    if (result.MaxDiff > kNoticeable) {
        SavePng((fs::path(g_referenceDir) / (name + ".actual.png")).string(), image);
        SavePng((fs::path(g_referenceDir) / (name + ".diff.png")).string(), diff);
    }
    return result;
}

} // namespace sage::rendertest
