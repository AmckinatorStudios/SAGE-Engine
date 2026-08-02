#include "sage/assets/format/TextureFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sage::assets {

namespace {

constexpr uint32_t kChunkInfo = FourCC("TINF");
constexpr uint32_t kChunkMip0 = FourCC("MIP0");   // дальше MIP1, MIP2, ...

uint32_t MipChunkId(int level) {
    // 'MIP0' + level: имена остаются читаемыми в hex-редакторе до 16 уровней,
    // а больше и не бывает — это текстура 65536x65536.
    return kChunkMip0 + (uint32_t)(level << 24);
}

// --- BC1/BC3 ----------------------------------------------------------------
//
// Кодировщик намеренно простой: для каждого блока 4x4 берутся крайние цвета по
// главной оси разброса, и остальные пиксели раскладываются по четырём точкам
// между ними. Это не самый качественный из известных методов (их развитие —
// отдельная дисциплина), но он предсказуем, быстр и не требует зависимостей, а
// разница с «идеальным» кодировщиком заметна на единицах процентов пикселей.

struct Rgba { uint8_t R, G, B, A; };

uint16_t Pack565(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
void Unpack565(uint16_t c, int& r, int& g, int& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5) & 0x3F) * 255 / 63;
    b = (c & 0x1F) * 255 / 31;
}

// Крайние точки блока по наибольшему разбросу: берём минимум и максимум по
// каждому каналу. Для 4x4 этого достаточно — блок редко содержит больше двух
// заметно разных цветов.
void BlockEndpoints(const Rgba block[16], uint16_t& outMax, uint16_t& outMin) {
    int rmin = 255, gmin = 255, bmin = 255, rmax = 0, gmax = 0, bmax = 0;
    for (int i = 0; i < 16; ++i) {
        rmin = std::min(rmin, (int)block[i].R);
        gmin = std::min(gmin, (int)block[i].G);
        bmin = std::min(bmin, (int)block[i].B);
        rmax = std::max(rmax, (int)block[i].R);
        gmax = std::max(gmax, (int)block[i].G);
        bmax = std::max(bmax, (int)block[i].B);
    }
    outMax = Pack565(rmax, gmax, bmax);
    outMin = Pack565(rmin, gmin, bmin);
}

void EncodeColorBlock(const Rgba block[16], ByteWriter& w) {
    uint16_t c0, c1;
    BlockEndpoints(block, c0, c1);
    // BC1 требует c0 > c1 для непрозрачного режима с четырьмя цветами: при
    // c0 <= c1 аппаратура читает блок как трёхцветный с прозрачностью.
    if (c0 < c1) std::swap(c0, c1);
    if (c0 == c1) {
        // Одноцветный блок: индексы нулевые, оба конца совпадают.
        w.U16(c0);
        w.U16(c1);
        w.U32(0);
        return;
    }
    int r0, g0, b0, r1, g1, b1;
    Unpack565(c0, r0, g0, b0);
    Unpack565(c1, r1, g1, b1);
    const int pr[4] = {r0, r1, (2 * r0 + r1) / 3, (r0 + 2 * r1) / 3};
    const int pg[4] = {g0, g1, (2 * g0 + g1) / 3, (g0 + 2 * g1) / 3};
    const int pb[4] = {b0, b1, (2 * b0 + b1) / 3, (b0 + 2 * b1) / 3};

    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestD = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            const int dr = (int)block[i].R - pr[k];
            const int dg = (int)block[i].G - pg[k];
            const int db = (int)block[i].B - pb[k];
            const int d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = k; }
        }
        bits |= (uint32_t)best << (i * 2);
    }
    w.U16(c0);
    w.U16(c1);
    w.U32(bits);
}

