// Шрифт-картинка: ширины символов читаются по пикселям сетки 16×16.
#include "TestFramework.h"

#include <vector>

#include "sage/render/Font.h"

namespace {
// Картинка 128×128 (ячейка 8): в ячейке n закрашены столбцы 0..cols-1.
std::vector<unsigned char> Sheet(int cell, int cols, int size = 128) {
    std::vector<unsigned char> px((size_t)size * size * 4, 0);
    const int cw = size / 16;
    const int cx = (cell % 16) * cw, cy = (cell / 16) * cw;
    for (int y = 0; y < cw; ++y)
        for (int x = 0; x < cols; ++x) {
            unsigned char* p = &px[((size_t)(cy + y) * size + cx + x) * 4];
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    return px;
}
} // namespace

TEST(ImageFont_width_from_last_filled_column) {
    // «A» нарисована в 5 столбцах из 8: шаг — 5 + 1 промежуток.
    std::vector<unsigned char> px = Sheet('A', 5);
    std::vector<int> u = ImageFontUnits(px.data(), 128, 128);
    CHECK_EQ(u['A'], 6);
    // Пробел пуст, но шаг у него есть (4); пустая ячейка — символа нет.
    CHECK_EQ(u[' '], 4);
    CHECK_EQ(u['B'], 0);
}

TEST(ImageFont_width_scales_to_eight_pixel_units) {
    // Ячейка 16 (картинка 256): 10 столбцов рисунка — 5 единиц сетки на 8, + 1.
    std::vector<unsigned char> px = Sheet('W', 10, 256);
    std::vector<int> u = ImageFontUnits(px.data(), 256, 256);
    CHECK_EQ(u['W'], 6);
}
