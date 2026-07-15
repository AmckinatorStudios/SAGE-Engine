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
    : m_shader(ShaderPaths::DebugTextVert, ShaderPaths::DebugTextFrag),
      m_spriteShader("assets/shaders/ui_sprite.vert", "assets/shaders/ui_sprite.frag") {
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

    // Спрайты: динамический квад (позиция xy + uv), 4 вершины, перезаливается
    // на каждый спрайт (их немного — иконки/картинки, не тысячи).
    glGenVertexArrays(1, &m_spriteVao);
    glGenBuffers(1, &m_spriteVbo);
    glBindVertexArray(m_spriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_spriteVbo);
    glBufferData(GL_ARRAY_BUFFER, 4 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
}

UIRenderer::~UIRenderer() {
    if (m_spriteVbo) glDeleteBuffers(1, &m_spriteVbo);
    if (m_spriteVao) glDeleteVertexArrays(1, &m_spriteVao);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

void UIRenderer::Begin(int screenWidth, int screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_vertices.clear();
    m_quadCount = 0;
    m_commands.clear();
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
    NoteColoredQuad();
}

void UIRenderer::NoteColoredQuad() {
    // Продлеваем последнюю цветную команду или заводим новую (если перед этим
    // был спрайт) — так порядок слоёв соблюдается, а текстовые квады (их
    // добавляет Text напрямую) не выпадают из диапазонов отрисовки.
    if (!m_commands.empty() && m_commands.back().Kind == Command::Type::ColoredQuads) {
        ++m_commands.back().QuadCount;
    } else {
        Command c;
        c.Kind = Command::Type::ColoredQuads;
        c.FirstQuad = m_quadCount;
        c.QuadCount = 1;
        m_commands.push_back(c);
    }
    ++m_quadCount;
}

void UIRenderer::Sprite(float x, float y, float w, float h, const Texture& texture,
                        glm::vec4 tint, glm::vec4 uv) {
    Command c;
    c.Kind = Command::Type::Sprite;
    c.Tex = &texture;
    c.X = x; c.Y = y; c.W = w; c.H = h;
    c.Tint = tint; c.UV = uv;
    m_commands.push_back(c);
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
        NoteColoredQuad(); // тот же учёт, что и у Rect — иначе глифы вне команд
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

void UIRenderer::DrawColored(size_t firstQuad, size_t quadCount, const glm::mat4& proj) {
    m_shader.Use();
    m_shader.SetMat4("uProjection", proj);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    // Индексы глобальны (квад q -> вершины q*4..), поэтому подмассив [firstQuad..]
    // ссылается ровно на свои вершины в общем VBO кадра.
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(quadCount * 6), GL_UNSIGNED_INT,
                   (void*)(firstQuad * 6 * sizeof(unsigned int)));
}

void UIRenderer::DrawSprite(const Command& cmd, const glm::mat4& proj) {
    if (!cmd.Tex) return;
    float x = cmd.X, y = cmd.Y, w = cmd.W, h = cmd.H;
    float u0 = cmd.UV.x, v0 = cmd.UV.y, u1 = cmd.UV.z, v1 = cmd.UV.w;
    // Текстуры движка грузятся с флипом по вертикали (см. Texture), поэтому в UI
    // (0,0 сверху) верх спрайта берёт v1, низ — v0 — иначе иконка вверх ногами.
    float verts[16] = {
        x,     y,     u0, v1,
        x + w, y,     u1, v1,
        x + w, y + h, u1, v0,
        x,     y + h, u0, v0,
    };
    m_spriteShader.Use();
    m_spriteShader.SetMat4("uProjection", proj);
    m_spriteShader.SetVec4("uTint", cmd.Tint);
    m_spriteShader.SetInt("uTexture", 0);
    cmd.Tex->Bind(0);
    glBindVertexArray(m_spriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_spriteVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void UIRenderer::End() {
    if (m_commands.empty()) return;

    glDisable(GL_DEPTH_TEST);
    // В ortho-проекции с перевёрнутой осью Y (0 сверху) обход вершин квадов
    // становится CW — backface culling движка отсёк бы весь интерфейс
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glm::mat4 proj = glm::ortho(0.0f, (float)m_screenWidth, (float)m_screenHeight, 0.0f, -1.0f, 1.0f);

    // Один раз заливаем все цветные вершины кадра; индексы — под все квады
    if (m_quadCount > 0) {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(UIVertex), m_vertices.data(), GL_DYNAMIC_DRAW);
        EnsureIndexCapacity(m_quadCount);
    }

    // Проходим команды по порядку — слои (панель/спрайт/текст) сохраняются
    for (const Command& cmd : m_commands) {
        if (cmd.Kind == Command::Type::ColoredQuads) {
            DrawColored(cmd.FirstQuad, cmd.QuadCount, proj);
        } else {
            DrawSprite(cmd, proj);
        }
    }

    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}
