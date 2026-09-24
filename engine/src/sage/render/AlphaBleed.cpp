#include "sage/render/AlphaBleed.h"

#include <cstdint>

namespace sage::render {

void BleedTransparentColor(std::vector<unsigned char>& rgba, int w, int h, int passes) {
    if (w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return;
    const size_t count = (size_t)w * h;
    constexpr unsigned char kEmpty = 16;

    // 1 — пиксель со «своим» цветом (непрозрачный или уже получивший цвет).
    std::vector<uint8_t> filled(count);
    uint64_t sum[3] = {0, 0, 0};
    size_t solid = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* p = &rgba[i * 4];
        if (p[3] >= kEmpty) {
            filled[i] = 1;
            sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
            ++solid;
        }
    }
    if (solid == 0 || solid == count) return;

    // Растекание: пустой пиксель берёт среднее заполненных соседей (8 штук).
    // Новые значения копятся отдельно и применяются после прохода — иначе цвет
    // растекался бы в сторону обхода дальше, чем в остальные.
    std::vector<size_t> grown;
    std::vector<unsigned char> grownColor;
    for (int pass = 0; pass < passes; ++pass) {
        grown.clear();
        grownColor.clear();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (filled[i]) continue;
                unsigned acc[3] = {0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx;
                        if ((dx == 0 && dy == 0) || xx < 0 || xx >= w) continue;
                        const size_t j = (size_t)yy * w + xx;
                        if (!filled[j]) continue;
                        acc[0] += rgba[j * 4]; acc[1] += rgba[j * 4 + 1]; acc[2] += rgba[j * 4 + 2];
                        ++n;
                    }
                }
                if (n == 0) continue;
                grown.push_back(i);
                grownColor.push_back((unsigned char)(acc[0] / n));
                grownColor.push_back((unsigned char)(acc[1] / n));
                grownColor.push_back((unsigned char)(acc[2] / n));
            }
        }
        if (grown.empty()) break;
        for (size_t k = 0; k < grown.size(); ++k) {
            unsigned char* p = &rgba[grown[k] * 4];
            p[0] = grownColor[k * 3]; p[1] = grownColor[k * 3 + 1]; p[2] = grownColor[k * 3 + 2];
            filled[grown[k]] = 1;
        }
    }

    // Остаток — средним цветом: для дальних мип-уровней важен именно он, а
    // растекать по всей картинке дорого и незачем.
    const unsigned char mean[3] = {(unsigned char)(sum[0] / solid), (unsigned char)(sum[1] / solid),
                                   (unsigned char)(sum[2] / solid)};
    for (size_t i = 0; i < count; ++i) {
        if (filled[i]) continue;
        unsigned char* p = &rgba[i * 4];
        p[0] = mean[0]; p[1] = mean[1]; p[2] = mean[2];
    }
}

} // namespace sage::render
