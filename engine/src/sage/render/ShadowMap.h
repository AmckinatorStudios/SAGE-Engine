#pragma once
#include <cmath>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------
// ShadowMap — карта теней от направленного света ("солнца"). Сначала сцена
// рисуется по глубине из точки зрения солнца в depth-текстуру (проход
// теней), затем основные шейдеры сэмплируют её, чтобы решить, находится ли
// фрагмент в тени. Мягкие края даёт PCF-фильтрация в шейдере-приёмнике.
//
// Свет направленный (ортографическая проекция — параллельные лучи), поэтому
// "камера света" — это ортобокс, накрывающий интересующую часть мира.
// Центр и радиус бокса передаются каждый кадр.
//
// Тонкая обёртка над rhi::RenderTarget (вид DepthOnly) + матрица света.
// Часть ЯДРА рендера — не зависит от конкретной игры.
//
// Использование за кадр:
//   shadows.SetLightMatrix(sun.Direction, center, radius);
//   shadows.BeginRender();
//   depthShader.Use(); depthShader.SetMat4("uLightSpace", shadows.LightMatrix());
//   ...рисуем отбрасывающую тень геометрию (uModel + Draw)...
//   shadows.EndRender(window.Width(), window.Height());
//   ...в основном проходе: BindTexture2D(1, shadows.DepthTexture()) + uLightSpace...
// ---------------------------------------------------------------------
class ShadowMap {
public:
    explicit ShadowMap(int resolution = 2048) : m_resolution(resolution) {
        sage::rhi::RenderTargetDesc desc;
        desc.Width = resolution;
        desc.Height = resolution;
        desc.Kind = sage::rhi::RenderTargetKind::DepthOnly;
        m_target = sage::rhi::GraphicsDevice::Get().CreateRenderTarget(desc);
    }

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;
    ShadowMap(ShadowMap&&) noexcept = default;
    ShadowMap& operator=(ShadowMap&&) noexcept = default;

    // Строит матрицу вида-проекции света: ортобокс с полустороной radius,
    // отцентрованный на center. sunDir — направление, КУДА летит свет.
    void SetLightMatrix(glm::vec3 sunDir, glm::vec3 center, float radius) {
        glm::vec3 dir = glm::normalize(sunDir);
        // Безопасный up: когда солнце почти в зените (dir ~ вертикаль),
        // обычный up (0,1,0) вырождается — берём горизонтальный.
        glm::vec3 up = (glm::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                 : glm::vec3(0.0f, 1.0f, 0.0f);

        // ПРИВЯЗКА К СЕТКЕ ТЕКСЕЛЕЙ.
        //
        // Ортобокс двигается вслед за сценой, и без привязки его край каждый
        // кадр попадает между текселями. Тень при этом «кипит»: одни и те же
        // кромки перескакивают на тексель туда-обратно, и в движении это
        // заметно сильнее, чем сама пикселизация. Лечится тем, что центр бокса
        // округляется до целого числа текселей — тогда содержимое карты
        // сдвигается ровно на тексель, а не на его долю.
        const float texelWorld = (radius * 2.0f) / (float)m_resolution;
        {
            const glm::mat4 view = glm::lookAt(center - dir, center, up);
            glm::vec3 inLight = glm::vec3(view * glm::vec4(center, 1.0f));
            inLight.x = std::floor(inLight.x / texelWorld) * texelWorld;
            inLight.y = std::floor(inLight.y / texelWorld) * texelWorld;
            center = glm::vec3(glm::inverse(view) * glm::vec4(inLight, 1.0f));
        }

        // "Камеру света" отодвигаем назад по лучу, чтобы видеть весь бокс
        // снаружи; far берём с запасом на высоту геометрии над центром.
        glm::vec3 eye = center - dir * (radius * 2.0f);
        glm::mat4 lightView = glm::lookAt(eye, center, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 4.0f);
        m_lightMatrix = lightProj * lightView;
    }

    // Размер текселя карты в метрах — по нему видно, откуда берётся качество:
    // вдвое меньший радиус даёт вдвое более мелкий тексель при том же
    // разрешении. Открыто наружу ради диагностики и проверок.
    float TexelWorldSize(float radius) const { return (radius * 2.0f) / (float)m_resolution; }
    int Resolution() const { return m_resolution; }

    void BeginRender() {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        m_target->Bind(); // FBO + viewport под разрешение карты
        device.Clear(/*color=*/false, /*depth=*/true);
        // Отсечение ЛИЦЕВЫХ граней в проходе глубины (вместо задних):
        // классический приём против shadow acne — в карту пишется задняя
        // стенка объекта, а лицевую (освещённую) сравнение уже не самозатеняет.
        // Работает для замкнутых тел (воксельные кубы, меши-кубы, .obj).
        device.SetCullMode(sage::rhi::CullMode::Front);
    }

    void EndRender(int screenW, int screenH) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetCullMode(sage::rhi::CullMode::Back);
        device.BindDefaultFramebuffer();
        device.SetViewport(0, 0, screenW, screenH);
    }

    // Нативный хендл depth-текстуры — для привязки шейдерам-приёмникам
    // через GraphicsDevice::BindTexture2D.
    unsigned int DepthTexture() const { return m_target->DepthTextureHandle(); }
    const glm::mat4& LightMatrix() const { return m_lightMatrix; }

private:
    std::unique_ptr<sage::rhi::RenderTarget> m_target;
    int m_resolution;
    glm::mat4 m_lightMatrix{1.0f};
};
