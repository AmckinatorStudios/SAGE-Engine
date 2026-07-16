#pragma once
#include <memory>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------
// Настройки прохода пост-процессинга (см. assets/shaders/post.frag). Все
// значения — обычные uniform'ы, заливаемые каждый кадр игрой, поэтому она
// может менять картинку без правки движка.
// ---------------------------------------------------------------------
struct PostProcessSettings {
    float Exposure = 1.05f;         // экспозиция HDR перед тон-маппингом
    float Gamma = 2.2f;             // гамма-коррекция (sRGB ~2.2)
    float Saturation = 1.16f;       // 1.0 — без изменений, >1 — сочнее, 0 — ч/б
    float Contrast = 1.06f;         // 1.0 — без изменений
    float VignetteStrength = 0.35f; // 0 — выкл, затемнение к углам экрана
    bool Enabled = true;
};

// ---------------------------------------------------------------------
// PostProcess — владеет полноэкранным треугольником и рисует его. HDR-текстура
// цвета сцены (из Framebuffer) сэмплируется post.frag: тон-маппинг HDR->LDR,
// гамма-коррекция, виньетка/насыщенность/контраст — результат пишется в
// текущий привязанный буфер (в обычном режиме — экранный).
//
// Вызывающий сам создаёт post-шейдер и выставляет его uniform'ы — этот класс
// отвечает только за геометрию прохода. Часть ЯДРА рендера.
// ---------------------------------------------------------------------
class PostProcess {
public:
    PostProcess() {
        // Геометрия без атрибутов: вершины полноэкранного треугольника
        // генерируются в post.vert из gl_VertexID, буфер не нужен (но объект
        // геометрии нужен — draw-вызов без него невозможен в core-профилях).
        m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(sage::rhi::VertexLayout{});
    }

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    PostProcess(PostProcess&&) noexcept = default;
    PostProcess& operator=(PostProcess&&) noexcept = default;

    // Рисует полноэкранный проход. Вызывающий уже сделал shader.Use(),
    // выставил его uniform'ы и привязал текстуру сцены к юниту 0.
    void Draw() const {
        // Полноэкранный треугольник накрывает весь кадр — глубина не нужна
        // (и не должна мешать), временно выключаем depth-тест.
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetDepthTest(false);
        m_geometry->DrawArrays(3);
        device.SetDepthTest(true);
    }

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
};
