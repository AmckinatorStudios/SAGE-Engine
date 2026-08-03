#pragma once
#include <algorithm>
#include <memory>
#include <optional>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------
// Framebuffer — offscreen render-таргет для пост-процессинга. Сцена рисуется
// не в экранный буфер напрямую, а сюда: float-цвет (HDR — значения не
// обрезаются в [0,1], что нужно честному тон-маппингу) + depth-буфер. После
// прохода сцены текстура цвета сэмплируется цепочкой PostFX.
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
//   ...PostFX::Render по sceneFbo.ColorTexture()/DepthTexture()...
// ---------------------------------------------------------------------
class Framebuffer {
public:
    // samples > 1 — сглаживание кромок средствами растеризатора (MSAA). Рисовать
    // в такой буфер можно как обычно, но ПЕРЕД чтением ColorTexture/DepthTexture
    // надо вызвать Resolve: наружу отдаются обычные текстуры, а не многосэмпловые.
    explicit Framebuffer(int width, int height, int samples = 1) {
        sage::rhi::RenderTargetDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Kind = sage::rhi::RenderTargetKind::ColorHDRWithDepth;
        desc.Samples = samples;
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

    // Переносит многосэмпловое содержимое в обычные текстуры. Без MSAA — пустышка.
    // Зовётся один раз после всей отрисовки в этот буфер и до первого чтения.
    void Resolve() const { m_target->Resolve(); }
    // Сколько сэмплов реально выделено (1 — MSAA нет или недоступен).
    int Samples() const { return m_target->Samples(); }

    // Хендл текстуры цвета — для сэмплирования проходом пост-процесса
    // (GraphicsDevice::BindTexture2D).
    sage::rhi::TextureHandle ColorTexture() const { return m_target->ColorTextureHandle(); }
    // Хендл depth-текстуры сцены — для SSAO/пост-эффектов, читающих глубину.
    sage::rhi::TextureHandle DepthTexture() const { return m_target->DepthTextureHandle(); }
    // Сырое число для ImGui (viewport редактора) — см. Texture2D::NativeHandle.
    uint64_t NativeColorTexture() const { return m_target->NativeColorHandle(); }
    int Width() const { return m_target->Width(); }
    int Height() const { return m_target->Height(); }

private:
    std::unique_ptr<sage::rhi::RenderTarget> m_target;
};

// Приводит буфер к нужным размеру И числу сэмплов.
//
// ЗАЧЕМ ОТДЕЛЬНАЯ ФУНКЦИЯ, А НЕ ПРОСТО Resize. Размер вложений меняется на
// месте, а число сэмплов задаётся при СОЗДАНИИ хранилища — поменять его у
// живого таргета нельзя. Настройка же правится на лету (ползунок MSAA в
// настройках редактора), и без пересоздания она молча не действовала бы до
// перезапуска. Пересоздание происходит только когда число сэмплов реально
// изменилось: это не покадровая операция.
inline void EnsureFramebuffer(std::optional<Framebuffer>& fbo, int width, int height,
                              int samples = 1) {
    if (width <= 0 || height <= 0) return;
    samples = std::max(samples, 1);
    if (fbo && fbo->Samples() != samples) fbo.reset();
    if (!fbo) fbo.emplace(width, height, samples);
    else fbo->Resize(width, height);
}
