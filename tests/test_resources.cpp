// Модульные тесты менеджера памяти ResourceManager — GL-НЕЗАВИСИМЫЕ части:
//   • SelectEvictions — чистая LRU-политика вытеснения (ядро бюджета VRAM).
//   • DecodeImageFile — CPU-декодирование картинки в RGBA8 (первый шаг
//     асинхронного стриминга; выполняется фоновым потоком без GL).
// GPU-путь (заливка/замена текстур, реальное вытеснение из VRAM) проверяется
// headless-прогоном редактора в scripts/ci_smoke_test.sh — здесь только логика.
#include "TestFramework.h"

#include "sage/render/ResourceManager.h"
#include "sage/render/ModelLoader.h"
#include "rhi/null/NullDevice.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/core/Profiler.h"

#include <cstring>
#include <string>

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

// ============================================================================
//  Null-бэкенд: доказательство границы RHI
// ============================================================================
//
// Смысл этих проверок не в самом Null-устройстве, а в том, что движок с ним
// РАБОТАЕТ. Ресурсы создаются, геометрия рисуется, состояние выставляется — и
// всё это без единого вызова OpenGL. Значит движок нигде за пределами rhi/ на
// OpenGL не рассчитывает: раньше это было утверждение, теперь проверка.
//
// Чего эти проверки НЕ доказывают: что интерфейс годится для Vulkan. Пустышка
// соглашается с любой формой интерфейса, ей нечего возразить (см. комментарий
// в rhi/null/NullDevice.h).
TEST(null_backend_runs_engine_without_gl) {
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    CHECK_TRUE(device != nullptr);
    device->Init(nullptr);
    CHECK_TRUE(std::string(device->BackendName()) == "Null");

    sage::rhi::GraphicsDevice::SetCurrent(device.get());
    sage::rhi::NullDevice::ResetCounters();

    // Ресурсы всех видов — те же вызовы, что делает настоящий рендер.
    std::string err;
    auto shader = device->CreateShaderProgram("void main(){}", "void main(){}");
    CHECK_TRUE(shader != nullptr);

    sage::rhi::VertexLayout layout;
    auto geometry = device->CreateGeometry(layout);
    CHECK_TRUE(geometry != nullptr);

    sage::rhi::Texture2DDesc texDesc;
    texDesc.Width = 4; texDesc.Height = 4;
    const unsigned char pixels[4 * 4 * 4] = {};
    auto texture = device->CreateTexture2D(texDesc, pixels);
    CHECK_TRUE(texture != nullptr);
    // Хендлы разных ресурсов обязаны различаться: ноль означает «нет ресурса»,
    // а совпадение маскировало бы ошибки привязки.
    auto texture2 = device->CreateTexture2D(texDesc, pixels);
    CHECK_TRUE(texture->NativeHandle() != 0);
    CHECK_TRUE(texture->NativeHandle() != texture2->NativeHandle());

    sage::rhi::RenderTargetDesc rtDesc;
    rtDesc.Width = 64; rtDesc.Height = 64;
    auto target = device->CreateRenderTarget(rtDesc);
    CHECK_TRUE(target != nullptr);
    CHECK_TRUE(target->Width() == 64);
    target->Resize(128, 32);
    CHECK_TRUE(target->Width() == 128 && target->Height() == 32);

    // Отрисовка и состояние.
    device->SetViewport(0, 0, 128, 128);
    device->Clear();
    shader->Use();
    shader->SetMat4("uModel", glm::mat4(1.0f));
    geometry->DrawIndexed(36);
    geometry->DrawIndexedInstanced(36, 10);

    const sage::rhi::NullCounters& counters = sage::rhi::NullDevice::Counters();
    CHECK_TRUE(counters.DrawCalls == 2);
    CHECK_TRUE(counters.Shaders == 1);
    CHECK_TRUE(counters.Textures == 2);
    CHECK_TRUE(counters.RenderTargets == 1);
    CHECK_TRUE(counters.StateChanges > 0);

    // Чтение пикселей отдаёт чёрный кадр, а не мусор: недетерминированные
    // данные здесь ломали бы проверки в местах, где причина неочевидна.
    unsigned char frame[2 * 2 * 3];
    std::memset(frame, 0xAB, sizeof(frame));
    device->ReadPixelsRGB(0, 0, 2, 2, frame);
    bool cleared = true;
    for (unsigned char c : frame) cleared = cleared && c == 0;
    CHECK_TRUE(cleared);

    sage::rhi::GraphicsDevice::SetCurrent(nullptr);
}