void EncodeAlphaBlock(const Rgba block[16], ByteWriter& w) {
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) {
        amin = std::min(amin, (int)block[i].A);
        amax = std::max(amax, (int)block[i].A);
    }
    w.U8((uint8_t)amax);
    w.U8((uint8_t)amin);
    // Восемь уровней между концами, по три бита на пиксель — 48 бит подряд.
    uint64_t bits = 0;
    const int span = amax - amin;
    for (int i = 0; i < 16; ++i) {
        int idx = 0;
        if (span > 0) {
            const float t = (float)(block[i].A - amin) / (float)span;
            const int step = (int)std::lround(t * 7.0f);
            // Раскладка индексов BC3: 0 -> amax, 1 -> amin, 2..7 — между.
            if (step == 7) idx = 0;
            else if (step == 0) idx = 1;
            else idx = 8 - step;
        }
        bits |= (uint64_t)(idx & 7) << (i * 3);
    }
    for (int i = 0; i < 6; ++i) w.U8((uint8_t)((bits >> (i * 8)) & 0xFF));
}

void DecodeColorBlock(const uint8_t* src, Rgba out[16], bool opaque) {
    const uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
    const uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));
    uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) bits |= (uint32_t)src[4 + i] << (i * 8);
    int r0, g0, b0, r1, g1, b1;
    Unpack565(c0, r0, g0, b0);
    Unpack565(c1, r1, g1, b1);
    int pr[4], pg[4], pb[4];
    pr[0] = r0; pg[0] = g0; pb[0] = b0;
    pr[1] = r1; pg[1] = g1; pb[1] = b1;
    if (c0 > c1 || opaque) {
        pr[2] = (2 * r0 + r1) / 3; pg[2] = (2 * g0 + g1) / 3; pb[2] = (2 * b0 + b1) / 3;
        pr[3] = (r0 + 2 * r1) / 3; pg[3] = (g0 + 2 * g1) / 3; pb[3] = (b0 + 2 * b1) / 3;
    } else {
        pr[2] = (r0 + r1) / 2; pg[2] = (g0 + g1) / 2; pb[2] = (b0 + b1) / 2;
        pr[3] = 0; pg[3] = 0; pb[3] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        const int k = (bits >> (i * 2)) & 3;
        out[i].R = (uint8_t)pr[k];
        out[i].G = (uint8_t)pg[k];
        out[i].B = (uint8_t)pb[k];
        out[i].A = 255;
    }
}

void DecodeAlphaBlock(const uint8_t* src, Rgba out[16]) {
    const int a0 = src[0], a1 = src[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)src[2 + i] << (i * 8);
    int lut[8];
    lut[0] = a0;
    lut[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i) lut[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
    } else {
        for (int i = 2; i < 6; ++i) lut[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
        lut[6] = 0;
        lut[7] = 255;
    }
    for (int i = 0; i < 16; ++i) out[i].A = (uint8_t)lut[(bits >> (i * 3)) & 7];
}

// Собрать блок 4x4, дополняя края повтором последнего пикселя: текстура не
// обязана быть кратна четырём, а класть в блок нули значило бы затемнить край.
void GatherBlock(const uint8_t* rgba, int w, int h, int bx, int by, Rgba out[16]) {
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int sx = std::min(bx + x, w - 1);
            const int sy = std::min(by + y, h - 1);
            const uint8_t* p = rgba + ((size_t)sy * w + sx) * 4;
            out[y * 4 + x] = {p[0], p[1], p[2], p[3]};
        }
    }
}

std::vector<uint8_t> EncodeLevel(const uint8_t* rgba, int w, int h, TexturePixelFormat fmt) {
    if (fmt == TexturePixelFormat::RGBA8) {
        return std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4);
    }
    ByteWriter out;
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            Rgba block[16];
            GatherBlock(rgba, w, h, bx, by, block);
            if (fmt == TexturePixelFormat::BC3) EncodeAlphaBlock(block, out);
            EncodeColorBlock(block, out);
        }
    }
    return std::move(out.Bytes);
}

