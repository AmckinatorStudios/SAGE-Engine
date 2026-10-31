#include "sage/render/PostFXDetail.h"
#include "sage/render/PostFX.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/render/LensFlare.h"

using namespace sage::rhi;

// Звенья тракта пост-обработки и их помощники (см. PostFXDetail.h).
namespace sage::render::postfx {

GraphicsDevice& Dev(PostContext& ctx) { return *ctx.Device; }

// Полный экран: треугольник без вершинного буфера, рисуется тремя вершинами по
// gl_VertexID (см. kFsVert). Один и тот же для всех звеньев.
void DrawFullscreen(PostContext& ctx) { ctx.Scratch->Fullscreen->DrawArrays(3); }

// Сообщение, которое печатается ОДИН раз за процесс.
//
// Тракт исполняется КАЖДЫЙ кадр, и причина отказа у него одна и та же: без
// этой памяти лог получал бы шестьдесят одинаковых строк в секунду — ровно та
// беда, ради которой в движке уже заведено «печатать один раз» для ошибок
// скриптов. Здесь это тем важнее, что причина — свойство ТРАКТА, а не кадра.
void OnceLog(const std::string& message) {
    static std::vector<std::string> said;
    if (std::find(said.begin(), said.end(), message) != said.end()) return;
    said.push_back(message);
    LOG_WARN("PostFX") << message;
}

// Лениво создаёт буфер звена: полноразмерный HDR-таргет стоит заметной VRAM, а
// глубина резкости включается не в каждом проекте.
RenderTarget* Keep(std::unique_ptr<RenderTarget>& slot, int w, int h) {
    if (!slot) slot = MakeColor(std::max(1, w), std::max(1, h));
    return slot.get();
}

PostParamDesc Param(const char* name, const char* label, float def, float min, float max,
                    const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Float;
    p.Min = min;
    p.Max = max;
    p.Default[0] = def;
    p.Hint = hint ? hint : "";
    return p;
}

// Целое в диапазоне: число выборок, шагов — то, что дробным не бывает.
PostParamDesc IntParam(const char* name, const char* label, int def, int min, int max,
                       const char* hint) {
    PostParamDesc p = Param(name, label, (float)def, (float)min, (float)max, hint);
    p.Type = PostParamType::Int;
    return p;
}

// Цвет-множитель. Умолчание — белый: «ничего не менять» обязано быть видно
// глазом, а не вычисляться из нулей.
PostParamDesc ColorParam(const char* name, const char* label, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Color;
    p.Min = 0.0f;
    p.Max = 2.0f;
    p.Default[0] = p.Default[1] = p.Default[2] = p.Default[3] = 1.0f;
    p.Hint = hint ? hint : "";
    return p;
}

// Выбор из именованных вариантов: режим кривой, способ смешивания и прочее,
// где число само по себе не значит ничего.
PostParamDesc EnumParam(const char* name, const char* label, int def,
                        std::vector<std::string> options, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Enum;
    p.Min = 0.0f;
    p.Max = (float)(options.size() > 0 ? options.size() - 1 : 0);
    p.Default[0] = (float)def;
    p.Options = std::move(options);
    p.Hint = hint ? hint : "";
    return p;
}

// Галка: включить/выключить часть звена.
PostParamDesc BoolParam(const char* name, const char* label, bool def, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Bool;
    p.Min = 0.0f;
    p.Max = 1.0f;
    p.Default[0] = def ? 1.0f : 0.0f;
    p.Hint = hint ? hint : "";
    return p;
}

// Подпись-разделитель внутри списка параметров звена. Значения у неё нет:
// это способ сгруппировать настройки, не заводя вложенных структур.
PostParamDesc Heading(const char* label) {
    PostParamDesc p;
    p.Label = label;
    p.Type = PostParamType::Heading;
    return p;
}

// --- Звено «Ambient Occlusion» ----------------------------------------------
// Публикует карту postaux::kAO. И радиус, и сила принадлежат ЗВЕНУ: убрали
// звено — уехали оба, и возводить в степень стало некому (см. kAoBlurFrag).
void RunAmbientOcclusion(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("SSAO");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const glm::vec2 texel(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height);

    sc.Ao->Bind();
    Shader& ao = SsaoShader();
    ao.Use();
    ao.SetInt("uDepth", 0);
    device.BindTexture2D(0, ctx.SceneDepth);
    ao.SetMat4("uProj", ctx.Proj);
    ao.SetMat4("uInvProj", glm::inverse(ctx.Proj));
    ao.SetFloat("uRadius", e.Float("radius", 0.5f));
    ao.SetVec2("uTexel", texel);
    DrawFullscreen(ctx);

    // Размытие РАЗДЕЛЁННОЕ: горизонталь в AoBlur, вертикаль обратно в Ao. Два
    // прохода по 9 выборок вместо одного по 16 — и дешевле, и шире окно
    // (сглаживает шум выборки, который квадрат 4x4 оставлял).
    Shader& aob = AoBlurShader();
    aob.Use();
    aob.SetInt("uAO", 0);
    aob.SetVec2("uTexel", texel);
    aob.SetFloat("uStrength", glm::max(e.Float("strength", 1.0f), 0.01f));

    sc.AoBlur->Bind();
    aob.SetVec2("uDirection", glm::vec2(1.0f, 0.0f));
    aob.SetInt("uApplyStrength", 0);
    device.BindTexture2D(0, sc.Ao->ColorTextureHandle());
    DrawFullscreen(ctx);

    sc.Ao->Bind();
    aob.SetVec2("uDirection", glm::vec2(0.0f, 1.0f));
    aob.SetInt("uApplyStrength", 1);
    device.BindTexture2D(0, sc.AoBlur->ColorTextureHandle());
    DrawFullscreen(ctx);

    // Готовая карта лежит в Ao: вертикальный проход пишет обратно в него.
    // Карта ещё и публикуется — звену ИГРЫ она может понадобиться (см.
    // postaux), — но применяет её к кадру САМО звено: «эффект, который ничего
    // не меняет, пока его не подмешает кто-то другой», — это не звено тракта.
    PostPublishAux(ctx, postaux::kAO, sc.Ao->ColorTextureHandle());

    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& mul = MulShader();
    mul.Use();
    mul.SetInt("uScene", 0);
    mul.SetInt("uMul", 1);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Ao->ColorTextureHandle());
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), ctx.ColorIsLdr);
}

