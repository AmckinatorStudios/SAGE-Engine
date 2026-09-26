// ============================================================================
//  Частицы на экране: текстура, раскадровка из отдельных файлов, смешивание,
//  вытягивание по скорости, объёмные фигуры, следы.
//
//  Модульные тесты (tests/test_particles.cpp) проверяют, КАК частица живёт;
//  здесь — что это доезжает до пикселей. Раньше частица умела только мягкий
//  круг одного цвета: ни картинки, ни раскадровки, ни режима отрисовки.
//
//  Отдельный прогон по чужому набору текстур: если заданы переменные
//  SAGE_PARTICLE_PACK_DIR (папка с картинками частиц) и SAGE_PARTICLE_SHOT_DIR
//  (куда сложить кадры), из набора собираются эффекты (дым из кадров, пламя,
//  искры, пузыри, дождь, снег) и снимаются кадры для глаза. Набор в репозиторий
//  не кладётся: чужие картинки движку не принадлежат.
// ============================================================================
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "stb_image_write.h"

#include "sage/core/Paths.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ParticleEffect.h"
#include "sage/render/ParticleSystem.h"
#include "sage/rhi/GraphicsDevice.h"

namespace fs = std::filesystem;
using namespace sage::fx;

namespace sage::rendertest {
namespace {

constexpr int W = 256, H = 192;

glm::mat4 ParticleView() {
    return glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}
glm::mat4 ParticleProj() { return glm::perspective(glm::radians(50.0f), (float)W / (float)H, 0.1f, 100.0f); }

// Картинка RGBA на диск — для текстур частиц в проверке.
std::string WriteRgba(const fs::path& dir, const std::string& name, int w, int h,
                      const std::function<glm::u8vec4(int, int)>& px) {
    std::vector<unsigned char> data((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const glm::u8vec4 c = px(x, y);
            unsigned char* d = &data[((size_t)y * w + x) * 4];
            d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a;
        }
    const fs::path file = dir / name;
    stbi_write_png(file.string().c_str(), w, h, 4, data.data(), w * 4);
    return file.string();
}

// Кадр с частицами на тёмном фоне.
Image Shoot(ParticleSystem& sys, const glm::vec3& bg = glm::vec3(0.05f, 0.06f, 0.08f),
            const glm::mat4& view = ParticleView()) {
    Framebuffer out(W, H);
    out.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetSRGBWrite(false);
    device.SetClearColor(bg.r, bg.g, bg.b, 1.0f);
    device.Clear(true, true);
    sys.DrawFromView(view, ParticleProj());
    Image img = Capture(W, H);
    device.BindDefaultFramebuffer();
    return img;
}

glm::ivec3 At(const Image& im, int x, int y) {
    const size_t i = ((size_t)y * im.Width + x) * 3;
    return {im.Pixels[i], im.Pixels[i + 1], im.Pixels[i + 2]};
}

// Одна неподвижная частица в центре кадра.
ParticleEffect Still(float size) {
    ParticleEffect f;
    f.RateOverTime = 0.0f;
    f.StartLifetime = {100.0f, 100.0f};
    f.StartSpeed = {0.0f, 0.0f};
    f.StartSize = {size, size};
    f.StartRotation = {0.0f, 0.0f};
    f.Shape = EmitShape::Point;
    return f;
}

// --- 1. Текстура частицы доезжает до кадра ----------------------------------
void TestTextureAndFlipbookFrames(const fs::path& dir) {
    // Картинка: красный круг на прозрачном. Квадрат вокруг обязан остаться фоном.
    const std::string disc = WriteRgba(dir, "disc.png", 32, 32, [](int x, int y) {
        const float d = std::hypot(x - 15.5f, y - 15.5f);
        return d < 12.0f ? glm::u8vec4(255, 20, 20, 255) : glm::u8vec4(0, 0, 0, 0);
    });
    ParticleSystem sys;
    ParticleEffect f = Still(2.0f);
    f.Texture = disc;
    sys.Burst(f, glm::vec3(0.0f), 1);
    const Image img = Shoot(sys);
    const glm::ivec3 center = At(img, W / 2, H / 2);
    // Угол квадрата частицы (прозрачный в картинке) — фон.
    const int half = (int)(2.0f * 0.5f / (5.0f * std::tan(glm::radians(25.0f))) * H * 0.5f);
    const glm::ivec3 corner = At(img, W / 2 + half - 3, H / 2 + half - 3);
    std::printf("       текстура: центр (%d,%d,%d), угол (%d,%d,%d)\n", center.r, center.g, center.b,
                corner.r, corner.g, corner.b);
    Check(center.r > 200 && center.g < 60, "картинка частицы видна в кадре");
    Check(corner.r < 40, "прозрачное в картинке — прозрачно и в кадре");

    // Раскадровка из ОТДЕЛЬНЫХ файлов: четыре кадра разного цвета, движок
    // склеивает их в лист сам. Проверяется и склейка, и то, что номер кадра
    // попадает в свою ячейку (перевёрнутый лист дал бы чужой цвет).
    const glm::u8vec4 colors[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    std::vector<std::string> frames;
    for (int i = 0; i < 4; ++i) {
        const glm::u8vec4 c = colors[i];
        frames.push_back(WriteRgba(dir, "frame_" + std::to_string(i) + ".png", 8, 8,
                                   [c](int, int) { return c; }));
    }
    for (int k = 0; k < 4; ++k) {
        ParticleSystem s;
        ParticleEffect e = Still(2.0f);
        e.Frames = frames;
        e.Flipbook = FlipbookMode::Fixed;
        e.StartFrame = k;
        e.PixelArt = true;
        s.Burst(e, glm::vec3(0.0f), 1);
        const glm::ivec3 got = At(Shoot(s), W / 2, H / 2);
        const glm::ivec3 want(colors[k].r, colors[k].g, colors[k].b);
        const bool ok = std::abs(got.r - want.r) < 30 && std::abs(got.g - want.g) < 30 && std::abs(got.b - want.b) < 30;
        std::printf("       кадр %d: (%d,%d,%d)\n", k, got.r, got.g, got.b);
        Check(ok, "кадр раскадровки из отдельных файлов — своего цвета");
    }
}

// --- 2. Смешивание: сложение светится, прозрачность — нет --------------------
void TestBlendModes() {
    auto overlap = [](BlendMode mode) {
        ParticleSystem sys;
        ParticleEffect f = Still(2.0f);
        f.Sprite = ParticleSprite::Square;
        f.StartColorA = f.StartColorB = glm::vec4(0.4f, 0.2f, 0.1f, 1.0f);
        f.Blend = mode;
        sys.Burst(f, glm::vec3(0.0f), 3);   // три слоя в одной точке
        return At(Shoot(sys, glm::vec3(0.0f)), W / 2, H / 2);
    };
    const glm::ivec3 alpha = overlap(BlendMode::Alpha);
    const glm::ivec3 add = overlap(BlendMode::Additive);
    std::printf("       три слоя: прозрачность %d, сложение %d (красный)\n", alpha.r, add.r);
    Check(std::abs(alpha.r - 102) < 12, "непрозрачные слои не складываются");
    Check(add.r > 250, "при сложении слои накапливают свет");
}

// --- 3. Вытянутые по скорости: полоса вдоль движения -------------------------
void TestStretchedAlongMotion() {
    ParticleSystem sys;
    ParticleEffect f = Still(0.3f);
    f.Sprite = ParticleSprite::Square;
    f.StartSpeed = {4.0f, 4.0f};
    f.Shape = EmitShape::Cone;
    f.ConeAngle = 0.0f;
    f.Radius = 0.0f;
    f.ShapeRotation = glm::vec3(0.0f, 0.0f, -90.0f);   // летит вдоль +X
    f.Render = RenderMode::Stretched;
    f.StretchLength = 1.0f;
    f.StretchBySpeed = 1.0f;
    sys.Burst(f, glm::vec3(0.0f), 1);
    sys.Update(1.0f / 60.0f);
    const Image img = Shoot(sys);
    int minX = W, maxX = 0, minY = H, maxY = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (At(img, x, y).r > 128) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    const int wide = maxX - minX, tall = maxY - minY;
    std::printf("       вытянутая частица: %d × %d пикселей\n", wide, tall);
    Check(wide > tall * 3, "частица вытянута вдоль скорости");
}

// --- 4. Объёмные фигуры: грани затенены по-разному ---------------------------
void TestMeshParticles() {
    ParticleSystem sys;
    ParticleEffect f = Still(1.6f);
    f.Render = RenderMode::Mesh;
    f.Mesh = MeshShape::Cube;
    f.StartColorA = f.StartColorB = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    f.StartRotation = {35.0f, 35.0f};
    sys.Burst(f, glm::vec3(0.0f), 1);
    const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, 3.0f, 3.5f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
    const Image img = Shoot(sys, glm::vec3(0.0f), view);
    std::vector<int> shades;
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            const int v = At(img, x, y).g;
            if (v > 20) shades.push_back(v / 16);
        }
    std::sort(shades.begin(), shades.end());
    shades.erase(std::unique(shades.begin(), shades.end()), shades.end());
    std::printf("       куб-частица: различимых оттенков %d\n", (int)shades.size());
    Check(shades.size() >= 2, "у объёмной частицы видны грани");
}

// --- 5. След: лента тянется за частицей ---------------------------------------
void TestTrails() {
    ParticleSystem sys;
    ParticleEffect f = Still(0.25f);
    f.StartSpeed = {3.0f, 3.0f};
    f.Shape = EmitShape::Cone;
    f.ConeAngle = 0.0f;
    f.Radius = 0.0f;
    f.ShapeRotation = glm::vec3(0.0f, 0.0f, -90.0f);
    f.Render = RenderMode::Trail;
    f.TrailLifetime = 1.0f;
    f.TrailMinDistance = 0.02f;
    f.TrailHead = false;
    f.TrailMaxPoints = 64;   // весь путь в следе (по умолчанию — 24 точки)
    sys.Burst(f, glm::vec3(-1.5f, 0.0f, 0.0f), 1);
    for (int i = 0; i < 40; ++i) sys.Update(1.0f / 60.0f);   // 2 м пути
    const Image img = Shoot(sys);
    int lit = 0;
    for (int x = 0; x < W; ++x)
        if (At(img, x, H / 2).r > 100) ++lit;
    std::printf("       след: освещено %d пикселей вдоль пути\n", lit);
    Check(lit > W / 4, "за частицей тянется лента следа");
}

// --- 6. Набор текстур пользователя: эффекты данными и кадры для глаза ----------
void ShootPack(const fs::path& pack, const fs::path& shots) {
    auto has = [&](const std::string& n) {
        std::error_code ec;
        return fs::exists(pack / n, ec);
    };
    auto path = [&](const std::string& n) { return (pack / n).string(); };
    auto sequence = [&](const std::string& stem) {
        std::vector<std::string> out;
        for (int i = 0; has(stem + std::to_string(i) + ".png"); ++i) out.push_back(path(stem + std::to_string(i) + ".png"));
        return out;
    };
    fs::create_directories(shots);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.2f, 6.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0, 1, 0));