// Профилировщик кадра БЕЗ видеокарты: с Null-бэкендом таймеров GPU нет, и
// проверяется ровно то, что не зависит от драйвера, — учёт участков, их
// вложенность и то, что выключенный профилировщик не стоит ничего.
//
// Почему тут нет проверок «проход занял меньше N мс»: время на машине CI
// зависит от соседей по железу, и такой порог либо ничего не ловит, либо
// падает на ровном месте. Меряем структуру, а не скорость.
TEST(profiler_records_nested_scopes) {
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    device->Init(nullptr);
    sage::rhi::GraphicsDevice::SetCurrent(device.get());

    // Выключенный профилировщик обязан молчать: ни одной записи, иначе за
    // измерения платили бы в игре у игрока.
    sage::profile::SetEnabled(false);
    sage::profile::BeginFrame();
    { SAGE_PROFILE("Мимо"); }
    sage::profile::EndFrame();
    CHECK_TRUE(sage::profile::Frame().empty());

    sage::profile::SetEnabled(true);
    CHECK_TRUE(sage::profile::Enabled());
    // Null не умеет таймеров GPU — и не притворяется, что умеет.
    CHECK_TRUE(!sage::profile::GpuTimersAvailable());

    // Результат созревает не сразу (метки GPU читаются через несколько кадров),
    // поэтому крутим несколько одинаковых кадров и смотрим на готовый.
    for (int i = 0; i < 8; ++i) {
        sage::profile::BeginFrame();
        {
            SAGE_PROFILE("Отрисовка");
            { SAGE_PROFILE("Тени"); }
            { SAGE_PROFILE("Сцена"); }
        }
        { SAGE_PROFILE("Интерфейс"); }
        sage::profile::EndFrame();
    }

    const std::vector<sage::profile::Entry>& frame = sage::profile::Frame();
    CHECK_TRUE(frame.size() == 4);
    // Порядок — тот, в котором участки ОТКРЫВАЛИСЬ, поэтому вложенные идут
    // сразу за родителем, а не после него.
    CHECK_TRUE(frame[0].Name == "Отрисовка" && frame[0].Depth == 0);
    CHECK_TRUE(frame[1].Name == "Тени" && frame[1].Depth == 1);
    CHECK_TRUE(frame[2].Name == "Сцена" && frame[2].Depth == 1);
    CHECK_TRUE(frame[3].Name == "Интерфейс" && frame[3].Depth == 0);

    // Родитель не может длиться меньше своего ребёнка: это была бы явная
    // ошибка учёта, а не разброс измерений.
    CHECK_TRUE(frame[0].CpuMs >= frame[1].CpuMs);
    CHECK_TRUE(sage::profile::FrameCpuMs() > 0.0);

    // Усреднение — по ИМЕНИ: состав проходов меняется от кадра к кадру
    // (эффект включили, тени выключили), и «третья строка с третьей» складывала
    // бы разные вещи.
    const std::vector<sage::profile::Entry>& avg = sage::profile::Average();
    CHECK_TRUE(avg.size() == 4);
    bool named = false;
    for (const sage::profile::Entry& e : avg) named = named || e.Name == "Тени";
    CHECK_TRUE(named);

    // Незакрытый Pop не должен ронять процесс: профилировщик — инструмент
    // отладки, и падать из-за него хуже, чем недосчитать участок.
    sage::profile::Pop();
    sage::profile::Pop();

    sage::profile::SetEnabled(false);
    sage::rhi::GraphicsDevice::SetCurrent(nullptr);
}