// --- Звено «Global Illumination» (экранное) ----------------------------------
// Трассировка в половинном разрешении: отражённый свет — низкочастотный, и
// полное разрешение стоило бы вчетверо дороже без видимой разницы, а шум
// выборки всё равно гасит размытие.
void RunScreenSpaceGI(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("SSGI");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const int hw = std::max(1, ctx.Width / 2), hh = std::max(1, ctx.Height / 2);
    RenderTarget* trace = Keep(sc.GiTrace, hw, hh);
    RenderTarget* blur = Keep(sc.GiBlur, hw, hh);
    // Качество — число лучей и шагов на пиксель: цена звена растёт ровно с ним.
    static const int kRays[] = {4, 8, 12};
    static const int kSteps[] = {6, 8, 12};
    const int q = glm::clamp(e.Int("quality", 1), 0, 2);

    trace->Bind();
    Shader& gi = SsgiShader();
    gi.Use();
    gi.SetInt("uDepth", 0);
    gi.SetInt("uScene", 1);
    device.BindTexture2D(0, ctx.SceneDepth);
    device.BindTexture2D(1, ctx.Color);
    gi.SetMat4("uProj", ctx.Proj);
    gi.SetMat4("uInvProj", glm::inverse(ctx.Proj));
    gi.SetVec2("uTexel", glm::vec2(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height));
    gi.SetFloat("uRadius", glm::max(e.Float("radius", 3.0f), 0.05f));
    gi.SetFloat("uThickness", glm::max(e.Float("thickness", 0.5f), 0.01f));
    gi.SetInt("uRays", kRays[q]);
    gi.SetInt("uSteps", kSteps[q]);
    const bool hasG = ctx.SceneAlbedo.Valid() && ctx.SceneNormal.Valid();
    gi.SetInt("uHasGBuffer", hasG ? 1 : 0);
    gi.SetInt("uGNormal", 2);
    gi.SetMat4("uViewRot", ctx.View);
    if (hasG) device.BindTexture2D(2, ctx.SceneNormal);
    DrawFullscreen(ctx);

    Shader& b = SsgiBlurShader();
    b.Use();
    b.SetInt("uGI", 0);
    b.SetVec2("uTexel", glm::vec2(1.0f / (float)hw, 1.0f / (float)hh));
    blur->Bind();
    b.SetVec2("uDirection", glm::vec2(1.0f, 0.0f));
    device.BindTexture2D(0, trace->ColorTextureHandle());
    DrawFullscreen(ctx);
    trace->Bind();
    b.SetVec2("uDirection", glm::vec2(0.0f, 1.0f));
    device.BindTexture2D(0, blur->ColorTextureHandle());
    DrawFullscreen(ctx);

    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& apply = SsgiApplyShader();
    apply.Use();
    apply.SetInt("uScene", 0);
    apply.SetInt("uGI", 1);
    apply.SetFloat("uIntensity", glm::max(e.Float("intensity", 1.0f), 0.0f));
    apply.SetFloat("uSaturation", glm::clamp(e.Float("saturation", 1.0f), 0.0f, 2.0f));
    apply.SetInt("uHasGBuffer", hasG ? 1 : 0);
    apply.SetInt("uGAlbedo", 2);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, trace->ColorTextureHandle());
    if (hasG) device.BindTexture2D(2, ctx.SceneAlbedo);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), ctx.ColorIsLdr);
}