    struct Shot { const char* Name; ParticleEffect Fx; glm::vec3 At; float Seconds; };
    std::vector<Shot> list;

    if (!sequence("big_smoke_").empty()) {
        ParticleEffect f;
        f.Frames = sequence("big_smoke_");
        f.PixelArt = true;
        f.RateOverTime = 25.0f;
        f.Shape = EmitShape::Cone;
        f.Radius = 0.3f;
        f.ConeAngle = 10.0f;
        f.StartLifetime = {2.0f, 3.0f};
        f.StartSpeed = {0.5f, 0.8f};
        f.StartSize = {0.6f, 0.9f};
        f.StartColorA = glm::vec4(0.55f, 0.55f, 0.55f, 0.8f);
        f.StartColorB = glm::vec4(0.75f, 0.75f, 0.75f, 0.8f);
        f.UseForces = true;
        f.Wind = {0.8f, 0.0f, 0.0f};
        f.WindInfluence = 0.3f;
        f.UseNoise = true;
        f.NoiseStrength = 0.4f;
        f.UseSizeOverLifetime = true;
        f.SizeOverLifetime = Curve::Linear(0.7f, 1.8f);
        f.UseColorOverLifetime = true;
        f.ColorOverLifetime.Alphas = {{0.0f, 0.0f}, {0.1f, 1.0f}, {1.0f, 0.0f}};
        f.SortByDistance = true;
        list.push_back({"smoke_frames", f, glm::vec3(0.0f, 0.0f, 0.0f), 3.0f});
    }
    if (has("flame.png")) {
        ParticleEffect f;
        f.Texture = path("flame.png");
        f.PixelArt = true;
        f.RateOverTime = 40.0f;
        f.Shape = EmitShape::Circle;
        f.Radius = 0.35f;
        f.StartLifetime = {0.4f, 0.8f};
        f.StartSpeed = {0.2f, 0.4f};
        f.StartSize = {0.3f, 0.45f};
        f.UseForces = true;
        f.Force = {0.0f, 1.8f, 0.0f};
        f.UseSizeOverLifetime = true;
        f.SizeOverLifetime = Curve{{{0.0f, 1.0f}, {1.0f, 0.3f}}};
        f.UseColorOverLifetime = true;
        f.ColorOverLifetime.Alphas = {{0.0f, 1.0f}, {0.7f, 1.0f}, {1.0f, 0.0f}};
        f.Blend = BlendMode::Additive;
        f.Intensity = 1.5f;
        list.push_back({"flame", f, glm::vec3(0.0f), 1.5f});
    }
    if (!sequence("spark_").empty() || !sequence("generic_").empty()) {
        ParticleEffect f;
        f.Frames = !sequence("spark_").empty() ? sequence("spark_") : sequence("generic_");
        f.PixelArt = true;
        f.RateOverTime = 0.0f;
        f.Duration = 1.0f;
        f.Bursts.push_back({0.0f, 60, 80, 0, 1.0f});
        f.Shape = EmitShape::Hemisphere;
        f.Radius = 0.1f;
        f.StartLifetime = {0.6f, 1.2f};
        f.StartSpeed = {2.5f, 5.0f};
        f.StartSize = {0.12f, 0.2f};
        f.StartColorA = glm::vec4(1.0f, 0.9f, 0.5f, 1.0f);
        f.StartColorB = glm::vec4(1.0f, 0.6f, 0.2f, 1.0f);
        f.GravityScale = 0.8f;
        f.UseCollision = true;
        f.PlaneHeight = 0.0f;
        f.Bounce = 0.35f;
        f.Blend = BlendMode::Additive;
        list.push_back({"sparks", f, glm::vec3(0.0f, 1.0f, 0.0f), 0.6f});
    }
    if (has("bubble.png")) {
        ParticleEffect f;
        f.Texture = path("bubble.png");
        f.PixelArt = true;
        f.RateOverTime = 15.0f;
        f.Shape = EmitShape::Box;
        f.BoxSize = {3.0f, 0.1f, 1.0f};
        f.StartLifetime = {2.0f, 3.0f};
        f.StartSpeed = {0.4f, 0.8f};
        f.StartSize = {0.12f, 0.25f};
        f.UseNoise = true;
        f.NoiseStrength = 0.6f;
        f.NoiseFrequency = 1.5f;
        list.push_back({"bubbles", f, glm::vec3(0.0f), 3.0f});
    }
    if (has("drip_fall.png")) {
        ParticleEffect f;   // дождь: коробка над сценой, вытянутые капли, отскок от земли
        f.Texture = path("drip_fall.png");
        f.PixelArt = true;
        f.RateOverTime = 300.0f;
        f.Shape = EmitShape::Box;
        f.BoxSize = {8.0f, 0.1f, 4.0f};
        f.ShapeRotation = {180.0f, 0.0f, 0.0f};
        f.StartLifetime = {1.0f, 1.0f};
        f.StartSpeed = {6.0f, 8.0f};
        f.StartSize = {0.05f, 0.08f};
        f.StartColorA = f.StartColorB = glm::vec4(0.6f, 0.7f, 1.0f, 0.8f);
        f.UseForces = true;
        f.Wind = {-1.5f, 0.0f, 0.0f};
        f.WindInfluence = 0.5f;
        f.UseCollision = true;
        f.PlaneHeight = 0.0f;
        f.LifetimeLoss = 1.0f;
        f.Render = RenderMode::Stretched;
        f.StretchBySpeed = 0.5f;
        list.push_back({"rain", f, glm::vec3(0.0f, 4.0f, 0.0f), 1.5f});
    }
    {
        ParticleEffect f;   // снег: без картинки, мягкие хлопья, ветер с порывами, турбулентность
        f.RateOverTime = 120.0f;
        f.Shape = EmitShape::Box;
        f.BoxSize = {8.0f, 0.1f, 4.0f};
        f.ShapeRotation = {180.0f, 0.0f, 0.0f};
        f.StartLifetime = {4.0f, 5.0f};
        f.StartSpeed = {0.5f, 0.9f};
        f.StartSize = {0.05f, 0.1f};
        f.UseForces = true;
        f.Wind = {0.8f, 0.0f, 0.0f};
        f.Gustiness = 0.7f;
        f.WindInfluence = 0.5f;
        f.UseNoise = true;
        f.NoiseStrength = 0.4f;
        list.push_back({"snow", f, glm::vec3(0.0f, 4.0f, 0.0f), 4.0f});
    }