// Уменьшение картинки вдвое — усреднением 2x2, а не прореживанием. Проверяется
// без GL: это чистая арифметика над байтами.
TEST(downscale_averages_and_halves) {
    // Шахматка 4x4: чёрное и белое через пиксель. При усреднении 2x2 каждый
    // выходной пиксель обязан стать ровно серединой (127 или 128), а при
    // прореживании остался бы чёрным или белым — то есть эта проверка ловит
    // именно подмену усреднения на выбрасывание.
    std::vector<unsigned char> src((size_t)4 * 4 * 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const unsigned char v = ((x + y) % 2) ? 255 : 0;
            for (int c = 0; c < 4; ++c) src[((size_t)y * 4 + x) * 4 + c] = v;
        }
    }

    std::vector<unsigned char> dst;
    int w = 0, h = 0;
    ResourceManager::DownscaleRGBA(src, 4, 4, dst, w, h);
    CHECK_TRUE(w == 2 && h == 2);
    CHECK_TRUE(dst.size() == (size_t)2 * 2 * 4);
    for (unsigned char v : dst) CHECK_TRUE(v >= 126 && v <= 129);

    // Нечётные стороны и вырожденные размеры встречаются в реальных файлах —
    // падать на них нельзя.
    std::vector<unsigned char> odd((size_t)3 * 5 * 4, 200);
    ResourceManager::DownscaleRGBA(odd, 3, 5, dst, w, h);
    CHECK_TRUE(w == 1 && h == 2);
    ResourceManager::DownscaleRGBA(odd, 1, 1, dst, w, h);
    CHECK_TRUE(w == 1 && h == 1);
}

// Политика понижения: кого уменьшать, чтобы уложиться в бюджет.
TEST(downgrade_policy_picks_least_recently_used) {
    using D = ResourceManager::DowngradeCandidate;
    // Четыре текстуры по 4 МБ, бюджет 10 МБ. Понижение вдвое по стороне
    // освобождает три четверти, то есть 3 МБ с каждой.
    std::vector<D> cands;
    for (size_t i = 0; i < 4; ++i) {
        D d;
        d.Index = i;
        d.Bytes = 4u * 1024 * 1024;
        // Порядок обращений НАРОЧНО обратен порядку в списке: иначе проверка
        // «выбрали самые старые» проходила бы и без сортировки, просто потому
        // что список уже лежит в нужном порядке.
        d.Tick = (uint64_t)(100 - i); // индекс 3 — самая старая
        d.Side = 1024;
        d.Streamable = true;
        cands.push_back(d);
    }
    const size_t current = 16u * 1024 * 1024;
    const size_t budget = 10u * 1024 * 1024;

    std::vector<size_t> picked = ResourceManager::SelectDowngrades(cands, current, budget);
    // 16 - 3 = 13 > 10, 13 - 3 = 10 <= 10: хватает двух.
    CHECK_TRUE(picked.size() == 2);
    // И это САМЫЕ СТАРЫЕ: понижение заметнее всего на том, на что смотрят сейчас.
    CHECK_TRUE(picked[0] == 3 && picked[1] == 2);

    // В бюджете — не трогаем никого.
    CHECK_TRUE(ResourceManager::SelectDowngrades(cands, budget, budget).empty());
    // Бюджет 0 означает «без ограничения», а не «уменьшить всё до нуля».
    CHECK_TRUE(ResourceManager::SelectDowngrades(cands, current, 0).empty());

    // Мелкие текстуры не трогаются: экономия копеечная, а каша заметна.
    for (D& d : cands) d.Side = 64;
    CHECK_TRUE(ResourceManager::SelectDowngrades(cands, current, budget).empty());

    // Текстуры в полёте пропускаются — их картинка вот-вот сменится.
    for (D& d : cands) { d.Side = 1024; d.Streamable = false; }
    CHECK_TRUE(ResourceManager::SelectDowngrades(cands, current, budget).empty());
}

// --- База ассетов: устойчивость к переименованию ------------------------------
//
// Раньше ассет опознавался ПУТЁМ. Путь — не личность, а адрес: он меняется от
// переименования, от переноса в папку, от наведения порядка. Каждое такое
// движение молча ломало сцены — молча в буквальном смысле: сцена грузилась,
// объект оставался на месте, просто без модели, и в логе не было ни строчки.

#include "sage/assets/AssetDatabase.h"

#include <filesystem>
#include <fstream>

