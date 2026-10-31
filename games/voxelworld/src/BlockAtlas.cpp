#include "BlockAtlas.h"

#include <algorithm>
#include <cmath>

#include "Blocks.h"

namespace voxel {
namespace {

struct RGBA {
    float R, G, B, A;
};

uint32_t PixelHash(int x, int y, int tile) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)tile * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float Rnd(int x, int y, int tile, int salt = 0) {
    return (PixelHash(x + salt * 31, y - salt * 17, tile) & 0xFFFF) / 65535.0f;
}

RGBA Shade(RGBA c, float k) { return {c.R * k, c.G * k, c.B * k, c.A}; }
RGBA Mix(RGBA a, RGBA b, float t) {
    return {a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t, a.A + (b.A - a.A) * t};
}

// Пиксель (x, y) плитки tile; y = 0 — ВЕРХ плитки (так проще рисовать).
RGBA Paint(int tile, int x, int y) {
    const float n = Rnd(x, y, tile);
    const float n2 = Rnd(x, y, tile, 3);
    const RGBA soil{0.47f, 0.33f, 0.22f, 1}, grass{0.36f, 0.62f, 0.25f, 1};
    const RGBA stone{0.52f, 0.52f, 0.55f, 1}, sand{0.86f, 0.80f, 0.58f, 1};
    switch (tile) {
        case TileTurfTop: return Shade(grass, 0.82f + n * 0.3f);
        case TileTurfSide: {
            // Бахрома травы неровной глубины по верху земляного бока.
            const int fringe = 3 + (int)(Rnd(x, 0, tile, 5) * 3.0f);
            if (y < fringe) return Shade(grass, 0.8f + n * 0.25f);
            return Shade(soil, 0.8f + n * 0.3f);
        }
        case TileSoil: return Shade(soil, 0.78f + n * 0.32f + (n2 > 0.92f ? 0.2f : 0.0f));
        case TileGranite: {
            const float crack = (Rnd(x / 3, y / 2, tile, 9) > 0.82f) ? 0.78f : 1.0f;
            return Shade(stone, (0.82f + n * 0.22f) * crack);
        }
        case TileDune: return Shade(sand, 0.9f + n * 0.14f);
        case TileWater: {
            const float wave = 0.5f + 0.5f * std::sin((x + y * 0.5f) * 0.8f);
            return {0.16f + wave * 0.05f, 0.38f + wave * 0.08f, 0.72f + n * 0.08f, 0.68f};
        }
        case TileTrunkSide: {
            const float stripe = (x % 4 == 0) ? 0.72f : 1.0f;
            return Shade({0.43f, 0.31f, 0.19f, 1}, (0.8f + n * 0.25f) * stripe);
        }
        case TileTrunkTop: {
            const float dx = x - 7.5f, dy = y - 7.5f;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r > 7.0f) return Shade({0.43f, 0.31f, 0.19f, 1}, 0.8f + n * 0.2f);
            const float ring = 0.5f + 0.5f * std::cos(r * 1.9f);
            return Shade({0.72f, 0.57f, 0.36f, 1}, 0.8f + ring * 0.2f + n * 0.08f);
        }
        case TileFoliage: {
            if (n < 0.18f) return {0, 0, 0, 0}; // просветы между листьями
            return Shade({0.24f, 0.50f, 0.20f, 1}, 0.7f + n2 * 0.45f);
        }
        case TileBoards: {
            const bool seam = (y % 4 == 3) || ((x + (y / 4) * 5) % 16 == 0);
            return Shade({0.70f, 0.53f, 0.32f, 1}, (seam ? 0.62f : 0.88f) + n * 0.14f);
        }
        case TileLumen: {
            const float glint = n > 0.85f ? 1.25f : 1.0f;
            return Shade({1.0f, 0.86f, 0.45f, 1}, (0.78f + n2 * 0.22f) * glint);
        }
        case TileFrost: return Shade({0.93f, 0.95f, 0.98f, 1}, 0.9f + n * 0.1f);
        case TileFrostSide: {
            const int fringe = 4 + (int)(Rnd(x, 0, tile, 2) * 2.0f);
            if (y < fringe) return Shade({0.93f, 0.95f, 0.98f, 1}, 0.9f + n * 0.1f);
            return Shade(soil, 0.8f + n * 0.3f);
        }
        case TileClay: {
            const int row = y / 4;
            const bool mortar = (y % 4 == 3) || ((x + (row % 2) * 4) % 8 == 7);
            if (mortar) return Shade({0.75f, 0.72f, 0.66f, 1}, 0.9f + n * 0.1f);
            return Shade({0.66f, 0.30f, 0.24f, 1}, 0.82f + n * 0.25f);
        }
        case TileCrystal: {
            const bool frame = x == 0 || y == 0 || x == 15 || y == 15;
            const bool glare = (x - y == 3 || x - y == 4) && x > 4 && x < 12;
            if (frame) return {0.78f, 0.88f, 0.92f, 1};
            if (glare) return {1, 1, 1, 0.55f};
            return {0.75f, 0.88f, 0.95f, 0.12f};
        }
        case TileBasalt: return Shade({0.24f, 0.23f, 0.27f, 1}, 0.8f + n * 0.35f);
        case TileMoss: {
            const RGBA base = Shade(stone, 0.8f + n * 0.2f);
            return Rnd(x / 2, y / 2, tile, 4) > 0.55f ? Mix(base, Shade(grass, 0.7f + n2 * 0.2f), 0.7f) : base;
        }
        case TileEmber: {
            const bool vein = Rnd(x / 2, y / 2, tile, 7) > 0.7f;
            return vein ? RGBA{1.0f, 0.45f + n * 0.2f, 0.12f, 1} : Shade({0.30f, 0.12f, 0.08f, 1}, 0.8f + n * 0.4f);
        }
        default: return {1, 0, 1, 1};
    }
}

