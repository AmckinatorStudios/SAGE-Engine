#pragma once
#include <glad/glad.h>
#include "sage/core/Log.h"

// ---------------------------------------------------------------------
// Framebuffer — offscreen render-таргет для пост-процессинга. Сцена рисуется
// не в экранный буфер напрямую, а сюда: в float-текстуру цвета (GL_RGBA16F,
// чтобы у тон-маппинга/экспозиции был настоящий HDR-диапазон, а не
// обрезанные в [0,1] значения) плюс depth-renderbuffer для корректного
// перекрытия по глубине. После прохода сцены текстура цвета скармливается
// PostProcess, который рисует полноэкранный треугольник в экранный буфер.
//
// Часть ЯДРА рендера — не зависит от вокселей/The Boat.
//
// Использование:
//   Framebuffer sceneFbo(w, h);
//   ...каждый кадр...
//   sceneFbo.Resize(window.Width(), window.Height()); // no-op если не менялось
//   sceneFbo.Bind();
//   ...рисуем всю 3D-сцену...
//   glBindFramebuffer(GL_FRAMEBUFFER, 0); // вернулись в экранный буфер
//   ...PostProcess по sceneFbo.ColorTexture()...
// ---------------------------------------------------------------------
class Framebuffer {
public:
    Framebuffer(int width, int height) { Create(width, height); }
    ~Framebuffer() { Destroy(); }

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Делает этот FBO активным и выставляет viewport под его размер
    void Bind() const {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, m_width, m_height);
    }

    // Пересоздаёт хранилище цвета и глубины под новый размер окна.
    // No-op, если размер не изменился, поэтому безопасно звать каждый кадр.
    void Resize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        if (width == m_width && height == m_height) return;
        m_width = width;
        m_height = height;
        glBindTexture(GL_TEXTURE_2D, m_colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }

    unsigned int ColorTexture() const { return m_colorTex; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

private:
    void Create(int width, int height) {
        m_width = width;
        m_height = height;

        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

        // Цвет — float-текстура (HDR). LINEAR-фильтр, чтобы при небольших
        // рассинхронах размера полноэкранная выборка не давала ступенек.
        glGenTextures(1, &m_colorTex);
        glBindTexture(GL_TEXTURE_2D, m_colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);

        // Глубина — renderbuffer (её не нужно читать, только тестировать),
        // это дешевле и проще, чем depth-текстура.
        glGenRenderbuffers(1, &m_depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Framebuffer") << "Offscreen-буфер сцены неполный (incomplete)";
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Destroy() {
        if (m_colorTex) glDeleteTextures(1, &m_colorTex);
        if (m_depthRbo) glDeleteRenderbuffers(1, &m_depthRbo);
        if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    }

    unsigned int m_fbo = 0, m_colorTex = 0, m_depthRbo = 0;
    int m_width = 0, m_height = 0;
};