// --- Звено «Depth of Field» --------------------------------------------------
void RunDepthOfField(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Глубина резкости");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const int hw = std::max(1, ctx.Width / 2), hh = std::max(1, ctx.Height / 2);
    RenderTarget* prep = Keep(sc.DofPrep, hw, hh);
    RenderTarget* farT = Keep(sc.DofBlur, hw, hh);
    RenderTarget* nearA = Keep(sc.DofNearMaxA, hw, hh);
    RenderTarget* nearB = Keep(sc.DofNearMaxB, hw, hh);
    RenderTarget* nearT = Keep(sc.DofNear, hw, hh);
    RenderTarget* out = PostAcquireTarget(ctx, 1);

    const float maxRadius = glm::clamp(e.Float("maxRadius", 16.0f), 0.0f, 64.0f);
    const glm::mat4 invProj = glm::inverse(ctx.Proj);
    auto setCommon = [&](Shader& sh) {
        sh.SetInt("uDepth", 1);
        sh.SetMat4("uInvProj", invProj);
        sh.SetFloat("uFocus", glm::max(e.Float("focus", 10.0f), 0.01f));
        sh.SetInt("uAutoFocus", e.Int("focusMode", 0) == 1 ? 1 : 0);
        sh.SetFloat("uFocusRange", glm::max(e.Float("focusRange", 0.0f), 0.0f));
        sh.SetFloat("uAperture", glm::max(e.Float("aperture", 2.8f), 0.5f));
        sh.SetFloat("uFocalLength", glm::clamp(e.Float("focalLength", 85.0f), 1.0f, 1000.0f));
        sh.SetFloat("uNearScale", glm::max(e.Float("nearBlur", 1.0f), 0.0f));
        sh.SetFloat("uFarScale", glm::max(e.Float("farBlur", 1.0f), 0.0f));
        sh.SetFloat("uMaxRadius", maxRadius);
        sh.SetFloat("uFullHeight", (float)ctx.Height);
        device.BindTexture2D(1, ctx.SceneDepth);
    };
    static const int kSamples[] = {32, 64, 128};
    const int maxSamples = kSamples[glm::clamp(e.Int("quality", 1), 0, 2)];
    const glm::vec2 halfTexel(1.0f / (float)hw, 1.0f / (float)hh);

    // 1. Префильтр + знаковый круг.
    prep->Bind();
    Shader& dprep = DofPrepShader();
    dprep.Use();
    dprep.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    setCommon(dprep);
    dprep.SetVec2("uFullTexel", glm::vec2(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height));
    DrawFullscreen(ctx);

    // 2. Досягаемость ближнего плана: максимум по горизонтали, затем по вертикали.
    const int reach = glm::clamp((int)std::ceil(maxRadius * 0.5f), 1, 32);
    Shader& nmax = DofNearMaxShader();
    nmax.Use();
    nmax.SetInt("uSrc", 0);
    nmax.SetInt("uReach", reach);
    nearA->Bind();
    device.BindTexture2D(0, prep->ColorTextureHandle());
    nmax.SetVec2("uStep", glm::vec2(halfTexel.x, 0.0f));
    nmax.SetInt("uFromPrep", 1);
    DrawFullscreen(ctx);
    nearB->Bind();
    device.BindTexture2D(0, nearA->ColorTextureHandle());
    nmax.SetVec2("uStep", glm::vec2(0.0f, halfTexel.y));
    nmax.SetInt("uFromPrep", 0);
    DrawFullscreen(ctx);

    // 3–4. Дальний и ближний планы.
    Shader& gather = DofGatherShader();
    gather.Use();
    gather.SetInt("uPrep", 0);
    gather.SetInt("uNearMax", 1);
    gather.SetVec2("uTexel", halfTexel);
    gather.SetInt("uMaxSamples", maxSamples);
    device.BindTexture2D(0, prep->ColorTextureHandle());
    device.BindTexture2D(1, nearB->ColorTextureHandle());
    farT->Bind();
    gather.SetInt("uNear", 0);
    DrawFullscreen(ctx);
    nearT->Bind();
    gather.SetInt("uNear", 1);
    DrawFullscreen(ctx);

    // 5. Сведение.
    out->Bind();
    Shader& dcomp = DofCompositeShader();
    dcomp.Use();
    dcomp.SetInt("uScene", 0);
    dcomp.SetInt("uFar", 2);
    dcomp.SetInt("uNearLayer", 3);
    dcomp.SetInt("uShowFocus", e.Bool("showFocus", false) ? 1 : 0);
    dcomp.SetVec2("uHalfTexel", halfTexel);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(2, farT->ColorTextureHandle());
    device.BindTexture2D(3, nearT->ColorTextureHandle());
    setCommon(dcomp);
    DrawFullscreen(ctx);

    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Motion Blur» -----------------------------------------------------
void RunMotionBlur(PostContext& ctx, const PostEffect& e) {
    const bool useVelocity = ctx.Velocity.Valid();
    // С буфером скоростей история прошлого кадра НЕ нужна: её хранит проход
    // геометрии, у каждой сущности свою. Запасной путь без буфера сравнивает
    // матрицу этого кадра с матрицей прошлого и потому без истории невозможен.
    const bool useHistory = ctx.Scratch->HasPrevFrame;
    if (!useVelocity && !useHistory) {
        OnceLog("звено «Motion Blur» пропущено: нет ни буфера скоростей, ни прошлого кадра");
        return;
    }
    const float amount = e.Float("amount", 0.5f);
    if (amount <= 0.001f) return;

    SAGE_PROFILE("Смаз движения");
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& sh = MotionShader();
    sh.Use();
    sh.SetInt("uScene", 0);
    sh.SetInt("uDepth", 1);
    sh.SetInt("uVelocity", 2);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, ctx.SceneDepth);
    // Сэмплер обязан быть привязан всегда, даже когда путь не выбран:
    // непривязанный юнит на части драйверов читается как чёрная текстура,
    // а на части — как мусор из чужого прохода.
    device.BindTexture2D(2, useVelocity ? ctx.Velocity : ctx.SceneDepth);
    sh.SetInt("uUseVelocity", useVelocity ? 1 : 0);
    sh.SetMat4("uInvViewProj", glm::inverse(ctx.Proj * ctx.View));
    sh.SetMat4("uPrevViewProj", ctx.Scratch->PrevViewProj);
    sh.SetFloat("uAmount", amount);
    sh.SetInt("uSamples", glm::clamp(e.Int("samples", 12), 2, 32));
    sh.SetFloat("uMaxLen", glm::clamp(e.Float("maxLength", 5.0f), 0.1f, 25.0f) * 0.01f);
    DrawFullscreen(ctx);

    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Lens Flare»: блик солнца в объективе ----------------------------
// Раньше жил в НАСТРОЙКАХ ПРОЕКТА и рисовался мимо тракта камеры: выключатель
// компонента «Пост-обработка» его не гасил, а в редакторе его нельзя было ни
// включить, ни настроить. Теперь это звено, как все остальные.
void RunLensFlare(PostContext& ctx, const PostEffect& e) {
    if (!ctx.Lighting) {
        OnceLog("звено «Lens Flare» пропущено: кадру не передано освещение (PostFX::SetLighting)");
        return;
    }
    const float intensity = e.Float("intensity", 1.0f);
    if (intensity <= 0.0f) return;
    PostScratch& sc = *ctx.Scratch;
    if (!sc.Flare) sc.Flare = std::make_shared<LensFlare>();
    GraphicsDevice& device = Dev(ctx);

    // Копия кадра в свою цель, блик — поверх неё сложением.
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& copy = CopyShader();
    copy.Use();
    copy.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);

    LensFlareSettings s;
    s.Enabled = true;
    s.Intensity = intensity;
    s.Ghosts = glm::clamp(e.Int("ghosts", 6), 0, 12);
    s.GhostSpacing = e.Float("ghostSpacing", 0.32f);
    s.GhostSize = e.Float("ghostSize", 0.055f);
    s.ApertureBlades = glm::clamp(e.Int("blades", 6), 0, 12);
    s.Halo = e.Float("halo", 0.55f);
    s.HaloRadius = e.Float("haloRadius", 0.42f);
    s.Starburst = e.Float("starburst", 0.5f);
    s.Glare = e.Float("glare", 0.45f);
    s.Chroma = e.Float("chroma", 0.55f);
    s.Threshold = e.Float("threshold", 1.1f);
    s.Lights = std::max(e.Float("lights", 1.0f), 0.0f);
    s.LightSize = std::max(e.Float("lightSize", 0.03f), 0.001f);
    s.Screen = std::max(e.Float("screen", 0.6f), 0.0f);
    s.ScreenThreshold = std::max(e.Float("screenThreshold", 1.5f), 0.01f);
    // Видимость солнца — по кадру СЦЕНЫ, а не по уже обработанному: порог
    // «это солнце» задан в яркости сцены, до экспозиции.
    sc.Flare->Render(*out, ctx.SceneColor, ctx.SceneDepth, ctx.Width, ctx.Height, ctx.Proj,
                     ctx.View, *ctx.Lighting, s);
    // Блик возвращает тест глубины — тракт работает без него.
    device.SetDepthTest(false);
    device.SetBlend(false);
    device.SetViewport(0, 0, ctx.Width, ctx.Height);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Bloom» -----------------------------------------------------------
// Публикует карту postaux::kBloom. Порог И сила принадлежат звену: сила
// умножается на последнем проходе размытия, а не тем, кто это свечение
// подмешивает (см. kBlurFrag).
void RunBloom(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Bloom");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const int hw = sc.Bright->Width(), hh = sc.Bright->Height();

    sc.Bright->Bind();
    Shader& br = BrightShader();
    br.Use();
    br.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    br.SetFloat("uThreshold", e.Float("threshold", 1.0f));
    DrawFullscreen(ctx);

    Shader& blur = BlurShader();
    blur.Use();
    blur.SetInt("uTex", 0);
    // РАДИУС — шаг между выборками размытия. Он принадлежит звену: «свечение
    // шире» и «свечение ярче» — разные желания, и одной силой их не выразить.
    const float radius = glm::clamp(e.Float("radius", 1.0f), 0.25f, 4.0f);
    const glm::vec2 texel(radius / (float)hw, radius / (float)hh);
    const float intensity = e.Float("intensity", 0.55f);
    sage::rhi::TextureHandle src = sc.Bright->ColorTextureHandle();
    RenderTarget* dstA = sc.BloomA.get();
    RenderTarget* dstB = sc.BloomB.get();
    for (int i = 0; i < 2; ++i) {
        const bool last = (i == 1);
        dstA->Bind(); // горизонтальный
        device.BindTexture2D(0, src);
        blur.SetVec2("uDir", glm::vec2(texel.x, 0.0f));
        blur.SetInt("uApplyScale", 0);
        DrawFullscreen(ctx);
        dstB->Bind(); // вертикальный
        device.BindTexture2D(0, dstA->ColorTextureHandle());
        blur.SetVec2("uDir", glm::vec2(0.0f, texel.y));
        blur.SetFloat("uScale", intensity);
        blur.SetInt("uApplyScale", last ? 1 : 0);
        DrawFullscreen(ctx);
        src = dstB->ColorTextureHandle();
    }
    PostPublishAux(ctx, postaux::kBloom, src);

    // И СРАЗУ В КАДР. Раньше свечение только публиковалось, а складывал его с
    // кадром тон-маппинг — то есть выключенный тон-маппинг забирал свечение с
    // собой, а «сила свечения» жила в одном звене, а применялась в другом.
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& add = AddShader();
    add.Use();
    add.SetInt("uScene", 0);
    add.SetInt("uAdd", 1);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, src);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Простое звено: один полноэкранный проход по текущей картинке -----------
//
// Общее тело для всех звеньев, которым нужен ровно один проход: цель из пула,
// исходник нулевым юнитом, свои uniform-ы — и результат становится текущим
// кадром. Без него каждое такое звено повторяло бы восемь одинаковых строк, и
// ошибиться в них можно было бы по-разному.
void RunSimplePass(PostContext& ctx, Shader& shader, bool ldrOut,
                   const std::function<void(Shader&)>& setup) {
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    shader.Use();
    shader.SetInt("uScene", 0);
    setup(shader);
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), ldrOut);
}

