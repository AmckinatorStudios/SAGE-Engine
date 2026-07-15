#include "UIRenderer.h"
#include "stb_easy_font.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace {
    // Временный буфер под геометрию одного вызова Text (stb_easy_font пишет
    // сюда сырой поток вершин, который мы затем масштабируем и переносим
    // в общий буфер кадра).
    constexpr int kTextScratchBytes = 64 * 1024;
    char g_textScratch[kTextScratchBytes];
}

UIRenderer::UIRenderer()
    : m_shader(ShaderPaths::DebugTextVert, ShaderPaths::DebugTextFrag) {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // Формат вершины совпадает с stb_easy_font: 3 float позиции + 4 байта цвета
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UIVertex), (void*)12);

    glBindVertexArray(0);
}

UIRenderer::~UIRenderer() {
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void UIRenderer::Begin(int screenWidth, int screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_vertices.clear();
    m_quadCount = 0;
}

void UIRenderer::PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha) {
    unsigned char r = static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    unsigned char g = static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    unsigned char b = static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    unsigned char a = static_cast<unsigned char>(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f);

    m_vertices.push_back({x,     y,     0.0f, r, g, b, a});
    m_vertices.push_back({x + w, y,     0.0f, r, g, b, a});
    m_vertices.push_back({x + w, y + h, 0.0f, r, g, b, a});
    m_vertices.push_back({x,     y + h, 0.0f, r, g, b, a});
    ++m_quadCount;
}

void UIRenderer::Rect(float x, float y, float w, float h, glm::vec3 color, float alpha) {
    PushQuad(x, y, w, h, color, alpha);
}

void UIRenderer::RectOutline(float x, float y, float w, float h, float t, glm::vec3 color, float alpha) {
    PushQuad(x, y, w, t, color, alpha);              // верх
    PushQuad(x, y + h - t, w, t, color, alpha);      // низ
    PushQuad(x, y + t, t, h - 2 * t, color, alpha);  // лево
    PushQuad(x + w - t, y + t, t, h - 2 * t, color, alpha); // право
}

void UIRenderer::Text(float x, float y, float scale, glm::vec3 color, const std::string& text) {
    if (text.empty()) return;

    unsigned char rgba[4] = {
        static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        255
    };

    int quads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(text.c_str()), rgba,
                                     g_textScratch, kTextScratchBytes);

    // stb пишет вершины в "своих" пикселях от (0,0) — масштабируем и
    // переносим в экранные координаты, добавляя в общий буфер кадра.
    const char* src = g_textScratch;
    for (int q = 0; q < quads; ++q) {
        for (int v = 0; v < 4; ++v) {
            const float* pos = reinterpret_cast<const float*>(src);
            const unsigned char* col = reinterpret_cast<const unsigned char*>(src + 12);
            m_vertices.push_back({
                x + pos[0] * scale,
                y + pos[1] * scale,
                0.0f,
                col[0], col[1], col[2], col[3]
            });
            src += 16;
        }
        ++m_quadCount;
    }
}

float UIRenderer::MeasureText(const std::string& text, float scale) {
    if (text.empty()) return 0.0f;
    return stb_easy_font_width(const_cast<char*>(text.c_str())) * scale;
}

void UIRenderer::TextCentered(float centerX, float y, float scale, glm::vec3 color, const std::string& text) {
    Text(centerX - MeasureText(text, scale) * 0.5f, y, scale, color, text);
}

void UIRenderer::EnsureIndexCapacity(size_t quadCount) {
    if (m_indexCapacity >= quadCount * 6) return;
    std::vector<unsigned int> indices;
    indices.reserve(quadCount * 6);
    for (size_t q = 0; q < quadCount; ++q) {
        unsigned int base = static_cast<unsigned int>(q * 4);
        indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_DYNAMIC_DRAW);
    m_indexCapacity = indices.size();
}

void UIRenderer::End() {
    if (m_quadCount == 0) return;

    glDisable(GL_DEPTH_TEST);
    // В ortho-проекции с перевёрнутой осью Y (0 сверху) обход вершин квадов
    // становится CW — backface culling движка отсёк бы весь интерфейс
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glm::mat4 proj = glm::ortho(0.0f, (float)m_screenWidth, (float)m_screenHeight, 0.0f, -1.0f, 1.0f);
    m_shader.Use();
    m_shader.SetMat4("uProjection", proj);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(UIVertex), m_vertices.data(), GL_DYNAMIC_DRAW);

    EnsureIndexCapacity(m_quadCount);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_quadCount * 6), GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}
