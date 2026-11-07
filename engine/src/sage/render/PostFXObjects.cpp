// ---------------------------------------------------------------------------
// ЭФФЕКТЫ ОТДЕЛЬНЫХ ОБЪЕКТОВ — наложение слоёв поверх готового кадра камеры.
//
// Слой — цепочка звеньев изображения (те же Invert, Blur, Glow, Dissolve…,
// что у камеры) и номер в маске кадра. Для каждого слоя: кадр копируется в
// свою цель, цепочка прогоняется по всему кадру, а результат смешивается с
// исходником ТОЛЬКО там, где маска показывает этот слой.
//
// Почему через маску, а не правкой шейдеров объектов. Путей отрисовки у
// движка несколько (инстансный, текстурный, прозрачный, свой шейдер
// материала), а свои шейдеры пишет игра: вшить эффект в каждый значило бы
// требовать того же от каждого шейдера игры. Маска работает с любым
// материалом и даёт объектам ровно тот же набор эффектов, что и экрану.
// Цена — эффект видит только кадр: растворение закрашивает объект цветом
// звена, а не открывает то, что за ним (для этого — прозрачность материала).
// ---------------------------------------------------------------------------
#include "sage/render/PostFX.h"
#include "sage/render/PostFXDetail.h"

#include "sage/core/Profiler.h"

namespace sage::render {

namespace {

using namespace postfx;

const char* kObjectCompositeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBase;     // кадр до слоя
uniform sampler2D uFx;       // кадр после цепочки слоя
uniform sampler2D uMask;     // номер слоя в красном канале (k/255)
uniform float uLayer;        // номер этого слоя / 255
void main() {
    vec3 base = texture(uBase, vUV).rgb;
    float m = texture(uMask, vUV).r;
    // Допуск — полшага восьмибитной маски: номер соседнего слоя не совпадёт.
    float hit = abs(m - uLayer) < (0.5 / 255.0) ? 1.0 : 0.0;
    FragColor = vec4(mix(base, texture(uFx, vUV).rgb, hit), 1.0);
}
)";

Shader& CompositeShader() {
    static Shader* s = new Shader(Shader::FromSource(kFsVert, kObjectCompositeFrag, "PostFX.ObjectComposite"));
    return *s;
}

} // namespace

void PostFX::ApplyObjectLayers(PostContext& ctx) {
    SAGE_PROFILE("Эффекты объектов");
    GraphicsDevice& device = *ctx.Device;
    // Слои — звенья ИЗОБРАЖЕНИЯ: им нужен готовый (LDR) кадр. Если тракт
    // камеры почему-то не дошёл до тон-маппинга, слои честно пропускаются.
    if (!ctx.ColorIsLdr) {
        OnceLog("эффекты объектов пропущены: кадр камеры не прошёл тон-маппинг");
        return;
    }
    if (!m_objectBase || m_objectBase->Width() != ctx.Width || m_objectBase->Height() != ctx.Height)
        m_objectBase = MakeColor(ctx.Width, ctx.Height);

    for (size_t i = 0; i < m_objectLayers->size(); ++i) {
        const PostChain& layer = (*m_objectLayers)[i];

        // 1) Исходник — в свою цель: звенья слоя берут цели из общего пула.
        m_objectBase->Bind();
        device.SetViewport(0, 0, ctx.Width, ctx.Height);
        Shader& copy = CopyShader();
        copy.Use();
        copy.SetInt("uScene", 0);
        device.BindTexture2D(0, ctx.Color);
        DrawFullscreen(ctx);
        const sage::rhi::TextureHandle base = m_objectBase->ColorTextureHandle();
        ctx.Color = base;

        // 2) Цепочка слоя по всему кадру — те же звенья, что у камеры.
        bool any = false;
        for (const PostEffect& effect : layer.Effects) {
            if (!effect.Enabled) continue;
            const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
            if (!kind || !kind->Run) continue;
            // Слой работает с готовым кадром: звеньям HDR-этапа (свечение
            // сцены, экспозиция) и звеньям глубины здесь не место.
            if (Has(kind->Needs, PostNeeds::HdrColor) || Has(kind->Needs, PostNeeds::Depth)) {
                OnceLog("звено «" + kind->Label + "» не работает как эффект объекта — пропущено");
                continue;
            }
            kind->Run(ctx, effect);
            any = true;
        }
        if (!any) continue;   // ctx.Color уже указывает на копию исходника

        // 3) Смешивание: результат слоя — только на пикселях объекта.
        RenderTarget* out = PostAcquireTarget(ctx);
        out->Bind();
        device.SetViewport(0, 0, ctx.Width, ctx.Height);
        Shader& mix = CompositeShader();
        mix.Use();
        mix.SetInt("uBase", 0);
        mix.SetInt("uFx", 1);
        mix.SetInt("uMask", 2);
        mix.SetFloat("uLayer", (float)(i + 1) / 255.0f);
        device.BindTexture2D(0, base);
        device.BindTexture2D(1, ctx.Color);
        device.BindTexture2D(2, m_objectMask);
        DrawFullscreen(ctx);
        PostSetColor(ctx, out->ColorTextureHandle(), true);
    }
}

} // namespace sage::render