// --- Звено «Exposure»: сколько света собрал кадр ----------------------------
void RunExposure(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Экспозиция");
    RunSimplePass(ctx, ExposureShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetFloat("uEV", e.Float("exposure", 0.0f));
    });
}

// --- Звено «Auto Exposure»: глаз привыкает к свету сцены -------------------
void RunAutoExposure(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Автоэкспозиция");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    for (auto& t : sc.Adapt)
        if (!t) t = MakeColor(1, 1);

    // Шаг времени — по часам тракта; первый кадр и длинный разрыв (свернули
    // окно, стояли на точке останова) не должны раскачивать экспозицию.
    float dt = sc.HasAdapt ? ctx.Time - sc.AdaptTime : 0.0f;
    dt = glm::clamp(dt, 0.0f, 0.25f);
    sc.AdaptTime = ctx.Time;

    const int prev = sc.AdaptIndex, next = 1 - sc.AdaptIndex;
    sc.Adapt[next]->Bind();
    Shader& meter = AutoExposureMeterShader();
    meter.Use();
    meter.SetInt("uScene", 0);
    meter.SetInt("uPrev", 1);
    meter.SetInt("uHasPrev", sc.HasAdapt ? 1 : 0);
    meter.SetFloat("uDt", dt);
    meter.SetFloat("uSpeedUp", e.Float("speedUp", 3.0f));
    meter.SetFloat("uSpeedDown", e.Float("speedDown", 1.0f));
    const float minEv = e.Float("minEV", -8.0f), maxEv = e.Float("maxEV", 10.0f);
    meter.SetFloat("uMinEV", glm::min(minEv, maxEv));
    meter.SetFloat("uMaxEV", glm::max(minEv, maxEv));
    meter.SetFloat("uLow", glm::clamp(e.Float("lowPercent", 10.0f), 0.0f, 99.0f) * 0.01f);
    meter.SetFloat("uHigh", glm::clamp(e.Float("highPercent", 90.0f), 1.0f, 100.0f) * 0.01f);
    meter.SetFloat("uBlack", e.Float("blackLevel", -10.0f));
    // «Без поправки» — адаптация, при которой кадр не меняется: 0.18·2^(c−a) = 1.
    meter.SetFloat("uNeutral", e.Float("compensation", 1.0f) + std::log2(0.18f));
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Adapt[prev]->ColorTextureHandle());
    DrawFullscreen(ctx);
    sc.AdaptIndex = next;
    sc.HasAdapt = true;

    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& apply = AutoExposureApplyShader();
    apply.Use();
    apply.SetInt("uScene", 0);
    apply.SetInt("uAdapted", 1);
    apply.SetFloat("uCompensation", e.Float("compensation", 1.0f));
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Adapt[next]->ColorTextureHandle());
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Color»: яркость, контраст, насыщенность, температура ------------
void RunColor(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Цвет");
    RunSimplePass(ctx, ColorShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetFloat("uBrightness", glm::max(e.Float("brightness", 1.0f), 0.0f));
        sh.SetFloat("uContrast", glm::max(e.Float("contrast", 1.0f), 0.01f));
        sh.SetFloat("uSaturation", glm::max(e.Float("saturation", 1.0f), 0.0f));
        sh.SetFloat("uTemperature", e.Float("temperature", 0.0f));
        sh.SetFloat("uTint", e.Float("tint", 0.0f));
    });
}

