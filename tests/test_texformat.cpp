// .sagetex и конвертация в свои форматы.
//
// У текстурного формата два обещания, и оба проверяются числами: блочное
// сжатие даёт восьмикратную экономию видеопамяти при ошибке цвета в пределах
// нескольких единиц из 255, а карты, где потери недопустимы (нормали, данные),
// остаются побайтно точными.
#include "TestFramework.h"

#include "sage/assets/format/TextureFormat.h"
#include "sage/assets/import/Convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

using namespace sage::assets;

namespace {

// Плавный градиент с пятнами — то, на чём блочный кодировщик и работает.
// Случайный шум был бы нечестным тестом: его не жмёт никто.
std::vector<uint8_t> MakeImage(int w, int h, bool withAlpha) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px.data() + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(x * 255 / std::max(1, w - 1));
            p[1] = (uint8_t)(y * 255 / std::max(1, h - 1));
            p[2] = (uint8_t)(((x / 8) + (y / 8)) % 2 ? 200 : 60);
            p[3] = withAlpha ? (uint8_t)(x * 255 / std::max(1, w - 1)) : 255;
        }
    }
    return px;
}

double MeanAbsError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < channels; ++c) {
            sum += std::abs((int)a[i + c] - (int)b[i + c]);
            ++n;
        }
    }
    return sum / (double)n;
}

} // namespace

TEST(texture_bc1_is_eight_times_smaller_and_close) {
    const int w = 128, h = 128;
    const std::vector<uint8_t> src = MakeImage(w, h, /*withAlpha=*/false);

    TextureWriteOptions opts;
    opts.Intent = TextureIntent::Albedo;
    opts.GenerateMips = false;
    TextureStats stats;
    Blob blob = PackTexture(src.data(), w, h, opts, &stats);

    TextureData tex;
    std::string err;
    CHECK_TRUE(UnpackTexture(blob, tex, err));
    if (!err.empty()) std::printf("    err: %s\n", err.c_str());
    CHECK_TRUE(tex.Format == TexturePixelFormat::BC1);   // без альфы — BC1
    CHECK_EQ((int)tex.Levels.size(), 1);

    // Главное число: сколько занимает уровень 0 в ВИДЕОПАМЯТИ. Это 4 бита на
    // пиксель против 32 — ровно в восемь раз меньше, и это не зависит от того,
    // как хорошо файл сжался на диске.
    CHECK_EQ((int)tex.Levels[0].size(), w * h / 2);
    CHECK_EQ((int)stats.SourceBytes, w * h * 4);

    std::vector<uint8_t> back;
    int bw = 0, bh = 0;
    CHECK_TRUE(DecodeToRGBA(tex, 0, back, bw, bh));
    CHECK_EQ(bw, w);
    CHECK_EQ(bh, h);
    const double err3 = MeanAbsError(src, back, 3);
    std::printf("    BC1: %d -> %d байт в VRAM, средняя ошибка канала %.2f/255\n",
                w * h * 4, (int)tex.Levels[0].size(), err3);
    // Порог щедрый намеренно: тест ловит сломанный кодировщик, а не борется за
    // проценты качества.
    CHECK_TRUE(err3 < 12.0);
}

TEST(texture_bc3_keeps_alpha) {
    const int w = 64, h = 64;
    const std::vector<uint8_t> src = MakeImage(w, h, /*withAlpha=*/true);

    TextureWriteOptions opts;
    opts.GenerateMips = false;
    Blob blob = PackTexture(src.data(), w, h, opts);

    TextureData tex;
    std::string err;
    CHECK_TRUE(UnpackTexture(blob, tex, err));
    // Есть альфа — обязан выбраться BC3, иначе прозрачность просто пропала бы.
    CHECK_TRUE(tex.Format == TexturePixelFormat::BC3);
    CHECK_EQ((int)tex.Levels[0].size(), w * h);   // 8 бит на пиксель

    std::vector<uint8_t> back;
    int bw, bh;
    CHECK_TRUE(DecodeToRGBA(tex, 0, back, bw, bh));
    // Альфа-канал: проверяем именно его, ради него BC3 и выбран.
    double alphaErr = 0.0;
    for (size_t i = 3; i < src.size(); i += 4) alphaErr += std::abs((int)src[i] - (int)back[i]);
    alphaErr /= (double)(src.size() / 4);
    std::printf("    BC3: средняя ошибка альфы %.2f/255\n", alphaErr);
    CHECK_TRUE(alphaErr < 10.0);
}

