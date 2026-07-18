#pragma once
#include <memory>

#include <glm/glm.hpp>

#include "sage/rhi/Resources.h"
#include "sage/render/Framebuffer.h"

// ---------------------------------------------------------------------------
// PostFX — полный проход пост-обработки движка поверх HDR-буфера сцены:
//   SSAO (screen-space ambient occlusion) из depth-текстуры сцены
//   Bloom (свечение ярких участков: bright-pass + separable-размытие)
//   Финальный composite: экспозиция + ACES тон-маппинг + AO + bloom +
//   насыщенность/контраст + ВИНЬЕТКА + гамма.
// Владеет промежуточными буферами (ColorHDR) и встроенными шейдерами; консьюмер
// (редактор/игра) просто зовёт Render с цветом+глубиной сцены. Реюзабелен —
// один экземпляр на вьюпорт. Часть ЯДРА рендера, не зависит от конкретной игры.
// ---------------------------------------------------------------------------
namespace sage::render {

struct PostFXSettings {
    bool Enabled = true;

    // Тон-маппинг / цвет.
    float Exposure = 1.05f;
    float Gamma = 2.2f;
    float Saturation = 1.16f;
    float Contrast = 1.06f;

    // Bloom (свечение).
    bool BloomEnabled = true;
    float BloomThreshold = 1.0f;  // яркость, выше которой пиксель «светится»
    float BloomIntensity = 0.55f; // сила добавляемого свечения

    // SSAO (ambient occlusion).
    bool AOEnabled = true;
    float AORadius = 0.5f;    // радиус выборки в мировых единицах (вид-пространство)
    float AOStrength = 1.0f;  // 0 — выкл, 1 — норма, >1 — сильнее затемнение

    // Виньетка.
    float Vignette = 0.35f;   // 0 — выкл, затемнение к углам
};

class PostFX {
public:
    PostFX();
    PostFX(const PostFX&) = delete;
    PostFX& operator=(const PostFX&) = delete;

    // Прогоняет всю цепочку и пишет результат в ТЕКУЩИЙ привязанный буфер
    // (экран или FBO), в прямоугольник (outX,outY,outW,outH).
    //   sceneColor — HDR-цвет сцены (Framebuffer::ColorTexture)
    //   sceneDepth — depth-текстура сцены (Framebuffer::DepthTexture); 0 — без SSAO
    //   w,h        — размер буфера сцены (внутреннее разрешение прохода эффектов)
    //   proj       — проекция камеры (нужна SSAO для реконструкции позиций из глубины)
    // output != nullptr — composite пишется в этот Framebuffer (вьюпорт редактора);
    // output == nullptr — в экранный буфер, в прямоугольник (outX,outY,outW,outH).
    void Render(unsigned int sceneColor, unsigned int sceneDepth, int w, int h,
                const glm::mat4& proj, const PostFXSettings& s,
                Framebuffer* output, int outX, int outY, int outW, int outH);

private:
    void EnsureTargets(int w, int h);

    std::unique_ptr<sage::rhi::Geometry> m_fsTri; // полноэкранный треугольник (без атрибутов)

    // Промежуточные буферы (ColorHDR). AO — полноразмерный, bloom — половинный.
    std::unique_ptr<sage::rhi::RenderTarget> m_ao, m_aoBlur;
    std::unique_ptr<sage::rhi::RenderTarget> m_bright, m_bloomA, m_bloomB;
    int m_w = 0, m_h = 0;
};

} // namespace sage::render
