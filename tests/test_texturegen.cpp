// Процедурные текстуры: узор считается формулой, без файла на диске и без GL.
//
// Проверяется здесь не «картинка нарисовалась» — это видно и глазом, — а те
// три свойства, поломку которых глазом заметить труднее всего и позже всего:
//
//   БЕСШОВНОСТЬ. Текстура на полу повторяется десятки раз. Шов, не сошедшийся
//   по краю, виден не на одном стыке, а сразу на всех, и выглядит он как
//   сетка поверх узора — то есть как ошибка рендера, а не генератора.
//
//   СЕТКА КЛЕТОК. Шахматка нужна ради масштаба: если число клеток не то, что
//   заказали, пол врёт о размере, а по нему меряется вся сцена.
//
//   КАРТА НОРМАЛЕЙ. Плоский участок обязан давать нормаль строго вверх, край
//   узора — наклон. Ошибка знака здесь даёт вывернутый наизнанку рельеф,
//   который на статичном свете почти неотличим от правильного.
#include "TestFramework.h"

#include "sage/render/TextureGen.h"

#include <cmath>
#include <vector>

using sage::render::TextureRecipe;
using sage::render::GenerateTexturePixels;
using sage::render::ParseTexturePattern;

namespace {

const unsigned char* Px(const std::vector<unsigned char>& p, int w, int x, int y) {
    return &p[((size_t)y * (size_t)w + (size_t)x) * 4u];
}

TextureRecipe Checker(int size, int tiles) {
    TextureRecipe r;
    r.Kind = TextureRecipe::Pattern::Checker;
    r.Width = r.Height = size;
    r.TilesX = r.TilesY = (float)tiles;
    r.ColorA = {1.0f, 1.0f, 1.0f, 1.0f};
    r.ColorB = {0.0f, 0.0f, 0.0f, 1.0f};
    return r;
}

} // namespace

TEST(TextureGen_checker_has_exactly_the_requested_cells) {
    const int size = 64, tiles = 8;
    const std::vector<unsigned char> px = GenerateTexturePixels(Checker(size, tiles));
    CHECK_EQ(px.size(), (size_t)size * size * 4u);

    // Считаем переходы вдоль средней линии клетки: их обязано быть ровно
    // столько же, сколько клеток.
    const int row = size / (tiles * 2); // середина первого ряда клеток
    int flips = 0;
    for (int x = 1; x < size; ++x) {
        if (Px(px, size, x, row)[0] != Px(px, size, x - 1, row)[0]) ++flips;
    }
    CHECK_EQ(flips, tiles - 1);

    // Соседние по диагонали клетки — разного цвета: это и есть шахматка, а не
    // полосы.
    const int cell = size / tiles;
    CHECK_TRUE(Px(px, size, cell / 2, cell / 2)[0] != Px(px, size, cell + cell / 2, cell / 2)[0]);
    CHECK_TRUE(Px(px, size, cell / 2, cell / 2)[0] == Px(px, size, cell + cell / 2, cell + cell / 2)[0]);
}

TEST(TextureGen_patterns_are_seamless_at_the_edges) {
    // Столбец у левого края обязан продолжать столбец у правого: только так
    // текстура повторяется без видимой границы.
    //
    // Шахматки в списке НЕТ, и это не поблажка: у неё соседние клетки обязаны
    // отличаться, поэтому её края как раз ОБЯЗАНЫ не совпадать. Её
    // бесшовность — в чётности числа клеток, и она проверяется отдельно
    // (TextureGen_checker_tiles_without_a_double_row).
    const int size = 64;
    for (auto kind : {TextureRecipe::Pattern::Grid, TextureRecipe::Pattern::Bricks,
                      TextureRecipe::Pattern::Noise, TextureRecipe::Pattern::Dots}) {
        TextureRecipe r = Checker(size, 4);
        r.Kind = kind;
        r.Frequency = 4.0f;
        const std::vector<unsigned char> px = GenerateTexturePixels(r);

        int worstX = 0, worstY = 0;
        for (int i = 0; i < size; ++i) {
            worstX = std::max(worstX, std::abs((int)Px(px, size, 0, i)[0] -
                                               (int)Px(px, size, size - 1, i)[0]));
            worstY = std::max(worstY, std::abs((int)Px(px, size, i, 0)[0] -
                                               (int)Px(px, size, i, size - 1)[0]));
        }
        // Не ноль, а «в пределах одного текселя узора»: край и правда сходится
        // с краем, но соседние тексели узора отличаются законно.
        std::printf("       узор %d: расхождение краёв %d / %d (из 255)\n", (int)kind, worstX,
                    worstY);
        CHECK_TRUE(worstX <= 255 / 4);
        CHECK_TRUE(worstY <= 255 / 4);
    }
}