uint8_t ToByte(float v) { return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); }

} // namespace

std::vector<uint8_t> BuildAtlasPixels(int& width, int& height) {
    width = height = kAtlasGrid * kTilePixels;
    std::vector<uint8_t> px((size_t)width * height * 4, 0);
    for (int tile = 0; tile < TileCount; ++tile) {
        const int tx = tile % kAtlasGrid, ty = tile / kAtlasGrid;
        for (int y = 0; y < kTilePixels; ++y)
            for (int x = 0; x < kTilePixels; ++x) {
                const RGBA c = Paint(tile, x, y);
                // Плитка ty считается сверху; в текстуре строки снизу вверх.
                const int row = height - 1 - (ty * kTilePixels + y);
                const int col = tx * kTilePixels + x;
                uint8_t* p = &px[((size_t)row * width + col) * 4];
                p[0] = ToByte(c.R);
                p[1] = ToByte(c.G);
                p[2] = ToByte(c.B);
                p[3] = ToByte(c.A);
            }
    }
    return px;
}

void TileAverageColor(const std::vector<uint8_t>& pixels, int width, int tile, float out[3]) {
    const int height = (int)(pixels.size() / 4 / (size_t)width);
    const int tx = tile % kAtlasGrid, ty = tile / kAtlasGrid;
    float sum[3] = {0, 0, 0};
    int count = 0;
    for (int y = 0; y < kTilePixels; ++y)
        for (int x = 0; x < kTilePixels; ++x) {
            const int row = height - 1 - (ty * kTilePixels + y);
            const uint8_t* p = &pixels[((size_t)row * width + tx * kTilePixels + x) * 4];
            if (p[3] < 128) continue;
            for (int i = 0; i < 3; ++i) sum[i] += p[i] / 255.0f;
            ++count;
        }
    for (int i = 0; i < 3; ++i) out[i] = count ? sum[i] / count : 0.0f;
}

} // namespace voxel