namespace {
namespace afs = std::filesystem;

// Одноразовый проект во временной папке: база работает с файлами на диске, и
// проверять её на выдуманных путях бессмысленно.
struct TempProject {
    afs::path Dir;
    explicit TempProject(const char* name) {
        Dir = afs::temp_directory_path() / (std::string("sage_assetdb_") + name);
        afs::remove_all(Dir);
        afs::create_directories(Dir / "assets" / "models");
    }
    ~TempProject() {
        std::error_code ec;
        afs::remove_all(Dir, ec);
    }
    std::string Write(const std::string& rel, const std::string& body = "x") {
        const afs::path full = Dir / rel;
        afs::create_directories(full.parent_path());
        std::ofstream(full) << body;
        return rel;
    }
};
} // namespace

TEST(AssetDatabase_gives_a_file_a_stable_identity) {
    TempProject p("identity");
    p.Write("assets/models/hero.glb");

    sage::AssetDatabase db;
    CHECK_TRUE(db.ScanProject(p.Dir.string()) >= 1);

    const sage::AssetGuid guid = db.GuidOf("assets/models/hero.glb");
    CHECK_TRUE(guid.Valid());
    CHECK_TRUE(db.PathOf(guid) == "assets/models/hero.glb");

    // Повторное сканирование НЕ меняет личность: иначе каждый запуск редактора
    // протухал бы все ссылки разом.
    sage::AssetDatabase db2;
    db2.ScanProject(p.Dir.string());
    CHECK_TRUE(db2.GuidOf("assets/models/hero.glb") == guid);
}

TEST(AssetDatabase_survives_a_rename) {
    TempProject p("rename");
    p.Write("assets/models/hero.glb");

    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());
    const sage::AssetGuid guid = db.GuidOf("assets/models/hero.glb");
    CHECK_TRUE(guid.Valid());

    // Переименовываем ВМЕСТЕ с сайдкаром — ровно так это делают проводник, git
    // и любой инструмент, переносящий файлы: .meta лежит рядом и называется
    // так же, поэтому едет следом сам.
    afs::rename(p.Dir / "assets/models/hero.glb", p.Dir / "assets/models/protagonist.glb");
    afs::rename(p.Dir / "assets/models/hero.glb.meta",
                p.Dir / "assets/models/protagonist.glb.meta");

    db.ScanProject(p.Dir.string());
    // Тот же GUID — НОВЫЙ путь. Это и есть починка: ссылка в сцене цела.
    std::printf("       после переименования GUID %s -> %s\n", guid.ToString().c_str(),
                db.PathOf(guid).c_str());
    CHECK_TRUE(db.PathOf(guid) == "assets/models/protagonist.glb");
    CHECK_TRUE(db.GuidOf("assets/models/protagonist.glb") == guid);
    // Старый путь больше ничего не значит — и это правильно.
    CHECK_FALSE(db.GuidOf("assets/models/hero.glb").Valid());
}

TEST(AssetDatabase_resolves_by_guid_after_a_move) {
    TempProject p("resolve");
    p.Write("assets/models/box.glb");

    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());
    const sage::AssetGuid guid = db.GuidOf("assets/models/box.glb");

    afs::create_directories(p.Dir / "assets/props");
    afs::rename(p.Dir / "assets/models/box.glb", p.Dir / "assets/props/box.glb");
    afs::rename(p.Dir / "assets/models/box.glb.meta", p.Dir / "assets/props/box.glb.meta");
    db.ScanProject(p.Dir.string());

    // Сцена помнит СТАРЫЙ путь и GUID. Побеждать должен GUID — иначе весь
    // механизм не имел бы смысла.
    const std::string resolved = db.Resolve(guid, "assets/models/box.glb");
    std::printf("       сцена помнит assets/models/box.glb, база отвечает %s\n", resolved.c_str());
    CHECK_TRUE(resolved == "assets/props/box.glb");
}

TEST(AssetDatabase_adopts_projects_made_before_it_existed) {
    // Старый проект: файлы есть, GUID'ов нет, сцена ссылается путём. Отказать
    // здесь значило бы, что такие проекты просто не открываются.
    TempProject p("legacy");
    p.Write("assets/models/old.glb");

    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());
    // Ссылка без GUID, но с живым путём — обязана разрешиться и обзавестись
    // личностью на будущее.
    const std::string resolved = db.Resolve(sage::AssetGuid{}, "assets/models/old.glb");
    CHECK_TRUE(resolved == "assets/models/old.glb");
    CHECK_TRUE(db.GuidOf("assets/models/old.glb").Valid());
}

