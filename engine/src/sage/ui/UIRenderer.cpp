#include "UIRenderer.h"
#include "stb_easy_font.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdio>

namespace {
    // Временный буфер под геометрию одного вызова Text (stb_easy_font пишет
    // сюда сырой поток вершин — используется только в fallback-пути без шрифта).
    constexpr int kTextScratchBytes = 64 * 1024;
    char g_textScratch[kTextScratchBytes];

    // Встроенный шейдер UI-текста/прямоугольников. Самодостаточен (не зависит
    // от ассетов игры): рисует и сплошные квады, и глифы из атласа шрифта.
    // Сигнал «сплошной квад» — vUV.x < 0 (тогда покрытие = 1); иначе покрытие
    // берётся из красного канала атласа (glyph coverage).
    const char* kUiVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;
out vec4 vColor;
out vec2 vUV;
uniform mat4 uProjection;
void main() {
    vColor = aColor;
    vUV = aUV;
    gl_Position = uProjection * vec4(aPos, 1.0);
}
)";

    const char* kUiFrag = R"(#version 330 core
in vec4 vColor;
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uFont;
void main() {
    float coverage = (vUV.x < 0.0) ? 1.0 : texture(uFont, vUV).r;
    FragColor = vec4(vColor.rgb, vColor.a * coverage);
}
)";

    // Кандидаты шрифта по умолчанию: сначала свой (рядом с бинарником, копируется
    // сборкой), затем системные — чтобы текст был читаемым везде.
    const char* kDefaultFontCandidates[] = {
        "assets/fonts/sage-default.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
}

UIRenderer::UIRenderer()
    : m_shader(Shader::FromSource(kUiVert, kUiFrag, "UIRenderer")) {
    // Формат вершины: 3 float позиции + 4 байта цвета + 2 float UV.
    sage::rhi::VertexLayout layout;
    layout.Stride = sizeof(UIVertex);
    layout.Attributes = {
        {0, 3, sage::rhi::AttribType::Float, 0},
        {1, 4, sage::rhi::AttribType::UByteNorm, 12},
        {2, 2, sage::rhi::AttribType::Float, 16},
    };
    m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);

    // Пытаемся загрузить шрифт по умолчанию — молча (без шрифта работает
    // fallback на stb_easy_font, поэтому это не ошибка).
    for (const char* path : kDefaultFontCandidates) {
        if (SetFont(path)) break;
    }
    if (!m_font) {
        LOG_WARN("UIRenderer") << "Шрифт по умолчанию не найден — текст через stb_easy_font (ASCII)";
    }
}

bool UIRenderer::SetFont(const std::string& path, float pixelHeight) {
    try {
        m_font = Font::Load(path, pixelHeight);
        return true;
    } catch (const std::exception& e) {
        LOG_DEBUG("UIRenderer") << "SetFont пропущен (" << path << "): " << e.what();
        return false;
    }
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

    // UV = (-1,-1) — сплошной квад (шейдер игнорирует атлас).
    m_vertices.push_back({x,     y,     0.0f, r, g, b, a, -1.0f, -1.0f});
    m_vertices.push_back({x + w, y,     0.0f, r, g, b, a, -1.0f, -1.0f});
    m_vertices.push_back({x + w, y + h, 0.0f, r, g, b, a, -1.0f, -1.0f});
    m_vertices.push_back({x,     y + h, 0.0f, r, g, b, a, -1.0f, -1.0f});
    ++m_quadCount;
}

void UIRenderer::PushGlyphQuad(float x0, float y0, float x1, float y1,
                               glm::vec2 uv0, glm::vec2 uv1, glm::vec3 color) {
    unsigned char r = static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    unsigned char g = static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    unsigned char b = static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    m_vertices.push_back({x0, y0, 0.0f, r, g, b, 255, uv0.x, uv0.y});
    m_vertices.push_back({x1, y0, 0.0f, r, g, b, 255, uv1.x, uv0.y});
    m_vertices.push_back({x1, y1, 0.0f, r, g, b, 255, uv1.x, uv1.y});
    m_vertices.push_back({x0, y1, 0.0f, r, g, b, 255, uv0.x, uv1.y});
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

    if (m_font) {
        // Масштаб API → множитель шрифта относительно базовой высоты запекания.
        float fontScale = (scale * m_scaleToPixels) / m_font->PixelHeight();
        std::vector<Font::PositionedGlyph> quads;
        m_font->BuildQuads(text, x, y, fontScale, quads);
        for (const auto& q : quads) {
            PushGlyphQuad(q.x0, q.y0, q.x1, q.y1, q.uv0, q.uv1, color);
        }
        return;
    }
    TextEasyFont(x, y, scale, color, text);
}

void UIRenderer::TextEasyFont(float x, float y, float scale, glm::vec3 color, const std::string& text) {
    unsigned char rgba[4] = {
        static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f),
        255
    };

    int quads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(text.c_str()), rgba,
                                     g_textScratch, kTextScratchBytes);

    const char* src = g_textScratch;
    for (int q = 0; q < quads; ++q) {
        const float* p0 = reinterpret_cast<const float*>(src);
        const unsigned char* col = reinterpret_cast<const unsigned char*>(src + 12);
        // stb пишет 4 вершины квада; берём габарит и выкладываем сплошной квад.
        float minx = p0[0], miny = p0[1], maxx = p0[0], maxy = p0[1];
        for (int v = 1; v < 4; ++v) {
            const float* pv = reinterpret_cast<const float*>(src + v * 16);
            minx = std::min(minx, pv[0]); maxx = std::max(maxx, pv[0]);
            miny = std::min(miny, pv[1]); maxy = std::max(maxy, pv[1]);
        }
        glm::vec3 c{col[0] / 255.0f, col[1] / 255.0f, col[2] / 255.0f};
        PushQuad(x + minx * scale, y + miny * scale,
                 (maxx - minx) * scale, (maxy - miny) * scale, c, 1.0f);
        src += 16 * 4;
    }
}

float UIRenderer::MeasureText(const std::string& text, float scale) const {
    if (text.empty()) return 0.0f;
    if (m_font) {
        float fontScale = (scale * m_scaleToPixels) / m_font->PixelHeight();
        return m_font->MeasureWidth(text, fontScale);
    }
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
    m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/true);
    m_indexCapacity = indices.size();
}

void UIRenderer::End() {
    if (m_quadCount == 0) return;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetDepthTest(false);
    // В ortho-проекции с перевёрнутой осью Y (0 сверху) обход вершин квадов
    // становится CW — backface culling движка отсёк бы весь интерфейс.
    device.SetCullMode(sage::rhi::CullMode::Off);
    device.SetBlend(true);

    glm::mat4 proj = glm::ortho(0.0f, (float)m_screenWidth, (float)m_screenHeight, 0.0f, -1.0f, 1.0f);
    m_shader.Use();
    m_shader.SetMat4("uProjection", proj);
    m_shader.SetInt("uFont", 0);
    if (m_font) m_font->Atlas().Bind(0);

    m_geometry->SetVertexData(m_vertices.data(), m_vertices.size() * sizeof(UIVertex), /*dynamic=*/true);
    EnsureIndexCapacity(m_quadCount);
    m_geometry->DrawIndexed(m_quadCount * 6);

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthTest(true);
}
