// ---------------------------------------------------------------------------
// Эффекты изображения (render/PostFXImage.cpp): Invert, Negative, Grayscale,
// Sepia, Posterize, Pixelate, Blur, Glow, Distortion, Dissolve.
//
// Каждая проверка — по сути эффекта на простом кадре: тёмно-серая левая
// половина, красная правая, белая вертикальная полоска (4 пикселя) в центре. Шейдер,
// который не собрался, звено пропускает молча — кадр выходит неизменным, и
// проверка падает: значит, собирается и работает каждое звено.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
#include <cstdlib>

#include <memory>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ObjectEffects.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/PostEffect.h"
#include "sage/render/PostFX.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rendertest {
namespace {

void Paint(Framebuffer& fb) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    fb.Bind();
    device.SetViewport(0, 0, fb.Width(), fb.Height());
    device.SetClearColor(0.2f, 0.2f, 0.2f, 1.0f);   // слева тёмно-серое
    device.Clear(true, true);
    device.SetClearColor(1.0f, 0.0f, 0.0f, 1.0f);   // справа красное
    device.SetScissor(true, fb.Width() * 3 / 4, 0, fb.Width() / 4, fb.Height());
    device.Clear(true, false);
    device.SetClearColor(1.0f, 1.0f, 1.0f, 1.0f);   // белая полоска в центре
    device.SetScissor(true, fb.Width() / 2, 0, 4, fb.Height());
    device.Clear(true, false);
    device.SetScissor(false);
}

using Params = std::vector<std::pair<const char*, float>>;

Image Run(const char* kind, const Params& params) {
    Framebuffer src(kW, kH), out(kW, kH);
    Paint(src);
    sage::render::PostChain chain;
    AddEffect(chain, kind);
    for (const auto& [name, value] : params) SetParam(chain, kind, name, value);
    sage::render::PostFX fx;
    fx.Render(src.ColorTexture(), src.DepthTexture(), kW, kH, glm::mat4(1.0f), glm::mat4(1.0f), chain, &out, 0, 0,
              kW, kH);
    out.Bind();
    Image img = Capture(kW, kH);
    sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    return img;
}

// Эффекты объекта: маска слоя 1 на левой половине кадра, слой — Invert.
// Пустой тракт камеры = только тон-маппинг; слой меняет ровно маскированное.
Image RunObjectLayer(bool withLayer) {
    Framebuffer src(kW, kH), mask(kW, kH), out(kW, kH);
    Paint(src);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    mask.Bind();
    device.SetViewport(0, 0, kW, kH);
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    device.SetClearColor(1.0f / 255.0f, 0.0f, 0.0f, 1.0f);   // слой 1 — левая половина
    device.SetScissor(true, 0, 0, kW / 2, kH);
    device.Clear(true, false);
    device.SetScissor(false);

    std::vector<sage::render::PostChain> layers(1);
    AddEffect(layers[0], "invert");
    sage::render::PostFX fx;
    if (withLayer) fx.SetObjectLayers(mask.ColorTexture(), &layers);
    const sage::render::PostChain camera;
    fx.Render(src.ColorTexture(), src.DepthTexture(), kW, kH, glm::mat4(1.0f), glm::mat4(1.0f), camera, &out, 0, 0,
              kW, kH);
    out.Bind();
    Image img = Capture(kW, kH);
    device.BindDefaultFramebuffer();
    return img;
}

// Весь путь на настоящей сцене: геометрия -> маска -> слой. Куб A с Invert,
// его потомок над ним, куб B без эффектов, и заслонка перед низом A.
struct ObjectScene {
    std::unique_ptr<Scene> Scene_;
    GameObject A;
};

ObjectScene BuildObjectScene() {
    ObjectScene out;
    out.Scene_ = std::make_unique<Scene>("ObjectEffects");
    Scene& scene = *out.Scene_;
    scene.Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -0.9f, -0.4f));
    scene.Lighting.Sun.Intensity = 2.0f;
    scene.Lighting.AmbientStrength = 0.4f;
    scene.Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene.Lighting.Skybox.Enabled = false;
    auto cube = [&](const char* name, glm::vec3 pos, glm::vec3 scale) {
        GameObject obj = scene.CreateObject(name);
        obj.GetTransform().Position = pos;
        obj.GetTransform().Scale = scale;
        obj.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        obj.Renderer().Color = {0.25f, 0.25f, 0.25f};
        return obj;
    };
    out.A = cube("A", {-1.5f, 0.0f, 0.0f}, glm::vec3(1.0f));
    GameObject child = cube("Child", {0.0f, 1.6f, 0.0f}, glm::vec3(0.8f));
    scene.SetParent(child.Entity(), out.A.Entity());
    cube("B", {1.5f, 0.0f, 0.0f}, glm::vec3(1.0f));
    cube("Shield", {-1.5f, -0.45f, 2.0f}, {1.2f, 0.4f, 0.2f});   // заслоняет низ A
    return out;
}