TEST(AssetDatabase_reports_a_missing_asset_instead_of_staying_silent) {
    TempProject p("missing");
    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());

    const sage::AssetGuid ghost = sage::AssetGuid::Generate();
    const std::string resolved = db.Resolve(ghost, "assets/models/gone.glb");
    CHECK_TRUE(resolved.empty());
    CHECK_EQ((int)db.Broken().size(), 1);
    CHECK_TRUE(db.Broken()[0].Hint == "assets/models/gone.glb");
    std::printf("       сломанная ссылка зафиксирована: %s\n", db.Broken()[0].Hint.c_str());

    // Повторное обращение не должно копить одинаковые записи: список читает
    // человек, и сто копий одной строки в нём бесполезны.
    db.Resolve(ghost, "assets/models/gone.glb");
    CHECK_EQ((int)db.Broken().size(), 1);
}

TEST(AssetDatabase_tracks_who_depends_on_what) {
    // «Удалить ассет» и «переименовать ассет» — операции с последствиями, и
    // показать их надо ДО, а не после.
    TempProject p("deps");
    p.Write("assets/models/hero.glb");
    p.Write("assets/materials/skin.sagemat");

    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());
    const sage::AssetGuid model = db.GuidOf("assets/models/hero.glb");
    const sage::AssetGuid mat = db.GuidOf("assets/materials/skin.sagemat");

    db.AddDependency(model, mat);
    CHECK_EQ((int)db.Dependents(mat).size(), 1);
    CHECK_TRUE(db.Dependents(mat)[0] == model);
    CHECK_EQ((int)db.Dependencies(model).size(), 1);

    // Дубликаты не копятся: одна и та же связь может объявляться при каждой
    // загрузке сцены.
    db.AddDependency(model, mat);
    CHECK_EQ((int)db.Dependents(mat).size(), 1);
    // Ссылка на себя бессмысленна и должна отбрасываться.
    db.AddDependency(model, model);
    CHECK_EQ((int)db.Dependencies(model).size(), 1);
}

TEST(AssetGuid_round_trips_through_text_and_rejects_garbage) {
    const sage::AssetGuid g = sage::AssetGuid::Generate();
    const std::string text = g.ToString();
    CHECK_EQ((int)text.size(), 32);
    CHECK_TRUE(sage::AssetGuid::FromString(text) == g);

    // Битый текст даёт невалидный GUID, а не исключение: ссылка на
    // несуществующий ассет — обычное состояние проекта в работе.
    CHECK_FALSE(sage::AssetGuid::FromString("").Valid());
    CHECK_FALSE(sage::AssetGuid::FromString("не-guid").Valid());
    CHECK_FALSE(sage::AssetGuid::FromString(std::string(32, 'z')).Valid());
    // Сгенерированный GUID не может быть нулевым: ноль означает «нет ссылки».
    for (int i = 0; i < 64; ++i) CHECK_TRUE(sage::AssetGuid::Generate().Valid());
}

// --- Путь, открываемый из чужого каталога -------------------------------------
//
// Ловит ровно ту ошибку, которую нашёл живой прогон: пути в проекте
// относительны его корню, и это верно, пока процесс запущен ИЗ корня. Собранная
// игра так и работает — а редактор живёт в своём каталоге, и там вода теряла
// материал и рисовалась серой.
TEST(AssetDatabase_finds_project_files_from_another_working_directory) {
    TempProject p("locate");
    p.Write("assets/models/hero.glb");

    sage::AssetDatabase db;
    db.ScanProject(p.Dir.string());

    // Путь от корня проекта — из чужого cwd сам по себе не откроется, и база
    // обязана достроить его до полного.
    const std::string located = db.LocatePath("assets/models/hero.glb");
    CHECK_TRUE(afs::exists(located));
    CHECK_TRUE(located == (p.Dir / "assets/models/hero.glb").string() ||
               located == "assets/models/hero.glb");

    // Путь, который И ТАК открывается, не трогаем: подмена сломала бы игру,
    // запущенную из своего каталога.
    const std::string absolute = (p.Dir / "assets/models/hero.glb").string();
    CHECK_TRUE(db.LocatePath(absolute) == absolute);

    // Файла нет нигде — возвращаем как дали, чтобы об этом сообщил тот, кто его
    // ждал, а не эта функция: она не знает, ошибка это или необязательный ассет.
    CHECK_TRUE(db.LocatePath("assets/models/gone.glb") == "assets/models/gone.glb");
    CHECK_TRUE(db.LocatePath("").empty());
}