// --- Звено «Color Grading»: тени, средние, света ----------------------------
void RunGrading(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Цветокоррекция");
    auto rgb = [&e](const char* name) {
        const PostValue* v = e.Find(name);
        return v ? glm::vec3(v->V[0], v->V[1], v->V[2]) : glm::vec3(1.0f);
    };
    RunSimplePass(ctx, GradeShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetVec3("uShadows", rgb("shadows"));
        sh.SetVec3("uMidtones", rgb("midtones"));
        sh.SetVec3("uHighlights", rgb("highlights"));
    });
}

// --- Звено «Tonemapping»: HDR -> готовый к показу кадр ----------------------
// ЕДИНСТВЕННОЕ звено каталога, переводящее кадр из HDR в LDR (см. Tonemaps).
void RunTonemap(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Тон-маппинг");
    RunSimplePass(ctx, TonemapShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetInt("uMode", glm::clamp(e.Int("mode", 2), 0, 6));
        sh.SetFloat("uGamma", glm::max(e.Float("gamma", 2.2f), 0.01f));
        sh.SetInt("uOutput", glm::clamp(e.Int("output", 0), 0, 2));
    });
}

// --- Звено «Vignette» -------------------------------------------------------
void RunVignette(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Виньетка");
    RunSimplePass(ctx, VignetteShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetFloat("uIntensity", glm::clamp(e.Float("intensity", 0.35f), 0.0f, 1.0f));
        sh.SetFloat("uSmoothness", glm::clamp(e.Float("smoothness", 0.6f), 0.01f, 1.0f));
    });
}

