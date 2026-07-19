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

    // Встроенный шейдер UI. Один на всё: сплошные/скруглённые квады (SDF),
    // глифы шрифта (покрытие из красного канала атласа), текстурные картинки
    // (uMode = 1: полноцветная выборка с тонированием).
    //   • «сплошной квад» — vUV.x < 0 (атлас не сэмплируется);
    //   • SDF включён, когда полуразмер vHalf.x > 0: считается расстояние до
    //     контура скруглённого прямоугольника; Params.y > 0 — режим рамки
    //     (полоса [-t..0] от контура), иначе — заливка с антиалиасным краем.
    const char* kUiVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec2 aLocal;
layout (location = 4) in vec2 aHalf;
layout (location = 5) in vec2 aParams;
out vec4 vColor;
out vec2 vUV;
out vec2 vLocal;
out vec2 vHalf;
out vec2 vParams;
uniform mat4 uProjection;
void main() {
    vColor = aColor;
    vUV = aUV;
    vLocal = aLocal;
    vHalf = aHalf;
    vParams = aParams;
    gl_Position = uProjection * vec4(aPos, 1.0);
}
)";

    const char* kUiFrag = R"(#version 330 core
in vec4 vColor;
in vec2 vUV;
in vec2 vLocal;
in vec2 vHalf;
in vec2 vParams;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uMode; // 0 — сплошные квады + глифы шрифта; 1 — текстурная картинка

// Знаковое расстояние до контура прямоугольника с радиусом скругления r:
// отрицательное внутри, ноль на контуре.
float RoundedBoxSDF(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - halfSize + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec4 col = vColor;
    if (uMode == 1) {
        col *= texture(uTex, vUV);            // картинка с тонированием
    } else if (vUV.x >= 0.0) {
        col.a *= texture(uTex, vUV).r;        // глиф: покрытие из атласа
    }
    if (vHalf.x > 0.0) {
        float d = RoundedBoxSDF(vLocal, vHalf, vParams.x);
        float aa = 0.75; // полупиксельный антиалиасинг края
        if (vParams.y > 0.0) {
            // Рамка: полоса толщиной t внутрь от контура.
            col.a *= (1.0 - smoothstep(-aa, aa, d)) *
                     smoothstep(-vParams.y - aa, -vParams.y + aa, d);
        } else {
            col.a *= 1.0 - smoothstep(-aa, aa, d);
        }
    }
    FragColor = col;
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
    // Формат вершины: позиция + цвет + UV + SDF-атрибуты (см. UIVertex).
    sage::rhi::VertexLayout layout;
    layout.Stride = sizeof(UIVertex);
    layout.Attributes = {
        {0, 3, sage::rhi::AttribType::Float, 0},
        {1, 4, sage::rhi::AttribType::UByteNorm, 12},
        {2, 2, sage::rhi::AttribType::Float, 16},
        {3, 2, sage::rhi::AttribType::Float, 24}, // Local
        {4, 2, sage::rhi::AttribType::Float, 32}, // Half
        {5, 2, sage::rhi::AttribType::Float, 40}, // Params (radius, border)
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
    m_segments.clear();
    m_clipStack.clear();
    m_quadCount = 0;
}

UIRenderer::Segment& UIRenderer::CurrentSegment(const Texture* image) {
    bool clipped = !m_clipStack.empty();
    glm::vec4 clip = clipped ? m_clipStack.back() : glm::vec4(0.0f);
    if (!m_segments.empty()) {
        Segment& last = m_segments.back();
        if (last.Image == image && last.Clipped == clipped &&
            (!clipped || last.Clip == clip)) {
            return last; // состояние не изменилось — продолжаем батч
        }
    }
    Segment seg;
    seg.FirstQuad = m_quadCount;
    seg.Image = image;
    seg.Clipped = clipped;
    seg.Clip = clip;
    m_segments.push_back(seg);
    return m_segments.back();
}

void UIRenderer::PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha,
                          float radius, float border, bool solidUv) {
    unsigned char r = static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    unsigned char g = static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    unsigned char b = static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    unsigned char a = static_cast<unsigned char>(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f);

    float hw = w * 0.5f, hh = h * 0.5f;
    // Радиус не может превышать половину меньшей стороны (иначе SDF ломает форму).
    float rad = glm::clamp(radius, 0.0f, glm::min(hw, hh));
    // UV сплошного квада = (-1,-1); картинки (solidUv=false) — 0..1.
    float u0 = solidUv ? -1.0f : 0.0f, v0 = solidUv ? -1.0f : 0.0f;
    float u1 = solidUv ? -1.0f : 1.0f, v1 = solidUv ? -1.0f : 1.0f;

    m_vertices.push_back({x,     y,     0.0f, r, g, b, a, u0, v0, -hw, -hh, hw, hh, rad, border});
    m_vertices.push_back({x + w, y,     0.0f, r, g, b, a, u1, v0,  hw, -hh, hw, hh, rad, border});
    m_vertices.push_back({x + w, y + h, 0.0f, r, g, b, a, u1, v1,  hw,  hh, hw, hh, rad, border});
    m_vertices.push_back({x,     y + h, 0.0f, r, g, b, a, u0, v1, -hw,  hh, hw, hh, rad, border});
    ++m_quadCount;
}