// Понижение вдвое усреднением четырёх пикселей. Усреднять надо в ЛИНЕЙНОМ
// пространстве, но текстуры albedo лежат в sRGB — для мипов разница мала и
// заметна лишь на резких контрастах, поэтому здесь простое среднее, а точную
// гамма-коррекцию делает пайплайн освещения.
std::vector<uint8_t> Downsample(const std::vector<uint8_t>& src, int w, int h, int& outW,
                                int& outH) {
    outW = std::max(1, w / 2);
    outH = std::max(1, h / 2);
    std::vector<uint8_t> dst((size_t)outW * outH * 4);
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            int acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, w - 1);
                    const int sy = std::min(y * 2 + dy, h - 1);
                    const uint8_t* p = src.data() + ((size_t)sy * w + sx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            }
            uint8_t* d = dst.data() + ((size_t)y * outW + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = (uint8_t)(acc[c] / n);
        }
    }
    return dst;
}

TexturePixelFormat ChooseFormat(const TextureWriteOptions& opts, bool hasAlpha) {
    if (opts.ForceFormat) return opts.Format;
    switch (opts.Intent) {
        case TextureIntent::NormalMap:
        case TextureIntent::DataMap:
        case TextureIntent::PixelArt:
            // Блочное сжатие здесь портит именно то, ради чего карту и делали:
            // рельеф, точные значения, резкие края пиксель-арта.
            return TexturePixelFormat::RGBA8;
        case TextureIntent::Albedo:
        default:
            return hasAlpha ? TexturePixelFormat::BC3 : TexturePixelFormat::BC1;
    }
}

size_t LevelBytes(int w, int h, TexturePixelFormat fmt) {
    if (fmt == TexturePixelFormat::RGBA8) return (size_t)w * h * 4;
    const size_t blocks = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4);
    return blocks * (fmt == TexturePixelFormat::BC3 ? 16 : 8);
}

} // namespace

Blob PackTexture(const uint8_t* rgba, int width, int height, const TextureWriteOptions& opts,
                 TextureStats* stats) {
    Blob blob(kAssetTypeTexture, kTextureSchemaVersion);
    if (!rgba || width <= 0 || height <= 0) return blob;

    bool hasAlpha = false;
    for (size_t i = 3; i < (size_t)width * height * 4 && !hasAlpha; i += 4)
        hasAlpha = rgba[i] != 255;
    const TexturePixelFormat fmt = ChooseFormat(opts, hasAlpha);
    // Пиксель-арт без мипов намеренно: мипы на листе спрайтов подмешивают
    // соседний спрайт по краям, и картинка «течёт» на расстоянии.
    const bool mips = opts.GenerateMips && opts.Intent != TextureIntent::PixelArt;

    std::vector<uint8_t> level(rgba, rgba + (size_t)width * height * 4);
    int w = width, h = height;
    int levelCount = 0;
    for (;;) {
        blob.Add(MipChunkId(levelCount), EncodeLevel(level.data(), w, h, fmt));
        ++levelCount;
        if (!mips || (w == 1 && h == 1) || levelCount >= 16) break;
        int nw, nh;
        level = Downsample(level, w, h, nw, nh);
        w = nw;
        h = nh;
    }

    {
        ByteWriter info;
        info.U32((uint32_t)width);
        info.U32((uint32_t)height);
        info.U32((uint32_t)fmt);
        info.U32((uint32_t)levelCount);
        info.U32((uint32_t)opts.Intent);
        blob.Add(kChunkInfo, std::move(info.Bytes), Codec::Raw);
    }

    if (stats) {
        stats->SourceBytes = (size_t)width * height * 4;
        stats->PackedBytes = blob.Serialize().size();
        stats->MipLevels = levelCount;
    }
    return blob;
}