Image RenderObjectScene(Scene& scene, const glm::mat4& view, const glm::mat4& proj) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    sage::ecs::RenderBatch batch;
    Framebuffer fbo(kW, kH), out(kW, kH);
    fbo.Bind();
    device.SetViewport(0, 0, kW, kH);
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    sage::render::SceneColorInput input;
    input.View = view;
    input.Proj = proj;
    input.ViewPos = glm::vec3(glm::inverse(view)[3]);
    input.Env = &env;
    sage::render::RenderSceneColor(scene, batch, input);
    sage::render::ObjectEffectsMask mask;
    const bool any = mask.Render(scene, batch, view, proj, kW, kH);
    fbo.Resolve();
    sage::render::PostFX fx;
    if (any) fx.SetObjectLayers(mask.Mask(), &mask.Layers());
    const sage::render::PostChain camera;
    fx.Render(fbo.ColorTexture(), fbo.DepthTexture(), kW, kH, proj, view, camera, &out, 0, 0, kW, kH);
    out.Bind();
    Image img = Capture(kW, kH);
    device.BindDefaultFramebuffer();
    return img;
}

const unsigned char* Px(const Image& img, int x, int y) { return &img.Pixels[((size_t)y * img.Width + x) * 3]; }

} // namespace

void RunImageEffectChecks() {
    std::printf("=== Эффекты изображения ===\n");
    const int y = kH / 2;
    const int gray = kW / 8, red = kW * 7 / 8, bar = kW / 2;

    // Исходный кадр (звено на нулевой интенсивности): LDR-кадр уже в гамме
    // дисплея, поэтому сравнение — с ним, а не с 0.2.
    const Image base = Run("invert", {{"intensity", 0.0f}});
    const int g0 = Px(base, gray, y)[0];
    std::printf("    исходное серое %d\n", g0);
    {   // Invert: x -> 1-x; красное становится голубым.
        const Image full = Run("invert", {{"intensity", 1.0f}});
        const unsigned char* g = Px(full, gray, y);
        const unsigned char* r = Px(full, red, y);
        std::printf("    invert: серое %d, красное %d %d %d\n", g[0], r[0], r[1], r[2]);
        Check(std::abs(g[0] - (255 - g0)) < 6 && r[0] < 10 && r[1] > 245 && r[2] > 245,
              "Invert: каждый канал в 1-x, красное — голубое");
        Check(std::abs(g0 - 124) < 20, "Invert на нулевой интенсивности не трогает кадр");
    }
    {   // Negative: светлое и тёмное наоборот, оттенок прежний (красное — красное).
        const Image img = Run("negative", {{"intensity", 1.0f}});
        const unsigned char* g = Px(img, gray, y);
        const unsigned char* r = Px(img, red, y);
        std::printf("    negative: серое %d, красное %d %d %d\n", g[0], r[0], r[1], r[2]);
        Check(std::abs(g[0] - (255 - g0)) < 6, "Negative: яркость в 1-x");
        Check(r[0] > r[1] + 60 && std::abs(r[1] - r[2]) < 8, "Negative: красное остаётся красным по оттенку");
    }
    {
        const unsigned char* r = Px(Run("grayscale", {{"intensity", 1.0f}}), red, y);
        Check(std::abs(r[0] - r[1]) < 4 && std::abs(r[1] - r[2]) < 4, "Grayscale: каналы равны");
    }
    {
        const unsigned char* g = Px(Run("sepia", {{"intensity", 1.0f}}), gray, y);
        Check(g[0] > g[2] + 8, "Sepia: серое тёплое (красного больше синего)");
    }
    {   // Posterize на 2 уровня: только 0 и 1 (серое ~0.48 -> 0).
        const Image img = Run("posterize", {{"intensity", 1.0f}, {"levels", 2.0f}});
        Check(Px(img, gray, y)[0] < 10 && Px(img, red, y)[0] > 245, "Posterize: два уровня на канал");
    }
    {   // Pixelate: внутри блока 16 пикселей — один цвет.
        const Image img = Run("pixelate", {{"intensity", 1.0f}, {"size", 16.0f}});
        const int x0 = (bar / 16) * 16;
        bool same = true;
        for (int x = x0; x < x0 + 16; ++x)
            for (int c = 0; c < 3; ++c) same = same && Px(img, x, y)[c] == Px(img, x0, y)[c];
        Check(same, "Pixelate: блок 16×16 одного цвета");
    }
    {   // Blur: белая полоска растекается на соседей.
        const Image img = Run("blur", {{"intensity", 1.0f}, {"radius", 8.0f}});
        const int near = Px(img, bar + 6, y)[1];
        std::printf("    blur: рядом с полоской %d (было %d)\n", near, g0);
        Check(near > g0 + 10, "Blur: свет полоски уходит на соседние пиксели");
    }
    {   // Glow: яркое светится ореолом, тёмное не светится.
        const Image img = Run("glow", {{"intensity", 1.0f}, {"radius", 8.0f}, {"threshold", 0.5f}});
        std::printf("    glow: рядом с полоской %d (было %d)\n", Px(img, bar + 6, y)[1], g0);
        Check(Px(img, bar + 6, y)[1] > g0 + 10, "Glow: ореол вокруг яркой полоски");
        Check(std::abs(Px(img, gray, y)[1] - g0) < 6, "Glow: тёмное ниже порога не светится");
    }
    {   // Distortion: полоска уходит со своего столбца хоть в одной строке.
        const Image img = Run("distortion", {{"intensity", 1.0f}, {"amplitude", 5.0f}, {"frequency", 10.0f}});
        int moved = 0;
        for (int row = 0; row < kH; row += 4)
            if (Px(img, bar, row)[1] < 200) ++moved;
        Check(moved > 0, "Distortion: кадр колышется (полоска сходит с места)");
    }
    {   // Dissolve: на 1 кадр целиком в цвет (по умолчанию — чёрный).
        const Image all = Run("dissolve", {{"intensity", 1.0f}});
        const Image none = Run("dissolve", {{"intensity", 0.0f}});
        Check(Px(all, red, y)[0] < 10 && Px(all, gray, y)[0] < 10, "Dissolve на 1: кадр растворился целиком");
        Check(Px(none, red, y)[0] > 240, "Dissolve на 0: кадр целый");
    }
    {   // Слой объекта: эффект только под маской, остальное — как без слоя.
        const Image plain = RunObjectLayer(false);
        const Image layered = RunObjectLayer(true);
        const int inside = Px(layered, gray, y)[0], before = Px(plain, gray, y)[0];
        std::printf("    слой объекта: под маской %d (было %d)\n", inside, before);
        Check(std::abs(inside - (255 - before)) < 6, "Эффект объекта: под маской кадр инвертирован");
        bool same = true;
        for (int x = kW / 2 + 1; x < kW; x += 7)
            for (int c = 0; c < 3; ++c) same = same && Px(layered, x, y)[c] == Px(plain, x, y)[c];
        Check(same, "Эффект объекта: вне маски кадр не тронут");
    }
    {   // Сцена: эффект на объекте и потомке, не на соседе и не сквозь заслонку.
        ObjectScene os = BuildObjectScene();
        const glm::mat4 view = glm::lookAt(glm::vec3(0, 0.5f, 7), glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)kW / (float)kH, 0.1f, 100.0f);
        const Image plain = RenderObjectScene(*os.Scene_, view, proj);
        auto& fxc = os.Scene_->Registry().emplace<sage::render::ObjectEffectsComponent>(os.A.Entity());
        AddEffect(fxc.Effects, "invert");
        const Image layered = RenderObjectScene(*os.Scene_, view, proj);
        auto at = [&](glm::vec3 world) {   // мировая точка -> пиксель кадра (строки сверху вниз)
            const glm::vec4 c = proj * view * glm::vec4(world, 1.0f);
            return glm::ivec2((int)((c.x / c.w * 0.5f + 0.5f) * kW), (int)((0.5f - c.y / c.w * 0.5f) * kH));
        };
        auto diff = [&](glm::vec3 world) {
            const glm::ivec2 p = at(world);
            return std::abs((int)Px(layered, p.x, p.y)[1] - (int)Px(plain, p.x, p.y)[1]);
        };
        const int onA = diff({-1.5f, 0.25f, 0.5f}), onChild = diff({-1.5f, 1.6f, 0.4f});
        const int onB = diff({1.5f, 0.2f, 0.5f}), onShield = diff({-1.5f, -0.45f, 2.1f});
        std::printf("    сцена: A %d, потомок %d, сосед %d, заслонка %d\n", onA, onChild, onB, onShield);
        Check(onA > 60, "Эффект объекта: сам объект инвертирован");
        Check(onChild > 60, "Эффект объекта: потомок — вместе с родителем");
        Check(onB == 0, "Эффект объекта: сосед без эффекта не тронут");
        Check(onShield == 0, "Эффект объекта: заслонённое не меняется (заслонка цела)");
    }
}

} // namespace sage::rendertest
