#include "sage/render/TextureGen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sage::render {
namespace {

// --- Шум -------------------------------------------------------------------
//
// Value noise на ЦЕЛОЙ решётке с периодом: узел (x + period, y) — это тот же
// узел, что и (x, y). Отсюда бесшовность: правый край текстуры интерполируется
// к тем же значениям, что и левый.
//
// Хеш вместо таблицы перестановок: таблица дала бы то же качество, но её
// пришлось бы держать в статической памяти и инициализировать по seed, а здесь
// seed — просто ещё одно слагаемое.
inline float Hash(int x, int y, unsigned int seed) {
    unsigned int h = (unsigned int)(x * 374761393) + (unsigned int)(y * 668265263) + seed * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFF;
}

inline int Wrap(int v, int period) {
    if (period <= 0) return v;
    int r = v % period;
    return r < 0 ? r + period : r;
}

// Сглаживание Ken Perlin (6t^5-15t^4+10t^3): у него нулевая ВТОРАЯ производная
// на концах, поэтому границы клеток не видны. У обычного smoothstep вторая
// производная рвётся, и на шуме это читается сеткой.
inline float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

float ValueNoise(float x, float y, int period, unsigned int seed) {
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = Fade(x - (float)x0), fy = Fade(y - (float)y0);
    const int xa = Wrap(x0, period), xb = Wrap(x0 + 1, period);
    const int ya = Wrap(y0, period), yb = Wrap(y0 + 1, period);
    const float v00 = Hash(xa, ya, seed), v10 = Hash(xb, ya, seed);
    const float v01 = Hash(xa, yb, seed), v11 = Hash(xb, yb, seed);
    const float a = v00 + (v10 - v00) * fx;
    const float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
}

// Фрактальный шум: сумма октав с удвоением частоты. Период тоже удваивается —
// иначе бесшовной осталась бы только первая октава.
float Fbm(float u, float v, int octaves, float frequency, float persistence, unsigned int seed) {
    int period = std::max(1, (int)std::lround(frequency));
    float amp = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i = 0; i < std::max(1, octaves); ++i) {
        sum += ValueNoise(u * (float)period, v * (float)period, period, seed + (unsigned)i * 7919u) * amp;
        norm += amp;
        amp *= persistence;
        period *= 2;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

// Целое число клеток: половина клетки на краю — это шов, который повторится
// на каждом стыке. Не меньше единицы.
inline int Tiles(float v) { return std::max(1, (int)std::lround(v)); }

// Число клеток для ШАХМАТКИ — обязательно ЧЁТНОЕ.
//
// У шахматки соседние клетки всегда разного цвета, поэтому её край сходится с
// краем только при чётном числе клеток: при нечётном последняя клетка ряда и
// первая клетка следующей укладки оказываются одного цвета, и по всему полу
// идут сдвоенные полосы — тот самый шов, которого у процедурной текстуры быть
// не должно. Просить «пять клеток» законно, молча отдать шов — нет, поэтому
// число округляется вверх до чётного.
inline int CheckerTiles(float v) {
    const int n = Tiles(v);
    return (n % 2 == 0) ? n : n + 1;
}

inline float Frac(float v) { return v - std::floor(v); }

// Ближе ли координата к линии, чем половина её толщины. Мягко (smoothstep),
// иначе тонкая линия на дальней плитке рассыпается в пунктир до всяких
// мипмапов.
inline float LineMask(float coord, float width) {
    const float d = std::min(coord, 1.0f - coord);          // расстояние до края клетки
    const float half = std::max(width, 1e-4f) * 0.5f;
    if (d >= half) return 0.0f;
    const float t = 1.0f - d / half;
    return t * t * (3.0f - 2.0f * t);
}

// --- Значение узора в точке -------------------------------------------------
//
// Одна функция на все узоры возвращает ДОЛЮ второго цвета в точке (0 — ColorA,
// 1 — ColorB). Цвет собирается снаружи, поэтому карта нормалей получается из
// той же функции без единой ветки: доля и есть высота.
float PatternValue(const TextureRecipe& r, float u, float v) {
    switch (r.Kind) {
        case TextureRecipe::Pattern::Solid:
            return 0.0f;

        case TextureRecipe::Pattern::Checker: {
            const int cx = (int)std::floor(u * (float)CheckerTiles(r.TilesX));
            const int cy = (int)std::floor(v * (float)CheckerTiles(r.TilesY));
            return ((cx + cy) & 1) ? 1.0f : 0.0f;
        }

        case TextureRecipe::Pattern::Grid: {
            const float fx = Frac(u * (float)Tiles(r.TilesX));
            const float fy = Frac(v * (float)Tiles(r.TilesY));
            return std::max(LineMask(fx, r.LineWidth), LineMask(fy, r.LineWidth));
        }

        case TextureRecipe::Pattern::Bricks: {
            const int rows = Tiles(r.TilesY);
            const float ry = v * (float)rows;
            const int row = (int)std::floor(ry);
            // Смещение ряда — то, чем кладка отличается от сетки: без него
            // швы выстраиваются в сплошные вертикали, и стена выглядит
            // плиткой, а не кладкой.
            const float shift = (float)(row & 1) * r.Offset;
            const float rx = Frac(u * (float)Tiles(r.TilesX) + shift);
            return std::max(LineMask(rx, r.LineWidth), LineMask(Frac(ry), r.LineWidth));
        }

        case TextureRecipe::Pattern::Dots: {
            const float fx = Frac(u * (float)Tiles(r.TilesX)) - 0.5f;
            const float fy = Frac(v * (float)Tiles(r.TilesY)) - 0.5f;
            const float d = std::sqrt(fx * fx + fy * fy);
            const float radius = std::max(0.02f, std::min(0.5f, r.LineWidth * 4.0f));
            if (d >= radius) return 0.0f;
            const float t = 1.0f - d / radius;
            return t * t * (3.0f - 2.0f * t);
        }

        case TextureRecipe::Pattern::Gradient: {
            if (r.Radial) {
                const float dx = u - 0.5f, dy = v - 0.5f;
                return std::min(1.0f, std::sqrt(dx * dx + dy * dy) * 2.0f);
            }
            const float a = r.Angle * 3.14159265f / 180.0f;
            const float t = (u - 0.5f) * std::cos(a) + (v - 0.5f) * std::sin(a) + 0.5f;
            return std::clamp(t, 0.0f, 1.0f);
        }

        case TextureRecipe::Pattern::Noise:
            return Fbm(u, v, r.Octaves, r.Frequency, r.Persistence, r.Seed);
    }
    return 0.0f;
}

inline unsigned char ToByte(float v) {
    return (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
}

} // namespace

std::string TextureRecipe::Key() const {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "gen:%d/%dx%d/%.3f,%.3f,%.3f,%.3f/%.3f,%.3f,%.3f,%.3f/"
                  "%.2f,%.2f/%.3f/%.3f/%.1f,%d/%d,%.2f,%.2f,%u/%.3f/%d,%.2f",
                  (int)Kind, Width, Height, ColorA.x, ColorA.y, ColorA.z, ColorA.w,
                  ColorB.x, ColorB.y, ColorB.z, ColorB.w, TilesX, TilesY, LineWidth, Offset,
                  Angle, Radial ? 1 : 0, Octaves, Frequency, Persistence, Seed, Grain,
                  AsNormal ? 1 : 0, NormalStrength);
    return buf;
}

std::vector<unsigned char> GenerateTexturePixels(const TextureRecipe& recipe) {
    const int w = std::clamp(recipe.Width, 1, 4096);
    const int h = std::clamp(recipe.Height, 1, 4096);
    std::vector<unsigned char> pixels((size_t)w * (size_t)h * 4u, 255);

    // Высота узора считается ОТДЕЛЬНЫМ проходом: карте нормалей нужны соседи,
    // а считать соседей заново на каждый пиксель — это четыре лишних вычисления
    // узора там, где хватает одного.
    std::vector<float> height((size_t)w * (size_t)h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Центр пикселя, а не угол: по углам узор смещён на полтекселя, и
            // шахматка на стыке текстур сходится не клетка в клетку.
            const float u = ((float)x + 0.5f) / (float)w;
            const float v = ((float)y + 0.5f) / (float)h;
            float t = PatternValue(recipe, u, v);
            if (recipe.Grain > 0.0f) {
                const float g = Fbm(u, v, 3, 16.0f, 0.5f, recipe.Seed + 104729u) - 0.5f;
                t = std::clamp(t + g * recipe.Grain, 0.0f, 1.0f);
            }
            height[(size_t)y * (size_t)w + (size_t)x] = t;
        }
    }

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * (size_t)w + (size_t)x;
            unsigned char* px = &pixels[i * 4u];
            if (!recipe.AsNormal) {
                const float t = height[i];
                const glm::vec4 c = recipe.ColorA + (recipe.ColorB - recipe.ColorA) * t;
                px[0] = ToByte(c.r); px[1] = ToByte(c.g); px[2] = ToByte(c.b); px[3] = ToByte(c.a);
                continue;
            }
            // Нормаль из высот: центральная разность по соседям С ЗАВОРОТОМ.
            // Заворот обязателен — иначе по краям карты нормалей появляется
            // рамка, которая на полу читается сеткой поверх узора.
            const int xm = (x - 1 + w) % w, xp = (x + 1) % w;
            const int ym = (y - 1 + h) % h, yp = (y + 1) % h;
            const float hl = height[(size_t)y * (size_t)w + (size_t)xm];
            const float hr = height[(size_t)y * (size_t)w + (size_t)xp];
            const float hd = height[(size_t)ym * (size_t)w + (size_t)x];
            const float hu = height[(size_t)yp * (size_t)w + (size_t)x];
            glm::vec3 n(-(hr - hl) * recipe.NormalStrength,
                        -(hu - hd) * recipe.NormalStrength,
                        // Масштаб по Z привязан к размеру текстуры: без него
                        // сила рельефа зависела бы от разрешения, и та же
                        // формула на 1024 давала бы вдвое более пологий рельеф,
                        // чем на 512.
                        2.0f / (float)std::max(w, h) * 8.0f);
            n = glm::normalize(n);
            px[0] = ToByte(n.x * 0.5f + 0.5f);
            px[1] = ToByte(n.y * 0.5f + 0.5f);
            px[2] = ToByte(n.z * 0.5f + 0.5f);
            px[3] = 255;
        }
    }
    return pixels;
}

std::shared_ptr<Texture> GenerateTexture(const TextureRecipe& recipe, TextureFilter filter,
                                         bool mipmaps) {
    const std::vector<unsigned char> pixels = GenerateTexturePixels(recipe);
    const int w = std::clamp(recipe.Width, 1, 4096);
    const int h = std::clamp(recipe.Height, 1, 4096);
    return std::make_shared<Texture>(pixels.data(), w, h, filter, mipmaps);
}

bool ParseTexturePattern(const std::string& name, TextureRecipe::Pattern& out) {
    struct Entry { const char* Id; TextureRecipe::Pattern Kind; };
    static const Entry kEntries[] = {
        {"solid", TextureRecipe::Pattern::Solid},
        {"checker", TextureRecipe::Pattern::Checker},
        {"grid", TextureRecipe::Pattern::Grid},
        {"noise", TextureRecipe::Pattern::Noise},
        {"gradient", TextureRecipe::Pattern::Gradient},
        {"bricks", TextureRecipe::Pattern::Bricks},
        {"dots", TextureRecipe::Pattern::Dots},
    };
    for (const Entry& e : kEntries) {
        if (name == e.Id) { out = e.Kind; return true; }
    }
    return false;
}

} // namespace sage::render