bool UnpackTexture(const Blob& blob, TextureData& out, std::string& err) {
    if (blob.AssetType() != kAssetTypeTexture) {
        err = "это не текстура (тип " + FourCCToString(blob.AssetType()) + ")";
        return false;
    }
    if (blob.AssetVersion() > kTextureSchemaVersion) {
        err = "схема текстуры новее, чем понимает движок";
        return false;
    }
    const std::vector<uint8_t>* info = blob.Find(kChunkInfo);
    if (!info) { err = "нет куска TINF"; return false; }
    ByteReader r(*info);
    out.Width = (int)r.U32();
    out.Height = (int)r.U32();
    out.Format = (TexturePixelFormat)r.U32();
    const int levels = (int)r.U32();
    r.U32();   // intent — метаданные, на чтение не влияет
    if (!r.Ok() || out.Width <= 0 || out.Height <= 0 || levels <= 0 || levels > 16) {
        err = "кусок TINF повреждён";
        return false;
    }
    if (out.Format != TexturePixelFormat::RGBA8 && out.Format != TexturePixelFormat::BC1 &&
        out.Format != TexturePixelFormat::BC3) {
        err = "неизвестный формат пикселей";
        return false;
    }

    out.Levels.clear();
    int w = out.Width, h = out.Height;
    for (int i = 0; i < levels; ++i) {
        const std::vector<uint8_t>* chunk = blob.Find(MipChunkId(i));
        if (!chunk) { err = "нет мип-уровня " + std::to_string(i); return false; }
        // Размер уровня вычисляется, а не берётся на веру: он должен сойтись с
        // объявленными шириной/высотой, иначе распаковка поедет по памяти.
        if (chunk->size() != LevelBytes(w, h, out.Format)) {
            err = "мип-уровень " + std::to_string(i) + " не того размера";
            return false;
        }
        out.Levels.push_back(*chunk);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return true;
}

bool DecodeToRGBA(const TextureData& tex, int level, std::vector<uint8_t>& outRGBA, int& outW,
                  int& outH) {
    if (level < 0 || level >= (int)tex.Levels.size()) return false;
    outW = std::max(1, tex.Width >> level);
    outH = std::max(1, tex.Height >> level);
    const std::vector<uint8_t>& src = tex.Levels[(size_t)level];

    if (tex.Format == TexturePixelFormat::RGBA8) {
        outRGBA = src;
        return outRGBA.size() == (size_t)outW * outH * 4;
    }

    outRGBA.assign((size_t)outW * outH * 4, 0);
    const int blockBytes = tex.Format == TexturePixelFormat::BC3 ? 16 : 8;
    size_t offset = 0;
    for (int by = 0; by < outH; by += 4) {
        for (int bx = 0; bx < outW; bx += 4) {
            if (offset + (size_t)blockBytes > src.size()) return false;
            Rgba block[16];
            if (tex.Format == TexturePixelFormat::BC3) {
                DecodeAlphaBlock(src.data() + offset, block);
                DecodeColorBlock(src.data() + offset + 8, block, /*opaque=*/true);
                // Цветовой декодер ставит альфу 255 — возвращаем свою.
                Rgba alphaOnly[16];
                DecodeAlphaBlock(src.data() + offset, alphaOnly);
                for (int i = 0; i < 16; ++i) block[i].A = alphaOnly[i].A;
            } else {
                DecodeColorBlock(src.data() + offset, block, /*opaque=*/false);
            }
            offset += (size_t)blockBytes;
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int px = bx + x, py = by + y;
                    if (px >= outW || py >= outH) continue;
                    uint8_t* d = outRGBA.data() + ((size_t)py * outW + px) * 4;
                    const Rgba& s = block[y * 4 + x];
                    d[0] = s.R; d[1] = s.G; d[2] = s.B; d[3] = s.A;
                }
            }
        }
    }
    return true;
}

bool WriteTextureFile(const std::string& path, const uint8_t* rgba, int width, int height,
                      const TextureWriteOptions& opts, std::string& err, TextureStats* stats) {
    const Blob blob = PackTexture(rgba, width, height, opts, stats);
    if (blob.Chunks().empty()) {
        err = "нечего писать: пустая картинка";
        return false;
    }
    return blob.Write(path, err);
}

bool ReadTextureFile(const std::string& path, TextureData& out, std::string& err) {
    Blob blob;
    if (!Blob::Read(path, blob, err)) return false;
    return UnpackTexture(blob, out, err);
}

} // namespace sage::assets