void UIRenderer::PushGlyphQuad(float x0, float y0, float x1, float y1,
                               glm::vec2 uv0, glm::vec2 uv1, glm::vec3 color, float alpha) {
    unsigned char r = static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    unsigned char g = static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    unsigned char b = static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    unsigned char a = static_cast<unsigned char>(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    // Half = 0 — SDF выключен (глиф режется покрытием атласа, не формой).
    m_vertices.push_back({x0, y0, 0.0f, r, g, b, a, uv0.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({x1, y0, 0.0f, r, g, b, a, uv1.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({x1, y1, 0.0f, r, g, b, a, uv1.x, uv1.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({x0, y1, 0.0f, r, g, b, a, uv0.x, uv1.y, 0, 0, 0, 0, 0, 0});
    ++m_quadCount;
}

void UIRenderer::Rect(float x, float y, float w, float h, glm::vec3 color, float alpha) {
    CurrentSegment(nullptr).QuadCount++;
    PushQuad(x, y, w, h, color, alpha, 0.0f, 0.0f, /*solidUv=*/true);
}

void UIRenderer::RoundedRect(float x, float y, float w, float h, glm::vec3 color,
                             float alpha, float radius) {
    CurrentSegment(nullptr).QuadCount++;
    PushQuad(x, y, w, h, color, alpha, radius, 0.0f, /*solidUv=*/true);
}

void UIRenderer::RectOutline(float x, float y, float w, float h, float t, glm::vec3 color, float alpha) {
    Rect(x, y, w, t, color, alpha);              // верх
    Rect(x, y + h - t, w, t, color, alpha);      // низ
    Rect(x, y + t, t, h - 2 * t, color, alpha);  // лево
    Rect(x + w - t, y + t, t, h - 2 * t, color, alpha); // право
}

void UIRenderer::RoundedRectOutline(float x, float y, float w, float h, float radius, float t,
                                    glm::vec3 color, float alpha) {
    CurrentSegment(nullptr).QuadCount++;
    PushQuad(x, y, w, h, color, alpha, radius, glm::max(t, 0.5f), /*solidUv=*/true);
}

void UIRenderer::Image(float x, float y, float w, float h, const Texture* texture,
                       glm::vec3 tint, float alpha, float radius) {
    if (!texture) return;
    CurrentSegment(texture).QuadCount++;
    PushQuad(x, y, w, h, tint, alpha, radius, 0.0f, /*solidUv=*/false);
}

void UIRenderer::PushClipRect(float x, float y, float w, float h) {
    glm::vec4 rect{x, y, glm::max(w, 0.0f), glm::max(h, 0.0f)};
    if (!m_clipStack.empty()) {
        // Пересечение с текущей маской: вложенный клип не может расширить внешний.
        const glm::vec4& c = m_clipStack.back();
        float x0 = glm::max(rect.x, c.x), y0 = glm::max(rect.y, c.y);
        float x1 = glm::min(rect.x + rect.z, c.x + c.z);
        float y1 = glm::min(rect.y + rect.w, c.y + c.w);
        rect = {x0, y0, glm::max(x1 - x0, 0.0f), glm::max(y1 - y0, 0.0f)};
    }
    m_clipStack.push_back(rect);
}

void UIRenderer::PopClipRect() {
    if (m_clipStack.empty()) {
        LOG_WARN("UIRenderer") << "PopClipRect без парного PushClipRect — игнорирую";
        return;
    }
    m_clipStack.pop_back();
}

void UIRenderer::Text(float x, float y, float scale, glm::vec3 color, const std::string& text,
                      float alpha) {
    if (text.empty()) return;

    if (m_font) {
        CurrentSegment(nullptr); // глифы идут в шрифтовый сегмент
        // Масштаб API → множитель шрифта относительно базовой высоты запекания.
        float fontScale = (scale * m_scaleToPixels) / m_font->PixelHeight();
        std::vector<Font::PositionedGlyph> quads;
        m_font->BuildQuads(text, x, y, fontScale, quads);
        size_t added = 0;
        for (const auto& q : quads) {
            PushGlyphQuad(q.x0, q.y0, q.x1, q.y1, q.uv0, q.uv1, color, alpha);
            ++added;
        }
        m_segments.back().QuadCount += added;
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
        Rect(x + minx * scale, y + miny * scale,
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

void UIRenderer::TextCentered(float centerX, float y, float scale, glm::vec3 color,
                              const std::string& text, float alpha) {
    Text(centerX - MeasureText(text, scale) * 0.5f, y, scale, color, text, alpha);
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
    if (!m_clipStack.empty()) {
        LOG_WARN("UIRenderer") << "End(): " << m_clipStack.size()
                               << " незакрытых PushClipRect — закрываю принудительно";
        m_clipStack.clear();
    }
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
    m_shader.SetInt("uTex", 0);

    m_geometry->SetVertexData(m_vertices.data(), m_vertices.size() * sizeof(UIVertex), /*dynamic=*/true);
    EnsureIndexCapacity(m_quadCount);

    // Сегменты: смена состояния (текстура/маска) только там, где она реально
    // случилась; без картинок и масок весь UI — по-прежнему один draw call.
    for (const Segment& seg : m_segments) {
        if (seg.QuadCount == 0) continue;
        if (seg.Clipped) {
            // glScissor ждёт левый НИЖНИЙ угол — переворачиваем Y.
            int sy = m_screenHeight - (int)(seg.Clip.y + seg.Clip.w);
            device.SetScissor(true, (int)seg.Clip.x, sy, (int)seg.Clip.z, (int)seg.Clip.w);
        } else {
            device.SetScissor(false);
        }
        if (seg.Image) {
            m_shader.SetInt("uMode", 1);
            seg.Image->Bind(0);
        } else {
            m_shader.SetInt("uMode", 0);
            if (m_font) m_font->Atlas().Bind(0);
        }
        m_geometry->DrawIndexedRange(seg.FirstQuad * 6, seg.QuadCount * 6);
    }
    device.SetScissor(false);

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthTest(true);
}