// --- Соответствие RHI: контракт, а не привычка --------------------------------
//
// Абстракция с одной реализацией — гипотеза. Пока бэкенд один, «интерфейс» и
// «то, что делает OpenGL» неразличимы, и отличить обязательство от привычки
// невозможно. Набор соответствия выносит контракт из комментариев в код: то, о
// чём интерфейс УЖЕ договорился, обязан выполнять каждый бэкенд.
#include "sage/rhi/Conformance.h"

TEST(RHI_null_backend_satisfies_the_contract) {
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    device->Init(nullptr);

    sage::rhi::ConformanceOptions options;
    // Null не растеризует, и пиксельный уровень честно пропускается: заставлять
    // пустышку «пройти» его значило бы получить зелёный результат, который
    // ничего не значит.
    options.Rasterizes = false;

    const sage::rhi::ConformanceResult result = sage::rhi::RunConformance(*device, options);
    for (const std::string& failure : result.Failures) {
        LOG_ERROR("Test") << "RHI-соответствие: " << failure;
    }
    CHECK_TRUE(result.Ok());
    CHECK_TRUE(result.Checked > 15);   // набор действительно отработал, а не выродился
}

// --- Модели: не только .obj ----------------------------------------------------
//
// Сущность сцены держит один Mesh, и грузился он через LoadObj — то есть в
// редакторе нельзя было поставить в сцену НИ ОДНУ модель в glTF, а это формат,
// в который экспортирует Blender по умолчанию. Класс Model формат понимал, но
// он не Mesh и в ECS не подключён; выглядело это как «модели не грузятся».
TEST(ModelLoader_reads_gltf_not_only_obj) {
    const afs::path dir = afs::temp_directory_path() / "sage_gltf_test";
    afs::remove_all(dir);
    afs::create_directories(dir);

    // Минимальный корректный .gltf: один треугольник, буфер в base64.
    // Позиции (0,0,0), (1,0,0), (0,1,0) — три vec3 float, 36 байт.
    const std::string gltf = R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0]}],
      "nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "mode": 4}]}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                     "min": [0,0,0], "max": [1,1,0]}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
      "buffers": [{"byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]
    })";
    const afs::path path = dir / "tri.gltf";
    std::ofstream(path) << gltf;

    bool loaded = false;
    try {
        const sage::render::MeshData d = ModelLoader::LoadMeshData(path.string());
        loaded = d.Vertices.size() == 3 && d.Indices.size() == 3;
    } catch (const std::exception& e) {
        LOG_ERROR("Test") << "glTF не загрузился: " << e.what();
    }
    CHECK_TRUE(loaded);
    CHECK_TRUE(ModelLoader::IsSupportedModel("hero.glb"));
    CHECK_TRUE(ModelLoader::IsSupportedModel("HERO.GLTF"));
    CHECK_TRUE(ModelLoader::IsSupportedModel("hero.obj"));
    CHECK_FALSE(ModelLoader::IsSupportedModel("hero.fbx"));

    // Три разные причины «не грузится» обязаны звучать по-разному: человеку,
    // у которого модель не встала в сцену, нужна именно та, что случилась.
    std::string missingMsg, badExtMsg;
    try { ModelLoader::LoadMeshData((dir / "нет.obj").string()); }
    catch (const std::exception& e) { missingMsg = e.what(); }
    try { ModelLoader::LoadMeshData(path.string() + ".fbx"); }
    catch (const std::exception& e) { badExtMsg = e.what(); }
    CHECK_TRUE(missingMsg.find("не найден") != std::string::npos);
    CHECK_TRUE(badExtMsg.find("не найден") != std::string::npos ||
               badExtMsg.find("не поддерживается") != std::string::npos);

    afs::remove_all(dir);
}
