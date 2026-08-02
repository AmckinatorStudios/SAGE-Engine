#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sage/assets/format/Blob.h"

// ---------------------------------------------------------------------------
// .sagetex — свой формат текстур.
//
// ЧЕМ ПЛОХИ PNG/JPG В ИГРЕ. Они форматы ХРАНЕНИЯ КАРТИНКИ, а не текстуры. PNG
// разжимается в полный RGBA-буфер, который целиком уезжает в видеопамять: 2048x2048
// — это 16 МБ на одну карту, и таких карт у материала пять. JPG вдобавок теряет
// качество там, где оно нужно (резкие края нормал-карты), и не умеет альфу.
// Декодирование обоих — это работа на каждый запуск, причём заметная: сотня
// текстур превращается в секунды загрузки.
//
// ЧТО ДЕЛАЕТ ЭТОТ. Хранит текстуру так, как её принимает видеокарта:
//
//   • блочное сжатие BC1/BC3 — 4 бита на пиксель вместо 32 (в 8 раз меньше
//     видеопамяти) и распаковка прямо в железе, то есть бесплатная. Это то, чем
//     отличается «текстура» от «картинки»: PNG нужно разжать целиком, BC —
//     нет;
//   • мип-уровни ПОСЧИТАНЫ ЗАРАНЕЕ. Их всё равно строят при загрузке; делать это
//     на каждом запуске у каждого игрока — чистая потеря времени;
//   • поверх — deflate контейнера, потому что блоки BC жмутся ещё процентов на
//     двадцать, а разжимаются один раз.
//
// ПРО ПОТЕРИ. BC1/BC3 — сжатие с потерями, как и любое блочное. Для albedo и
// пиксель-арта это незаметно; для карт нормалей и данных (roughness, маски) —
// нет, и там формат хранит несжатые пиксели: правильнее занять место, чем
// испортить карту, по которой считается свет. Выбор делается автоматически по
// назначению текстуры и переопределяется явно.
// ---------------------------------------------------------------------------

namespace sage::assets {

constexpr uint32_t kAssetTypeTexture = FourCC("TEXR");
constexpr uint32_t kTextureSchemaVersion = 1;

enum class TexturePixelFormat : uint32_t {
    RGBA8 = 0,   // без потерь, 32 бита на пиксель
    BC1 = 1,     // RGB, 4 бита на пиксель (без альфы)
    BC3 = 2,     // RGBA, 8 бит на пиксель
};

// Назначение — не украшение, а то, по чему выбирается кодирование. Ошибка здесь
// видна на экране: пожатая нормал-карта даёт «мыло» на рельефе.
enum class TextureIntent : uint32_t {
    Albedo = 0,      // цвет — можно жать
    NormalMap = 1,   // рельеф — НЕ жать блоками
    DataMap = 2,     // roughness/metallic/AO — НЕ жать
    PixelArt = 3,    // резкие края, без мипов и без блочного сжатия
};

struct TextureWriteOptions {
    TextureIntent Intent = TextureIntent::Albedo;
    bool GenerateMips = true;
    // Явно переопределить выбор формата. Пусто — решает Intent.
    bool ForceFormat = false;
    TexturePixelFormat Format = TexturePixelFormat::RGBA8;
};

struct TextureData {
    int Width = 0;
    int Height = 0;
    TexturePixelFormat Format = TexturePixelFormat::RGBA8;
    // Уровень 0 — полный размер, дальше вдвое меньше. Байты — уже в Format.
    std::vector<std::vector<uint8_t>> Levels;

    bool Empty() const { return Width <= 0 || Height <= 0 || Levels.empty(); }
};

struct TextureStats {
    size_t SourceBytes = 0;
    size_t PackedBytes = 0;
    int MipLevels = 0;
};

// Упаковать RGBA8-пиксели (row-major, 4 байта на пиксель, первая строка сверху).
Blob PackTexture(const uint8_t* rgba, int width, int height, const TextureWriteOptions& opts = {},
                 TextureStats* stats = nullptr);

bool UnpackTexture(const Blob& blob, TextureData& out, std::string& err);

bool WriteTextureFile(const std::string& path, const uint8_t* rgba, int width, int height,
                      const TextureWriteOptions& opts, std::string& err,
                      TextureStats* stats = nullptr);
bool ReadTextureFile(const std::string& path, TextureData& out, std::string& err);

// Разжать блочный уровень обратно в RGBA8 — для превью в редакторе и для
// проверок: без обратного преобразования качество кодировщика измерить нечем.
bool DecodeToRGBA(const TextureData& tex, int level, std::vector<uint8_t>& outRGBA, int& outW,
                  int& outH);

} // namespace sage::assets
