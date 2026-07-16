#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "sage/core/Log.h"

// ---------------------------------------------------------------------
// ShadowMap — карта теней от направленного света ("солнца"). Сначала сцена
// рисуется по глубине из точки зрения солнца в depth-текстуру (проход
// теней), затем основные шейдеры сэмплируют её, чтобы решить, находится ли
// фрагмент в тени. Мягкие края даёт PCF-фильтрация в шейдере-приёмнике.
//
// Свет направленный (ортографическая проекция — параллельные лучи), поэтому
// "камера света" — это ортобокс, накрывающий интересующую часть мира
// (корабль + вода вокруг). Центр и радиус бокса передаются каждый кадр.
//
// Часть ЯДРА рендера — не зависит от вокселей/The Boat.
//
// Использование за кадр:
//   shadows.SetLightMatrix(sun.Direction, shipCenter, radius);
//   shadows.BeginRender();
//   depthShader.Use(); depthShader.SetMat4("uLightSpace", shadows.LightMatrix());
//   ...рисуем отбрасывающую тень геометрию (uModel + Draw)...
//   shadows.EndRender(window.Width(), window.Height());
//   ...в основном проходе: bind shadows.DepthTexture() + uLightSpace...
// ---------------------------------------------------------------------
class ShadowMap {
public:
    explicit ShadowMap(int resolution = 2048) : m_resolution(resolution) {
        glGenFramebuffers(1, &m_fbo);

        glGenTextures(1, &m_depthTex);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, resolution, resolution, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // За пределами ортобокса солнца считаем "полностью освещено":
        // граничный тексель глубины = 1.0 (максимально далеко), поэтому
        // сравнение глубины никогда не даст тень для геометрии вне бокса.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTex, 0);
        // Только глубина, без цвета — явно отключаем чтение/запись цвета,
        // иначе FBO без color-attachment считается неполным.
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("ShadowMap") << "Buffer карты теней неполный (incomplete)";
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    ~ShadowMap() {
        if (m_depthTex) glDeleteTextures(1, &m_depthTex);
        if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    }

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // Строит матрицу вида-проекции света: ортобокс с полустороной radius,
    // отцентрованный на center. sunDir — направление, КУДА летит свет
    // (env.Sun.Direction).
    void SetLightMatrix(glm::vec3 sunDir, glm::vec3 center, float radius) {
        glm::vec3 dir = glm::normalize(sunDir);
        // Безопасный up: когда солнце почти в зените (dir ~ вертикаль),
        // обычный up (0,1,0) вырождается — берём горизонтальный.
        glm::vec3 up = (glm::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                 : glm::vec3(0.0f, 1.0f, 0.0f);
        // "Камеру света" отодвигаем назад по лучу, чтобы видеть весь бокс
        // снаружи; far берём с запасом на высоту геометрии над центром.
        glm::vec3 eye = center - dir * (radius * 2.0f);
        glm::mat4 lightView = glm::lookAt(eye, center, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 4.0f);
        m_lightMatrix = lightProj * lightView;
    }

    void BeginRender() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, m_resolution, m_resolution);
        glClear(GL_DEPTH_BUFFER_BIT);
        // Отсечение ЛИЦЕВЫХ граней в проходе глубины (вместо задних):
        // классический приём против shadow acne — в карту пишется задняя
        // стенка объекта, а лицевую (освещённую) сравнение уже не самозатеняет.
        // Работает для замкнутых тел (воксельные кубы, меши-кубы, .obj).
        glCullFace(GL_FRONT);
    }

    void EndRender(int screenW, int screenH) {
        glCullFace(GL_BACK);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, screenW, screenH);
    }

    unsigned int DepthTexture() const { return m_depthTex; }
    const glm::mat4& LightMatrix() const { return m_lightMatrix; }
    int Resolution() const { return m_resolution; }

private:
    unsigned int m_fbo = 0, m_depthTex = 0;
    int m_resolution;
    glm::mat4 m_lightMatrix{1.0f};
};
