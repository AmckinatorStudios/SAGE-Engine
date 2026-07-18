#pragma once
#include <memory>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------
// Framebuffer — offscreen render-таргет для пост-процессинга. Сцена рисуется
// не в экранный буфер напрямую, а сюда: float-цвет (HDR — значения не
// обрезаются в [0,1], что нужно честному тон-маппингу) + depth-буфер. После
// прохода сцены текстура цвета сэмплируется проходом PostProcess.
//
// Тонкая обёртка над rhi::RenderTarget (вид ColorHDRWithDepth) — о графическом
// API ничего не знает. Часть ЯДРА рендера, не зависит от конкретной игры.
//
// Использование:
//   Framebuffer sceneFbo(w, h);
//   ...каждый кадр...
//   sceneFbo.Resize(window.Width(), window.Height()); // no-op, если не менялось
//   sceneFbo.Bind();
//   ...рисуем всю 3D-сцену...
//   device.BindDefaultFramebuffer(); // вернулись в экранный буфер
//   ...PostProcess по sceneFbo.ColorTexture()...
// ---------------------------------------------------------------------
class Framebuffer {
public:
    Framebuffer(int width, int height) {
        sage::rhi::RenderTargetDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Kind = sage::rhi::RenderTargetKind::ColorHDRWithDepth;
        m_target = sage::rhi::GraphicsDevice::Get().CreateRenderTarget(desc);
    }

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&&) noexcept = default;
    Framebuffer& operator=(Framebuffer&&) noexcept = default;

    // Делает таргет активным и выставляет viewport под его размер.
    void Bind() const { m_target->Bind(); }

    // Пересоздаёт хранилище под новый размер (no-op, если не изменился) —
    // безопасно звать каждый кадр.
    void Resize(int width, int height) { m_target->Resize(width, height); }

    // Нативный хендл текстуры цвета — для сэмплирования проходом пост-процесса
    // (GraphicsDevice::BindTexture2D) и показа в ImGui (viewport редактора).
    unsigned int ColorTexture() const { return m_target->ColorTextureHandle(); }
    // Нативный хендл depth-текстуры сцены — для SSAO/пост-эффектов, читающих глубину.
    unsigned int DepthTexture() const { return m_target->DepthTextureHandle(); }
    int Width() const { return m_target->Width(); }
    int Height() const { return m_target->Height(); }

private:
    std::unique_ptr<sage::rhi::RenderTarget> m_target;
};
