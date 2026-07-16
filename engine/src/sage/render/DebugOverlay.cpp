#include "DebugOverlay.h"
#include "stb_easy_font.h"
#include "sage/rhi/GraphicsDevice.h"
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
    // stb_easy_font вершина: 3 float (x,y,z) + 4 байта цвета = 16 байт
    sage::rhi::VertexLayout layout;
    layout.Stride = 16;
    layout.Attributes = {
        {0, 3, sage::rhi::AttribType::Float, 0},
        {1, 4, sage::rhi::AttribType::UByteNorm, 12},
    };
    m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);
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
        m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/true);
        m_indexCapacity = indices.size();
    }

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetDepthTest(false);
    device.SetCullMode(sage::rhi::CullMode::Off); // квады текста в перевёрнутом ortho идут CW
    device.SetBlend(true);

    // Масштабируем экранные координаты так, чтобы мелкий векторный шрифт
    // stb_easy_font (~7px высота глифа) читался на HiDPI/телефонных экранах.
    glm::mat4 proj = glm::ortho(0.0f, screenWidth / kScreenScale, screenHeight / kScreenScale, 0.0f, -1.0f, 1.0f);

    m_shader.Use();
    m_shader.SetMat4("uProjection", proj);

    m_geometry->SetVertexData(g_vertexScratch, (size_t)offsetBytes, /*dynamic=*/true);
    m_geometry->DrawIndexed((size_t)totalQuads * 6);

    device.SetBlend(false);
    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthTest(true);
}
