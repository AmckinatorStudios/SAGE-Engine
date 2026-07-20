// Модульные тесты менеджера памяти ResourceManager — GL-НЕЗАВИСИМЫЕ части:
//   • SelectEvictions — чистая LRU-политика вытеснения (ядро бюджета VRAM).
//   • DecodeImageFile — CPU-декодирование картинки в RGBA8 (первый шаг
//     асинхронного стриминга; выполняется фоновым потоком без GL).
// GPU-путь (заливка/замена текстур, реальное вытеснение из VRAM) проверяется
// headless-прогоном редактора в scripts/ci_smoke_test.sh — здесь только логика.
#include "TestFramework.h"

#include "sage/render/ResourceManager.h"
#include "sage/render/ModelLoader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

using EC = ResourceManager::EvictCandidate;

TEST(Evict_nothing_when_under_budget) {
    std::vector<EC> c = {
        {0, 100, /*tick*/1, true},
        {1, 100, 2, true},
    };
    // 200 <= 512 — вытеснять нечего.
    auto out = ResourceManager::SelectEvictions(c, 200, 512);
    CHECK_TRUE(out.empty());
}

TEST(Evict_zero_budget_means_unlimited) {
    std::vector<EC> c = {{0, 1000, 1, true}};
    auto out = ResourceManager::SelectEvictions(c, 999999, 0);
    CHECK_TRUE(out.empty()); // бюджет 0 = без ограничения
}

TEST(Evict_oldest_first_until_under_budget) {
    // Три текстуры по 100 байт, суммарно 300, бюджет 150 — надо освободить
    // минимум 150, т.е. выгнать двоих самых старых (tick 1 и 2), оставить tick 3.
    std::vector<EC> c = {
        {0, 100, /*tick*/3, true}, // самая свежая
        {1, 100, 1, true},         // самая старая
        {2, 100, 2, true},
    };
    auto out = ResourceManager::SelectEvictions(c, 300, 150);
    CHECK_EQ((int)out.size(), 2);
    // Порядок вытеснения — от старейшей: сначала индекс 1 (tick 1), затем 2 (tick 2).
    CHECK_EQ((int)out[0], 1);
    CHECK_EQ((int)out[1], 2);
}

TEST(Evict_skips_referenced_textures) {
    // Старейшая (tick 1) ЗАНЯТА (Evictable=false) — её пропускаем, даже если она
    // старше. Выгоняем следующую доступную по возрасту.
    std::vector<EC> c = {
        {0, 100, 1, false}, // старейшая, но ссылаемая — не трогаем
        {1, 100, 2, true},
        {2, 100, 3, true},
    };
    auto out = ResourceManager::SelectEvictions(c, 300, 150);
    // Нужно освободить 150 -> два кандидата, но доступны только index 1 и 2.
    CHECK_EQ((int)out.size(), 2);
    CHECK_EQ((int)out[0], 1);
    CHECK_EQ((int)out[1], 2);
}

TEST(Evict_stops_when_cannot_reach_budget) {
    // Всё занято, кроме одной маленькой записи — освобождаем что можем и стоп
    // (не зацикливаемся, пытаясь достичь недостижимого бюджета).
    std::vector<EC> c = {
        {0, 1000, 1, false},
        {1, 50, 2, true},
    };
    auto out = ResourceManager::SelectEvictions(c, 1050, 100);
    CHECK_EQ((int)out.size(), 1);
    CHECK_EQ((int)out[0], 1);
}