// --- Звено «Chromatic Aberration» -------------------------------------------
void RunChromatic(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Хроматическая аберрация");
    RunSimplePass(ctx, ChromaticShader(), /*ldrOut=*/true, [&](Shader& sh) {
        // Сила без верхнего потолка параметра: 1.0 — разница увеличения 3%
        // кадра. Зажим только там, где масштаб выборки стал бы бессмысленным.
        sh.SetFloat("uAmount", glm::clamp(e.Float("amount", 0.3f) * 0.03f, 0.0f, 0.9f));
        sh.SetFloat("uStart", glm::clamp(e.Float("start", 0.2f), 0.0f, 0.99f));
        sh.SetFloat("uAspect", (float)ctx.Width / (float)std::max(ctx.Height, 1));
        sh.SetInt("uSamples", glm::clamp(e.Int("samples", 12), 3, 32));
    });
}

// --- Звено «Film Grain» -----------------------------------------------------
void RunGrain(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Зерно плёнки");
    RunSimplePass(ctx, GrainShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetVec2("uResolution", glm::vec2((float)ctx.Width, (float)ctx.Height));
        sh.SetFloat("uAmount", glm::clamp(e.Float("amount", 0.12f), 0.0f, 1.0f));
        sh.SetFloat("uSize", glm::clamp(e.Float("size", 1.5f), 0.5f, 8.0f));
        sh.SetFloat("uTime", ctx.Time);
    });
}