TEST(TextureGen_checker_tiles_without_a_double_row) {
    // Нечётное число клеток шахматка укладывать не умеет: последняя клетка
    // ряда и первая клетка следующей укладки совпали бы по цвету, и по полу
    // пошли бы сдвоенные полосы. Генератор округляет счёт до чётного — здесь
    // проверяется, что округляет, а не отдаёт шов.
    const int size = 64;
    const std::vector<unsigned char> px = GenerateTexturePixels(Checker(size, 5));
    const int row = 2;
    int flips = 0;
    for (int x = 1; x < size; ++x) {
        if (Px(px, size, x, row)[0] != Px(px, size, x - 1, row)[0]) ++flips;
    }
    // Пять клеток превратились в шесть: переходов на один меньше числа клеток.
    CHECK_EQ(flips, 5);
    // И края теперь разные — как и положено соседним клеткам шахматки.
    CHECK_TRUE(Px(px, size, 0, row)[0] != Px(px, size, size - 1, row)[0]);
}

TEST(TextureGen_normal_map_is_flat_where_the_pattern_is_flat) {
    TextureRecipe r = Checker(64, 4);
    r.Kind = TextureRecipe::Pattern::Grid;   // ровный фон + линии
    // Линия шириной в четверть клетки: тоньше текселя она попадает между
    // отсчётами, и проверять на ней нечего — это ограничение разрешения, а не
    // генератора.
    r.LineWidth = 0.25f;
    r.AsNormal = true;
    r.NormalStrength = 2.0f;
    const std::vector<unsigned char> px = GenerateTexturePixels(r);

    // Середина клетки — ровное место: нормаль смотрит вверх, то есть (0,0,1)
    // в тангенциальном пространстве -> (128,128,255) в байтах.
    const unsigned char* flat = Px(px, 64, 8, 8);
    CHECK_TRUE(std::abs((int)flat[0] - 128) <= 2);
    CHECK_TRUE(std::abs((int)flat[1] - 128) <= 2);
    CHECK_TRUE(flat[2] > 240);

    // На краю линии нормаль обязана отклониться: иначе рельефа нет вовсе.
    // Смотрим ВСЮ картинку: где именно проходит край, зависит от ширины линии
    // и разрешения, и привязываться к конкретной строке значит проверять
    // арифметику теста, а не генератор.
    int tilted = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const unsigned char* p = Px(px, 64, x, y);
            if (std::abs((int)p[0] - 128) > 12 || std::abs((int)p[1] - 128) > 12) ++tilted;
        }
    }
    std::printf("       наклонённых текселей на линии: %d\n", tilted);
    CHECK_TRUE(tilted > 0);
}

TEST(TextureGen_solid_fill_is_one_colour_and_grain_breaks_it) {
    TextureRecipe r = Checker(32, 1);
    r.Kind = TextureRecipe::Pattern::Solid;
    r.ColorA = {0.5f, 0.25f, 0.75f, 1.0f};
    std::vector<unsigned char> px = GenerateTexturePixels(r);
    for (int i = 0; i < 32 * 32; ++i) {
        CHECK_EQ((int)px[(size_t)i * 4u], (int)px[0]);
    }

    // Зерно — это то, что отличает материал от заливки: с ним пиксели обязаны
    // перестать быть одинаковыми.
    r.Grain = 0.5f;
    px = GenerateTexturePixels(r);
    int different = 0;
    for (int i = 1; i < 32 * 32; ++i) {
        if (px[(size_t)i * 4u] != px[0]) ++different;
    }
    CHECK_TRUE(different > 32 * 32 / 4);
}

TEST(TextureGen_recipe_key_tells_different_textures_apart) {
    // Ключ — это то, по чему текстуры кэшируются: совпал ключ у разных
    // рецептов — второй молча получит чужую картинку.
    TextureRecipe a = Checker(64, 4);
    TextureRecipe b = Checker(64, 8);
    CHECK_TRUE(a.Key() != b.Key());

    TextureRecipe c = a;
    CHECK_TRUE(a.Key() == c.Key());
    c.AsNormal = true;
    CHECK_TRUE(a.Key() != c.Key());
}

TEST(TextureGen_unknown_pattern_name_is_reported_not_guessed) {
    TextureRecipe::Pattern kind = TextureRecipe::Pattern::Solid;
    CHECK_TRUE(ParseTexturePattern("bricks", kind));
    CHECK_TRUE(kind == TextureRecipe::Pattern::Bricks);
    // Молча подставлять заливку нельзя: человек будет смотреть на однотонный
    // пол и думать, что сломан генератор.
    CHECK_TRUE(!ParseTexturePattern("marble", kind));
    CHECK_TRUE(!ParseTexturePattern("", kind));
}