// --- Хелпер: пишет минимальный несжатый 24-битный TGA (truecolor) на диск ---
static bool WriteSolidTGA(const fs::path& path, int w, int h,
                          unsigned char r, unsigned char g, unsigned char b) {
    std::vector<unsigned char> buf;
    unsigned char header[18] = {0};
    header[2] = 2;                     // тип 2 — несжатый truecolor
    header[12] = (unsigned char)(w & 0xFF);
    header[13] = (unsigned char)((w >> 8) & 0xFF);
    header[14] = (unsigned char)(h & 0xFF);
    header[15] = (unsigned char)((h >> 8) & 0xFF);
    header[16] = 24;                   // 24 бита на пиксель
    buf.insert(buf.end(), header, header + 18);
    for (int i = 0; i < w * h; ++i) {  // TGA хранит BGR
        buf.push_back(b);
        buf.push_back(g);
        buf.push_back(r);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    return (bool)f;
}

TEST(DecodeImageFile_reads_rgba_pixels) {
    fs::path p = fs::temp_directory_path() / "sage_test_decode.tga";
    bool wrote = WriteSolidTGA(p, 2, 3, /*r*/200, /*g*/40, /*b*/10);
    CHECK_TRUE(wrote);
    if (!wrote) return;

    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    bool ok = ResourceManager::DecodeImageFile(p.string(), pixels, w, h);
    CHECK_TRUE(ok);
    CHECK_EQ(w, 2);
    CHECK_EQ(h, 3);
    // Форсированный RGBA8: 2*3*4 = 24 байта.
    CHECK_EQ((int)pixels.size(), 24);
    if (pixels.size() == 24) {
        // Любой пиксель — тот же сплошной цвет (R,G,B,255). Проверяем первый.
        CHECK_EQ((int)pixels[0], 200);
        CHECK_EQ((int)pixels[1], 40);
        CHECK_EQ((int)pixels[2], 10);
        CHECK_EQ((int)pixels[3], 255);
    }

    std::error_code ec;
    fs::remove(p, ec);
}

TEST(DecodeImageFile_fails_on_missing_file) {
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    bool ok = ResourceManager::DecodeImageFile("this_file_does_not_exist_12345.png", pixels, w, h);
    CHECK_FALSE(ok);
}

TEST(Texture_estimate_bytes_accounts_for_mips) {
    // Без мипов: w*h*4. С мипами: +~33% (полная цепочка).
    size_t noMip = Texture::EstimateBytes(256, 256, false);
    size_t withMip = Texture::EstimateBytes(256, 256, true);
    CHECK_EQ((int)noMip, 256 * 256 * 4);
    CHECK_TRUE(withMip > noMip);
    CHECK_EQ((int)withMip, (int)(noMip + noMip / 3));
}

// --- Пайплайн импорта моделей: применение настроек к вершинам (GL-free) + сайдкар ---
TEST(ModelImport_scale_and_recenter) {
    std::vector<Vertex> verts(2);
    verts[0].Position = {0.0f, 0.0f, 0.0f};
    verts[1].Position = {2.0f, 0.0f, 0.0f}; // AABB центр (1,0,0), размер 2 по X

    ModelLoader::ImportSettings s;
    s.Recenter = true;   // центр -> 0: точки станут (-1,0,0) и (1,0,0)
    s.Scale = 3.0f;      // затем ×3: (-3,0,0) и (3,0,0)
    ModelLoader::ApplyImportSettings(verts, s);

    CHECK_NEAR(verts[0].Position.x, -3.0f, 1e-4);
    CHECK_NEAR(verts[1].Position.x, 3.0f, 1e-4);
}

TEST(ModelImport_normalize_size) {
    std::vector<Vertex> verts(2);
    verts[0].Position = {0.0f, 0.0f, 0.0f};
    verts[1].Position = {4.0f, 0.0f, 0.0f}; // наибольшая сторона = 4

    ModelLoader::ImportSettings s;
    s.NormalizeSize = true; // /4 -> сторона 1
    ModelLoader::ApplyImportSettings(verts, s);
    CHECK_NEAR(verts[1].Position.x - verts[0].Position.x, 1.0f, 1e-4);
}

TEST(ModelImport_sidecar_roundtrip) {
    std::string model = (fs::temp_directory_path() / "sage_test_model.obj").string();
    ModelLoader::ImportSettings s;
    s.Scale = 2.5f; s.Recenter = true; s.NormalizeSize = false;
    CHECK_TRUE(ModelLoader::SaveImportSettings(model, s));

    ModelLoader::ImportSettings back = ModelLoader::LoadImportSettings(model);
    CHECK_NEAR(back.Scale, 2.5f, 1e-4);
    CHECK_TRUE(back.Recenter);
    CHECK_FALSE(back.NormalizeSize);

    std::error_code ec;
    fs::remove(ModelLoader::ImportSidecarPath(model), ec);

    // Отсутствующий сайдкар -> дефолт (Scale 1, без правок).
    ModelLoader::ImportSettings def = ModelLoader::LoadImportSettings("no_such_model_xyz.obj");
    CHECK_NEAR(def.Scale, 1.0f, 1e-4);
    CHECK_FALSE(def.Recenter);
}