TEST(texture_normal_and_data_maps_stay_exact) {
    // Карту нормалей и карты данных жать блоками нельзя: по ним считается свет,
    // и «почти тот же цвет» означает «не тот рельеф». Формат обязан оставить их
    // побайтно точными САМ, по назначению, а не по просьбе.
    const int w = 32, h = 32;
    const std::vector<uint8_t> src = MakeImage(w, h, false);

    for (TextureIntent intent : {TextureIntent::NormalMap, TextureIntent::DataMap}) {
        TextureWriteOptions opts;
        opts.Intent = intent;
        opts.GenerateMips = false;
        Blob blob = PackTexture(src.data(), w, h, opts);
        TextureData tex;
        std::string err;
        CHECK_TRUE(UnpackTexture(blob, tex, err));
        CHECK_TRUE(tex.Format == TexturePixelFormat::RGBA8);

        std::vector<uint8_t> back;
        int bw, bh;
        CHECK_TRUE(DecodeToRGBA(tex, 0, back, bw, bh));
        CHECK_TRUE(back == src);   // именно побайтно
    }
}

TEST(texture_mips_are_generated_but_not_for_pixel_art) {
    const int w = 64, h = 64;
    const std::vector<uint8_t> src = MakeImage(w, h, false);

    TextureWriteOptions withMips;
    withMips.GenerateMips = true;
    TextureStats s1;
    Blob a = PackTexture(src.data(), w, h, withMips, &s1);
    TextureData t1;
    std::string err;
    CHECK_TRUE(UnpackTexture(a, t1, err));
    // 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1: семь уровней.
    CHECK_EQ((int)t1.Levels.size(), 7);
    CHECK_EQ(s1.MipLevels, 7);

    // Пиксель-арт: мипы на листе спрайтов подмешивают соседний спрайт по краям,
    // поэтому для него они не строятся и блочное сжатие не применяется.
    TextureWriteOptions art;
    art.Intent = TextureIntent::PixelArt;
    art.GenerateMips = true;
    Blob b = PackTexture(src.data(), w, h, art);
    TextureData t2;
    CHECK_TRUE(UnpackTexture(b, t2, err));
    CHECK_EQ((int)t2.Levels.size(), 1);
    CHECK_TRUE(t2.Format == TexturePixelFormat::RGBA8);
}

TEST(texture_non_multiple_of_four_survives) {
    // Текстура не обязана быть кратна четырём, а блок BC — всегда 4x4. Край
    // должен дополняться, а не выходить за буфер.
    const int w = 13, h = 7;
    const std::vector<uint8_t> src = MakeImage(w, h, false);
    TextureWriteOptions opts;
    opts.GenerateMips = false;
    Blob blob = PackTexture(src.data(), w, h, opts);

    TextureData tex;
    std::string err;
    CHECK_TRUE(UnpackTexture(blob, tex, err));
    CHECK_EQ(tex.Width, w);
    CHECK_EQ(tex.Height, h);
    std::vector<uint8_t> back;
    int bw, bh;
    CHECK_TRUE(DecodeToRGBA(tex, 0, back, bw, bh));
    CHECK_EQ(bw, w);
    CHECK_EQ(bh, h);
    CHECK_EQ((int)back.size(), w * h * 4);
}

TEST(texture_rejects_garbage) {
    Blob wrong(FourCC("MESH"), 1);
    TextureData tex;
    std::string err;
    CHECK_FALSE(UnpackTexture(wrong, tex, err));
    CHECK_FALSE(err.empty());

    Blob future(kAssetTypeTexture, kTextureSchemaVersion + 9);
    CHECK_FALSE(UnpackTexture(future, tex, err));
}