// --- Звено «FXAA» ------------------------------------------------------------
void RunFxaa(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("FXAA");
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& fxaa = FxaaShader();
    fxaa.Use();
    fxaa.SetInt("uTex", 0);
    fxaa.SetVec2("uTexel",
                 glm::vec2(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height));
    fxaa.SetFloat("uContrastThreshold", glm::max(e.Float("threshold", 0.0625f), 0.0f));
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/true);
}

} // namespace sage::render::postfx

namespace sage::render {

using namespace postfx;

RenderTarget* PostAcquireTarget(PostContext& ctx, int divisor) {
    PostScratch& sc = *ctx.Scratch;
    if (divisor < 1) divisor = 1;
    for (PostScratch::Pooled& entry : sc.Pool) {
        if (entry.Divisor != divisor) continue;
        // Цель, которую звено ЧИТАЕТ, брать нельзя: запись в неё затёрла бы вход.
        // Тракт линеен, поэтому этого одного правила достаточно — цель, бывшая
        // входом предыдущего звена, к этому моменту уже свободна.
        if (entry.Target->ColorTextureHandle() == ctx.Color) continue;
        return entry.Target.get();
    }
    // Свободных нет — заводим ещё одну. Так длина тракта не упирается в
    // заранее угаданное число буферов.
    PostScratch::Pooled fresh;
    fresh.Divisor = divisor;
    fresh.Target = MakeColor(std::max(1, sc.Width / divisor), std::max(1, sc.Height / divisor));
    sc.Pool.push_back(std::move(fresh));
    return sc.Pool.back().Target.get();
}

void PostDrawFullscreen(PostContext& ctx) { ctx.Scratch->Fullscreen->DrawArrays(3); }

void PostPublishAux(PostContext& ctx, const char* name, sage::rhi::TextureHandle texture) {
    if (!name) return;
    for (std::pair<std::string, sage::rhi::TextureHandle>& entry : ctx.Aux) {
        if (entry.first == name) {
            entry.second = texture;
            return;
        }
    }
    ctx.Aux.emplace_back(name, texture);
}

sage::rhi::TextureHandle PostAuxTexture(const PostContext& ctx, const char* name) {
    if (!name) return {};
    for (const std::pair<std::string, sage::rhi::TextureHandle>& entry : ctx.Aux)
        if (entry.first == name) return entry.second;
    return {};
}

void PostSetColor(PostContext& ctx, sage::rhi::TextureHandle texture, bool ldr) {
    ctx.Color = texture;
    ctx.ColorIsLdr = ldr;
}

// ============================================================================
//  Встроенные виды эффектов
// ============================================================================

} // namespace sage::render
