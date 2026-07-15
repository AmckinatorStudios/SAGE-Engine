#include "DebugOverlay.h"
#include "stb_easy_font.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace {
    // Один общий буфер под геометрию всех строк кадра — с запасом на
    // разумное число символов HUD'а (каждый символ — до пары quad'ов).
    constexpr int kVertexBufferBytes = 128 * 1024;
    char g_vertexScratch[kVertexBufferBytes];

    constexpr float kLineHeight = 12.0f; // строки stb_easy_font идут с шагом 12 "своих" пикселей
    constexpr float kScreenScale = 2.0f; // на скольких экранных пикселей растягиваем 1 "свой" пиксель шрифта
}

DebugOverlay::DebugOverlay()
    : m_shader(ShaderPaths::DebugTextVert, ShaderPaths::DebugTextFrag) {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // stb_easy_font вершина: 3 float (x,y,z) + 4 байта цвета = 16 байт
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 16, (void*)12);

    glBindVertexArray(0);
}

DebugOverlay::~DebugOverlay() {
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void DebugOverlay::Draw(const std::vector<DebugLine>& lines, int screenWidth, int screenHeight) {
    if (lines.empty()) return;

    int totalQuads = 0;
    int offsetBytes = 0;
    float y = 4.0f;

    for (const DebugLine& line : lines) {
        unsigned char color[4] = {
            static_cast<unsigned char>(glm::clamp(line.Color.r, 0.0f, 1.0f) * 255.0f),
            static_cast<unsigned char>(glm::clamp(line.Color.g, 0.0f, 1.0f) * 255.0f),
            static_cast<unsigned char>(glm::clamp(line.Color.b, 0.0f, 1.0f) * 255.0f),
            255
        };
        int remaining = kVertexBufferBytes - offsetBytes;
        if (remaining <= 0) break; // буфер кончился — обрезаем HUD, а не падаем

        int quads = stb_easy_font_print(4.0f, y, const_cast<char*>(line.Text.c_str()), color,
                                         g_vertexScratch + offsetBytes, remaining);
        totalQuads += quads;
        offsetBytes += quads * 4 * 16;
        y += kLineHeight;
    }
    if (totalQuads == 0) return;

    // Индексы для превращения quad'ов stb_easy_font (4 вершины) в треугольники
    // (GL_QUADS в core-профиле нет) — пересобираем, только если строк стало больше.
    static std::vector<unsigned int> indices;
    if ((int)indices.size() < totalQuads * 6) {
        indices.clear();
        indices.reserve(totalQuads * 6);
        for (int q = 0; q < totalQuads; ++q) {
            unsigned int base = static_cast<unsigned int>(q * 4);
            indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_DYNAMIC_DRAW);
        m_indexCapacity = indices.size();
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); // квады текста в перевёрнутом ortho идут CW
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Масштабируем экранные координаты так, чтобы мелкий векторный шрифт
    // stb_easy_font (~7px высота глифа) читался на HiDPI/телефонных экранах.
    glm::mat4 proj = glm::ortho(0.0f, screenWidth / kScreenScale, screenHeight / kScreenScale, 0.0f, -1.0f, 1.0f);

    m_shader.Use();
    m_shader.SetMat4("uProjection", proj);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, offsetBytes, g_vertexScratch, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    glDrawElements(GL_TRIANGLES, totalQuads * 6, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}
