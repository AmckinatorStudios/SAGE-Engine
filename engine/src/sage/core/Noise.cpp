#include "sage/core/Noise.h"

#include <algorithm>
#include <cmath>

namespace sage::noise {

namespace {

// splitmix32: перемешивание зерна. Свой, а не std::mt19937: последовательность
// mt19937 определена стандартом, а вот std::shuffle и распределения — нет, и
// таблица вышла бы разной в MSVC и libstdc++.
uint32_t Mix(uint32_t& state) {
    uint32_t z = (state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

inline int FastFloor(float x) { return x >= 0.0f ? (int)x : (int)x - (x != (float)(int)x ? 1 : 0); }
inline float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Градиенты Перлина: 12 рёбер куба (классический «улучшенный» шум 2002 г.).
inline float Grad3(int h, float x, float y, float z) {
    h &= 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
inline float Grad2(int h, float x, float y) {
    switch (h & 7) {
        case 0: return x + y;
        case 1: return -x + y;
        case 2: return x - y;
        case 3: return -x - y;
        case 4: return x;
        case 5: return -x;
        case 6: return y;
        default: return -y;
    }
}

} // namespace

void Source::Reseed(uint32_t seed) {
    m_seed = seed;
    uint8_t p[256];
    for (int i = 0; i < 256; ++i) p[i] = (uint8_t)i;
    uint32_t st = seed;
    for (int i = 255; i > 0; --i) {
        const int j = (int)(Mix(st) % (uint32_t)(i + 1));
        std::swap(p[i], p[j]);
    }
    for (int i = 0; i < 512; ++i) m_perm[i] = p[i & 255];
}

float Source::Hash(int x, int y, int z) const {
    return (float)P(P(P(x) + y) + z) / 256.0f;
}

float Source::Perlin(float x, float y) const {
    const int X = FastFloor(x), Y = FastFloor(y);
    x -= (float)X;
    y -= (float)Y;
    const float u = Fade(x), v = Fade(y);
    const int a = P(X) + Y, b = P(X + 1) + Y;
    const float r = Lerp(Lerp(Grad2(P(a), x, y), Grad2(P(b), x - 1, y), u),
                         Lerp(Grad2(P(a + 1), x, y - 1), Grad2(P(b + 1), x - 1, y - 1), u), v);
    return r * 0.7071f;   // максимум двумерного градиентного шума — √2/2
}

float Source::Perlin(float x, float y, float z) const {
    const int X = FastFloor(x), Y = FastFloor(y), Z = FastFloor(z);
    x -= (float)X;
    y -= (float)Y;
    z -= (float)Z;
    const float u = Fade(x), v = Fade(y), w = Fade(z);
    const int A = P(X) + Y, AA = P(A) + Z, AB = P(A + 1) + Z;
    const int B = P(X + 1) + Y, BA = P(B) + Z, BB = P(B + 1) + Z;
    return Lerp(Lerp(Lerp(Grad3(P(AA), x, y, z), Grad3(P(BA), x - 1, y, z), u),
                     Lerp(Grad3(P(AB), x, y - 1, z), Grad3(P(BB), x - 1, y - 1, z), u), v),
                Lerp(Lerp(Grad3(P(AA + 1), x, y, z - 1), Grad3(P(BA + 1), x - 1, y, z - 1), u),
                     Lerp(Grad3(P(AB + 1), x, y - 1, z - 1), Grad3(P(BB + 1), x - 1, y - 1, z - 1), u), v),
                w);
}

float Source::Simplex(float xin, float yin) const {
    // Симплексный шум Перлина (2001): треугольная решётка, без направленных
    // «квадратных» артефактов и дешевле в старших размерностях.
    const float F2 = 0.36602540378f, G2 = 0.2113248654f;
    const float s = (xin + yin) * F2;
    const int i = FastFloor(xin + s), j = FastFloor(yin + s);
    const float t = (float)(i + j) * G2;
    const float x0 = xin - ((float)i - t), y0 = yin - ((float)j - t);
    const int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const float x1 = x0 - (float)i1 + G2, y1 = y0 - (float)j1 + G2;
    const float x2 = x0 - 1.0f + 2.0f * G2, y2 = y0 - 1.0f + 2.0f * G2;
    auto corner = [&](int gi, float x, float y) {
        float tt = 0.5f - x * x - y * y;
        if (tt < 0.0f) return 0.0f;
        tt *= tt;
        return tt * tt * Grad2(gi, x, y);
    };
    const float n = corner(P(i + P(j)), x0, y0) + corner(P(i + i1 + P(j + j1)), x1, y1) +
                    corner(P(i + 1 + P(j + 1)), x2, y2);
    return 70.0f * n;
}

float Source::Simplex(float x, float y, float z) const {
    const float F3 = 1.0f / 3.0f, G3 = 1.0f / 6.0f;
    const float s = (x + y + z) * F3;
    const int i = FastFloor(x + s), j = FastFloor(y + s), k = FastFloor(z + s);
    const float t = (float)(i + j + k) * G3;
    const float x0 = x - ((float)i - t), y0 = y - ((float)j - t), z0 = z - ((float)k - t);
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }
    auto corner = [&](int gi, float cx, float cy, float cz) {
        float tt = 0.6f - cx * cx - cy * cy - cz * cz;
        if (tt < 0.0f) return 0.0f;
        tt *= tt;
        return tt * tt * Grad3(gi, cx, cy, cz);
    };
    const float n =
        corner(P(i + P(j + P(k))), x0, y0, z0) +
        corner(P(i + i1 + P(j + j1 + P(k + k1))), x0 - i1 + G3, y0 - j1 + G3, z0 - k1 + G3) +
        corner(P(i + i2 + P(j + j2 + P(k + k2))), x0 - i2 + 2 * G3, y0 - j2 + 2 * G3, z0 - k2 + 2 * G3) +
        corner(P(i + 1 + P(j + 1 + P(k + 1))), x0 - 1 + 3 * G3, y0 - 1 + 3 * G3, z0 - 1 + 3 * G3);
    return 32.0f * n;
}

float Source::Value(float x, float y) const { return Value(x, y, 0.0f); }

float Source::Value(float x, float y, float z) const {
    const int X = FastFloor(x), Y = FastFloor(y), Z = FastFloor(z);
    const float u = Fade(x - (float)X), v = Fade(y - (float)Y), w = Fade(z - (float)Z);
    auto h = [&](int dx, int dy, int dz) { return Hash(X + dx, Y + dy, Z + dz) * 2.0f - 1.0f; };
    return Lerp(Lerp(Lerp(h(0, 0, 0), h(1, 0, 0), u), Lerp(h(0, 1, 0), h(1, 1, 0), u), v),
                Lerp(Lerp(h(0, 0, 1), h(1, 0, 1), u), Lerp(h(0, 1, 1), h(1, 1, 1), u), v), w);
}

void Source::Worley(float x, float y, float z, float& f1, float& f2, float& cell) const {
    const int X = FastFloor(x), Y = FastFloor(y), Z = FastFloor(z);
    f1 = f2 = 1e9f;
    cell = 0.0f;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = X + dx, cy = Y + dy, cz = Z + dz;
                // Точка ячейки — три независимых хэша (разные «соли» по оси).
                const glm::vec3 pt((float)cx + Hash(cx, cy, cz), (float)cy + Hash(cx + 17, cy, cz),
                                   (float)cz + Hash(cx, cy + 31, cz));
                const float d = glm::length(pt - glm::vec3(x, y, z));
                if (d < f1) {
                    f2 = f1;
                    f1 = d;
                    cell = Hash(cx + 101, cy + 59, cz + 7);
                } else if (d < f2) {
                    f2 = d;
                }
            }
}

float Source::Sample(Kind kind, float x, float y, float z) const {
    switch (kind) {
        case Kind::Perlin: return Perlin(x, y, z);
        case Kind::Simplex: return Simplex(x, y, z);
        case Kind::Value: return Value(x, y, z);
        case Kind::Worley:
        case Kind::WorleyF2:
        case Kind::Cellular: {
            float f1, f2, c;
            Worley(x, y, z, f1, f2, c);
            return kind == Kind::Worley ? f1 : kind == Kind::WorleyF2 ? f2 : c;
        }
    }
    return 0.0f;
}

namespace {
template <class Shape>
float Octaves(const Source& s, Kind kind, glm::vec3 p, const Fractal& f, Shape shape) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = f.Frequency;
    const int octaves = std::clamp(f.Octaves, 1, 16);
    for (int o = 0; o < octaves; ++o) {
        sum += amp * shape(s.Sample(kind, p.x * freq, p.y * freq, p.z * freq));
        norm += amp;
        amp *= f.Gain;
        freq *= f.Lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}
} // namespace

float Fbm(const Source& s, Kind kind, glm::vec3 p, const Fractal& f) {
    return Octaves(s, kind, p, f, [](float n) { return n; });
}

float Ridged(const Source& s, Kind kind, glm::vec3 p, const Fractal& f) {
    return Octaves(s, kind, p, f, [](float n) {
        const float r = 1.0f - std::fabs(n);
        return r * r;
    });
}

float Turbulence(const Source& s, Kind kind, glm::vec3 p, const Fractal& f) {
    return Octaves(s, kind, p, f, [](float n) { return std::fabs(n); });
}

glm::vec3 Warp(const Source& s, Kind kind, glm::vec3 p, float strength, const Fractal& f) {
    // Три сдвинутые копии одного поля — три независимые составляющие сдвига.
    const glm::vec3 d(Fbm(s, kind, p, f), Fbm(s, kind, p + glm::vec3(5.2f, 1.3f, 7.7f), f),
                      Fbm(s, kind, p + glm::vec3(1.7f, 9.2f, 3.4f), f));
    return p + d * strength;
}

} // namespace sage::noise