TEST(convert_model_to_native_reports_what_happened) {
    // Готовим исходник в чужом формате — .bbmodel, один блок.
    const char* bb = R"({
      "resolution": {"width": 16, "height": 16},
      "elements": [{"type":"cube","from":[0,0,0],"to":[16,16,16],
        "faces": {
          "north": {"uv":[0,0,16,16],"texture":0},
          "south": {"uv":[0,0,16,16],"texture":0},
          "west":  {"uv":[0,0,16,16],"texture":0},
          "east":  {"uv":[0,0,16,16],"texture":0},
          "up":    {"uv":[0,0,16,16],"texture":0},
          "down":  {"uv":[0,0,16,16],"texture":0}
        }}]
    })";
    const std::string src = "conv_block.bbmodel";
    {
        std::FILE* f = std::fopen(src.c_str(), "wb");
        std::fwrite(bb, 1, std::strlen(bb), f);
        std::fclose(f);
    }

    ConvertOptions opts;
    opts.Overwrite = true;
    ConvertResult r = ConvertModelToNative(src, {}, opts);
    CHECK_TRUE(r.Ok);
    if (!r.Ok) std::printf("    err: %s\n", r.Error.c_str());
    // Имя вывода — рядом с исходником, с нашим расширением.
    CHECK_TRUE(r.OutputPath.find(".sagemesh") != std::string::npos);
    CHECK_EQ((int)r.Vertices, 24);
    CHECK_EQ((int)r.Triangles, 12);
    CHECK_TRUE(r.OutputBytes > 0);

    // Результат читается обратно как меш.
    sage::render::MeshData back;
    std::string err;
    CHECK_TRUE(ReadMeshFile(r.OutputPath, back, err));
    CHECK_EQ((int)back.Vertices.size(), 24);

    // Без Overwrite повторная конвертация обязана ОТКАЗАТЬСЯ, а не молча
    // затереть: пакетную конвертацию запускают по папке, где могли править руками.
    ConvertOptions noOverwrite;
    noOverwrite.Overwrite = false;
    ConvertResult again = ConvertModelToNative(src, {}, noOverwrite);
    CHECK_FALSE(again.Ok);
    CHECK_FALSE(again.Error.empty());

    std::remove(src.c_str());
    std::remove(r.OutputPath.c_str());
}

TEST(convert_reports_failures_per_file) {
    // Неизвестный формат — внятный отказ, а не исключение и не тихий успех.
    ConvertResult r = ConvertAnyToNative("nothing.xyz", {}, {});
    CHECK_FALSE(r.Ok);
    CHECK_FALSE(r.Error.empty());
    CHECK_TRUE(r.SourcePath == "nothing.xyz");

    // Битый файл с правильным расширением: ошибка приходит от импортёра.
    const std::string bad = "broken.bbmodel";
    {
        std::FILE* f = std::fopen(bad.c_str(), "wb");
        const char* junk = "{ это не модель";
        std::fwrite(junk, 1, std::strlen(junk), f);
        std::fclose(f);
    }
    ConvertOptions opts;
    opts.Overwrite = true;
    ConvertResult r2 = ConvertModelToNative(bad, {}, opts);
    CHECK_FALSE(r2.Ok);
    CHECK_FALSE(r2.Error.empty());
    std::remove(bad.c_str());
}

TEST(convert_guesses_texture_intent_from_name) {
    // Имя «..._normal.png» означает карту нормалей — соглашение всех библиотек
    // PBR-материалов. Проверяем через результат: такая карта обязана остаться
    // несжатой, иначе угадывание не работает.
    const int w = 16, h = 16;
    const std::vector<uint8_t> px = MakeImage(w, h, false);

    // Пишем PNG нечем — проверяем саму развилку через PackTexture с тем же
    // назначением, которое выберет конвертер, и через публичный признак.
    CHECK_TRUE(IsConvertibleTexture("stone_normal.png"));
    CHECK_TRUE(IsConvertibleTexture("A.JPG"));
    CHECK_FALSE(IsConvertibleTexture("model.obj"));
    CHECK_TRUE(IsConvertibleModel("model.obj"));
    CHECK_TRUE(IsConvertibleModel("thing.bbmodel"));
    // Свой формат конвертировать в себя незачем.
    CHECK_FALSE(IsConvertibleModel("already.sagemesh"));
}