    for (const Shot& s : list) {
        ParticleSystem sys;
        sys.CreateStream(s.Name, s.Fx, s.At);
        sys.SetStreamActive(s.Name, true);
        for (float t = 0.0f; t < s.Seconds; t += 1.0f / 60.0f) sys.Update(1.0f / 60.0f);
        Framebuffer out(640, 400);
        out.Bind();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetSRGBWrite(false);
        device.SetClearColor(0.18f, 0.24f, 0.34f, 1.0f);
        device.Clear(true, true);
        sys.DrawFromView(view, glm::perspective(glm::radians(55.0f), 640.0f / 400.0f, 0.1f, 100.0f));
        const Image img = Capture(640, 400);
        device.BindDefaultFramebuffer();
        const std::string file = (shots / (std::string(s.Name) + ".png")).string();
        SavePng(file, img);
        std::printf("       набор текстур: %s — живых частиц %zu → %s\n", s.Name, sys.AliveCount(), file.c_str());
        Check(sys.AliveCount() > 0, "эффект из чужого набора текстур живёт");
    }
}

} // namespace

void RunParticleChecks() {
    std::printf("\n--- Частицы ---\n");
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_particle_render";
    fs::create_directories(dir, ec);
    TestTextureAndFlipbookFrames(dir);
    TestBlendModes();
    TestStretchedAlongMotion();
    TestMeshParticles();
    TestTrails();
    fs::remove_all(dir, ec);

    const fs::path pack = sage::EnvPath("SAGE_PARTICLE_PACK_DIR");
    const fs::path shots = sage::EnvPath("SAGE_PARTICLE_SHOT_DIR");
    if (!pack.empty() && !shots.empty()) ShootPack(pack, shots);
}

} // namespace sage::rendertest
