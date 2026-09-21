#include "UIRenderer.h"
#include "stb_easy_font.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/assets/AssetDatabase.h"
#include <filesystem>
#include <system_error>
#include "sage/core/Config.h"
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
        // Ширина перехода — РЕАЛЬНЫЙ размер пикселя в единицах SDF, а не
        // константа. Константа верна ровно при одном масштабе интерфейса: на
        // другом край либо зубчатый (переход уже пикселя), либо мыльный
        // (шире). fwidth даёт, насколько d меняется между соседними пикселями,
        // то есть ровно ту ширину, на которой край и должен размываться.
        float aa = max(fwidth(d), 0.0001) * 0.75;
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

    // Сначала — шрифт из настроек (игра со своим стилем), потом кандидаты по
    // умолчанию. Молча: без шрифта работает fallback на stb_easy_font, поэтому
    // это не ошибка.
    bool loaded = false;
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    if (!cfg.UiFont.empty()) loaded = SetFont(cfg.UiFont, cfg.UiFontPixelHeight, cfg.UiFontPixelArt);
    if (!loaded) {
        // EngineAssetPath — потому что «рядом с бинарником» и «в текущей папке»
        // это разные места: плеер уходит в папку проекта, и свой шрифт по
        // относительному пути там уже не находился (текст молча падал на
        // stb_easy_font, то есть терял кириллицу).
        for (const char* path : kDefaultFontCandidates) {
            if (SetFont(sage::EngineAssetPath(path))) { loaded = true; break; }
        }
    }
    if (!m_font) {
        LOG_WARN("UIRenderer") << "Шрифт по умолчанию не найден — текст через stb_easy_font (ASCII)";
    }
}

bool UIRenderer::SetFont(const std::string& path, float pixelHeight, bool pixelArt) {
    try {
        m_font = Font::Load(path, pixelHeight, pixelArt);
        // Предупреждение о кириллице — один раз при загрузке. Пиксельные шрифты
        // из готовых наборов почти всегда только латинские, а надписи в игре
        // русские: без этой строки разработчик видит экран вопросительных знаков
        // и ищет ошибку в движке, а не в шрифте.
        if (m_font && !m_font->HasGlyph(0x0410 /* А */)) {
            LOG_WARN("UIRenderer") << "Шрифт без кириллицы (" << path
                                   << ") — русский текст будет знаками вопроса";
        }
        return true;
    } catch (const std::exception& e) {
        LOG_DEBUG("UIRenderer") << "SetFont пропущен (" << path << "): " << e.what();
        return false;
    }
}

void UIRenderer::SetView(glm::vec2 originPx, float scale, int fbWidth, int fbHeight) {
    m_viewOrigin = originPx;
    m_viewScale = scale > 0.0f ? scale : 1.0f;
    m_fbWidth = fbWidth > 0 ? fbWidth : m_screenWidth;
    m_fbHeight = fbHeight > 0 ? fbHeight : m_screenHeight;
}

void UIRenderer::Begin(int screenWidth, int screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    // Каждый кадр начинается с тождественного вида: смотровое преобразование —
    // это свойство ОДНОЙ отрисовки (редактор в режиме вёрстки), а не состояние,
    // которое должно пережить кадр и уехать в игру.
    m_viewOrigin = glm::vec2(0.0f);
    m_viewScale = 1.0f;
    m_fbWidth = screenWidth;
    m_fbHeight = screenHeight;
    m_vertices.clear();
    m_segments.clear();
    m_clipStack.clear();
    m_quadCount = 0;
}

UIRenderer::Segment& UIRenderer::CurrentSegment(const Texture* image, const Font* font) {
    bool clipped = !m_clipStack.empty();
    glm::vec4 clip = clipped ? m_clipStack.back() : glm::vec4(0.0f);
    if (!m_segments.empty()) {
        Segment& last = m_segments.back();
        // Шрифт — такая же смена состояния, как текстура: у каждого свой атлас,
        // и склеить в один кусок глифы двух шрифтов значит взять буквы второго
        // из картинки первого.
        if (last.Image == image && last.TextFont == font && last.Clipped == clipped &&
            (!clipped || last.Clip == clip)) {
            return last; // состояние не изменилось — продолжаем батч
        }
    }
    Segment seg;
    seg.FirstQuad = m_quadCount;
    seg.Image = image;
    seg.TextFont = font;
    seg.Clipped = clipped;
    seg.Clip = clip;
    m_segments.push_back(seg);
    return m_segments.back();
}

void UIRenderer::PushRotation(glm::vec2 center, float degrees) {
    m_rotCenter = center;
    const float rad = glm::radians(degrees);
    m_rotCos = std::cos(rad);
    m_rotSin = std::sin(rad);
    // Ноль градусов — не поворот: тратить по два умножения на вершину на
    // тождественное преобразование не за что, а элементов на экране сотни.
    m_rotActive = std::fabs(degrees) > 0.0001f;
}

void UIRenderer::PopRotation() { m_rotActive = false; }

glm::vec2 UIRenderer::Rotated(float x, float y) const {
    if (!m_rotActive) return {x, y};
    const float dx = x - m_rotCenter.x, dy = y - m_rotCenter.y;
    return {m_rotCenter.x + dx * m_rotCos - dy * m_rotSin,
            m_rotCenter.y + dx * m_rotSin + dy * m_rotCos};
}

void UIRenderer::PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha,
                          float radius, float border, bool solidUv,
                          const glm::vec3* bottomColor, const float* bottomAlpha) {
    auto byte = [](float v) {
        return static_cast<unsigned char>(glm::clamp(v, 0.0f, 1.0f) * 255.0f);
    };
    unsigned char r = byte(color.r), g = byte(color.g), b = byte(color.b), a = byte(alpha);
    const glm::vec3& bc = bottomColor ? *bottomColor : color;
    unsigned char r2 = byte(bc.r), g2 = byte(bc.g), b2 = byte(bc.b);
    unsigned char a2 = byte(bottomAlpha ? *bottomAlpha : alpha);

    float hw = w * 0.5f, hh = h * 0.5f;
    // Радиус не может превышать половину меньшей стороны (иначе SDF ломает форму).
    float rad = glm::clamp(radius, 0.0f, glm::min(hw, hh));
    // UV сплошного квада = (-1,-1); картинки (solidUv=false) — 0..1.
    float u0 = solidUv ? -1.0f : 0.0f, v0 = solidUv ? -1.0f : 0.0f;
    float u1 = solidUv ? -1.0f : 1.0f, v1 = solidUv ? -1.0f : 1.0f;

    const glm::vec2 p0 = Rotated(x, y), p1 = Rotated(x + w, y);
    const glm::vec2 p2 = Rotated(x + w, y + h), p3 = Rotated(x, y + h);
    m_vertices.push_back({p0.x, p0.y, 0.0f, r,  g,  b,  a,  u0, v0, -hw, -hh, hw, hh, rad, border});
    m_vertices.push_back({p1.x, p1.y, 0.0f, r,  g,  b,  a,  u1, v0,  hw, -hh, hw, hh, rad, border});
    m_vertices.push_back({p2.x, p2.y, 0.0f, r2, g2, b2, a2, u1, v1,  hw,  hh, hw, hh, rad, border});
    m_vertices.push_back({p3.x, p3.y, 0.0f, r2, g2, b2, a2, u0, v1, -hw,  hh, hw, hh, rad, border});
    ++m_quadCount;
}

void UIRenderer::PushGlyphQuad(float x0, float y0, float x1, float y1,
                               glm::vec2 uv0, glm::vec2 uv1, glm::vec3 color, float alpha,
                               float slant, float baseline) {
    unsigned char r = static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    unsigned char g = static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    unsigned char b = static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    unsigned char a = static_cast<unsigned char>(glm::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    // Half = 0 — SDF выключен (глиф режется покрытием атласа, не формой).
    // Курсив: глиф наклоняется ОТНОСИТЕЛЬНО БАЗОВОЙ ЛИНИИ — верх уезжает
    // вправо, низ (хвосты у «р», «у») влево. Наклон вокруг верха строки
    // поставил бы буквы на воздух: у строки уехал бы весь низ.
    const float topShift = slant != 0.0f ? (baseline - y0) * slant : 0.0f;
    const float botShift = slant != 0.0f ? (baseline - y1) * slant : 0.0f;
    const glm::vec2 g0 = Rotated(x0 + topShift, y0), g1 = Rotated(x1 + topShift, y0);
    const glm::vec2 g2v = Rotated(x1 + botShift, y1), g3 = Rotated(x0 + botShift, y1);
    m_vertices.push_back({g0.x, g0.y, 0.0f, r, g, b, a, uv0.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({g1.x, g1.y, 0.0f, r, g, b, a, uv1.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({g2v.x, g2v.y, 0.0f, r, g, b, a, uv1.x, uv1.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({g3.x, g3.y, 0.0f, r, g, b, a, uv0.x, uv1.y, 0, 0, 0, 0, 0, 0});
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

void UIRenderer::PushImageQuad(float x, float y, float w, float h, glm::vec2 uv0, glm::vec2 uv1,
                               glm::vec3 tint, float alpha) {
    auto byte = [](float v) {
        return static_cast<unsigned char>(glm::clamp(v, 0.0f, 1.0f) * 255.0f);
    };
    const unsigned char r = byte(tint.r), g = byte(tint.g), b = byte(tint.b), a = byte(alpha);
    // ПОВОРОТ ПРИМЕНЯЕТСЯ И К КАРТИНКЕ. Здесь его не было, и это единственное
    // место, где он пропускался: подложка (PushQuad) и буквы (PushGlyphQuad)
    // поворачивались, а картинка оставалась стоять прямо. Снаружи это
    // выглядело как «изображение не поворачивается вообще» — причём у кнопки с
    // фоном и подписью поворачивалось всё, кроме её значка.
    const glm::vec2 p0 = Rotated(x, y), p1 = Rotated(x + w, y);
    const glm::vec2 p2 = Rotated(x + w, y + h), p3 = Rotated(x, y + h);
    // Half = 0 — SDF выключен: у куска листа нет своей формы, он прямоугольный.
    m_vertices.push_back({p0.x, p0.y, 0.0f, r, g, b, a, uv0.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({p1.x, p1.y, 0.0f, r, g, b, a, uv1.x, uv0.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({p2.x, p2.y, 0.0f, r, g, b, a, uv1.x, uv1.y, 0, 0, 0, 0, 0, 0});
    m_vertices.push_back({p3.x, p3.y, 0.0f, r, g, b, a, uv0.x, uv1.y, 0, 0, 0, 0, 0, 0});
    ++m_quadCount;
}

namespace {
// V-координата пикселя ЛИСТА, считая сверху.
//
// Текстуры движка загружаются перевёрнутыми по вертикали (stbi flip): у OpenGL
// начало координат внизу, и для 3D так правильно. Но спрайт в наборе описан от
// ВЕРХНЕГО левого угла файла — как его видит человек в редакторе картинок, — и
// переворот надо снять ровно здесь. Иначе «панель в (11,11)» возьмёт кусок с
// противоположного края листа, и это выглядит как случайный мусор.
inline float SheetV(float yPixels, float texHeight) { return 1.0f - yPixels / texHeight; }

// Кусок листа в долях текстуры. Спрайт задан в пикселях исходника — здесь он и
// переводится, один раз и в одном месте.
struct SpriteUV { glm::vec2 uv0{0.0f, 1.0f}, uv1{1.0f, 0.0f}; };
SpriteUV ResolveSprite(const Texture& tex, const UIRenderer::Sprite& s) {
    SpriteUV out;
    if (s.Whole() || tex.Width() <= 0 || tex.Height() <= 0) return out;
    const float tw = (float)tex.Width(), th = (float)tex.Height();
    out.uv0 = {s.X / tw, SheetV(s.Y, th)};
    out.uv1 = {(s.X + s.W) / tw, SheetV(s.Y + s.H, th)};
    return out;
}
} // namespace

void UIRenderer::ImageSprite(float x, float y, float w, float h, const Texture* texture,
                             Sprite src, glm::vec3 tint, float alpha) {
    if (!texture) return;
    CurrentSegment(texture).QuadCount++;
    const SpriteUV uv = ResolveSprite(*texture, src);
    PushImageQuad(x, y, w, h, uv.uv0, uv.uv1, tint, alpha);
}

void UIRenderer::ImageNineSlice(float x, float y, float w, float h, const Texture* texture,
                                Sprite src, glm::vec4 border, float scale, glm::vec3 tint,
                                float alpha) {
    if (!texture || texture->Width() <= 0 || texture->Height() <= 0) return;
    if (src.Whole()) {
        src = Sprite{0.0f, 0.0f, (float)texture->Width(), (float)texture->Height()};
    }
    if (scale <= 0.0f) scale = 1.0f;

    // Углы в пикселях ЭКРАНА. Если рамка не влезает в запрошенный размер (панель
    // уже собственных углов), доли ужимаются пропорционально — иначе куски
    // наложились бы друг на друга и рамка вывернулась бы наизнанку.
    float l = border.x * scale, t = border.y * scale;
    float r = border.z * scale, b = border.w * scale;
    const float sumX = l + r, sumY = t + b;
    if (sumX > w && sumX > 0.0f) { const float k = w / sumX; l *= k; r *= k; }
    if (sumY > h && sumY > 0.0f) { const float k = h / sumY; t *= k; b *= k; }

    const float tw = (float)texture->Width(), th = (float)texture->Height();
    // Границы кусков: по X — левый край, конец угла, начало правого угла, край.
    const float sx[4] = {src.X, src.X + border.x, src.X + src.W - border.z, src.X + src.W};
    const float sy[4] = {src.Y, src.Y + border.y, src.Y + src.H - border.w, src.Y + src.H};
    const float dx[4] = {x, x + l, x + w - r, x + w};
    const float dy[4] = {y, y + t, y + h - b, y + h};

    CurrentSegment(texture).QuadCount += 9;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float qw = dx[col + 1] - dx[col];
            const float qh = dy[row + 1] - dy[row];
            if (qw <= 0.0f || qh <= 0.0f) { CurrentSegment(texture).QuadCount--; continue; }
            PushImageQuad(dx[col], dy[row], qw, qh,
                          {sx[col] / tw, SheetV(sy[row], th)},
                          {sx[col + 1] / tw, SheetV(sy[row + 1], th)}, tint, alpha);
        }
    }
}

void UIRenderer::ImageSliced(float x, float y, float w, float h, const Texture* texture,
                             Sprite src, const sage::ui::NineSlice& slice, float scale,
                             glm::vec3 tint, float alpha) {
    if (!texture || texture->Width() <= 0 || texture->Height() <= 0) return;
    if (src.Whole()) src = Sprite{0.0f, 0.0f, (float)texture->Width(), (float)texture->Height()};

    sage::ui::SliceRequest req;
    req.SrcX = src.X; req.SrcY = src.Y; req.SrcW = src.W; req.SrcH = src.H;
    req.DstX = x; req.DstY = y; req.DstW = w; req.DstH = h;
    req.Scale = scale > 0.0f ? scale : 1.0f;

    const std::vector<sage::ui::SliceQuad> quads = sage::ui::Solve(slice, req);
    if (quads.empty()) return;

    const float tw = (float)texture->Width(), th = (float)texture->Height();
    CurrentSegment(texture).QuadCount += (int)quads.size();
    for (const sage::ui::SliceQuad& q : quads) {
        PushImageQuad(q.DstX, q.DstY, q.DstW, q.DstH,
                      {q.SrcX / tw, SheetV(q.SrcY, th)},
                      {(q.SrcX + q.SrcW) / tw, SheetV(q.SrcY + q.SrcH, th)}, tint, alpha);
    }
}

void UIRenderer::PushFreeQuad(const glm::vec2 p[4], const glm::vec3 c[4], const float a[4]) {
    for (int i = 0; i < 4; ++i) {
        unsigned char r = static_cast<unsigned char>(glm::clamp(c[i].r, 0.0f, 1.0f) * 255.0f);
        unsigned char g = static_cast<unsigned char>(glm::clamp(c[i].g, 0.0f, 1.0f) * 255.0f);
        unsigned char b = static_cast<unsigned char>(glm::clamp(c[i].b, 0.0f, 1.0f) * 255.0f);
        unsigned char al = static_cast<unsigned char>(glm::clamp(a[i], 0.0f, 1.0f) * 255.0f);
        // UV = (-1,-1) — сплошная заливка; Half = 0 — SDF не участвует.
        const glm::vec2 rp = Rotated(p[i].x, p[i].y);
        m_vertices.push_back({rp.x, rp.y, 0.0f, r, g, b, al, -1.0f, -1.0f, 0, 0, 0, 0, 0, 0});
    }
    ++m_quadCount;
}

void UIRenderer::Circle(float cx, float cy, float radius, glm::vec3 color, float alpha) {
    RoundedRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, color, alpha, radius);
}

void UIRenderer::Ring(float cx, float cy, float radius, float thickness, glm::vec3 color,
                      float alpha) {
    RoundedRectOutline(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, radius,
                       thickness, color, alpha);
}

void UIRenderer::Quad(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3,
                      glm::vec3 color, float alpha) {
    CurrentSegment(nullptr).QuadCount++;
    const glm::vec2 pts[4] = {p0, p1, p2, p3};
    const glm::vec3 cols[4] = {color, color, color, color};
    const float alphas[4] = {alpha, alpha, alpha, alpha};
    PushFreeQuad(pts, cols, alphas);
}

void UIRenderer::Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec3 color, float alpha) {
    // Треугольник — квад со схлопнутой четвёртой вершиной: отдельный путь
    // отрисовки ради него заводить незачем, вырожденная сторона не рисуется.
    Quad(a, b, c, c, color, alpha);
}

void UIRenderer::GradientRect(float x, float y, float w, float h, glm::vec3 top, glm::vec3 bottom,
                              float alphaTop, float alphaBottom, float radius) {
    // ОДИН квад: цвет и так лежит в вершинах, а скругление считает SDF по
    // локальным координатам — ему всё равно, одноцветный квад или нет.
    // Собирать градиент полосами (как было) нельзя: полосы приходится класть
    // внахлёст, чтобы не осталось щелей, а полупрозрачный нахлёст смешивается
    // дважды — по панели идут тёмные линии на каждой границе полос.
    CurrentSegment(nullptr).QuadCount++;
    PushQuad(x, y, w, h, top, alphaTop, radius, 0.0f, /*solidUv=*/true, &bottom, &alphaBottom);
}

void UIRenderer::RectShadow(float x, float y, float w, float h, float radius,
                            float size, float alpha) {
    if (size <= 0.0f || alpha <= 0.0f) return;
    // Расширяющиеся контуры с падающей прозрачностью. Настоящее размытие
    // потребовало бы отдельного прохода и буфера — здесь оно не окупается:
    // тень под панелью интерфейса видна боковым зрением, и разницы между
    // ступенчатым спадом и гауссом на ней никто не замечает. Ступеней восемь,
    // а спад квадратичный: с четырьмя линейными по краю тени видны кольца.
    const int kSteps = 8;
    for (int i = kSteps; i >= 1; --i) {
        float t = (float)i / kSteps;
        float grow = size * t;
        float a = alpha * (1.0f - t) * (1.0f - t) * 0.9f;
        RoundedRect(x - grow, y - grow + size * 0.35f, w + grow * 2.0f, h + grow * 2.0f,
                    glm::vec3(0.0f), a, radius + grow);
    }
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

// Множитель шрифта: во сколько раз запечённый глиф больше или меньше того, что
// уйдёт на экран.
//
// НОРМИРУЕТСЯ ПО ВЫСОТЕ СТРОКИ, а не по высоте запекания атласа. Разница видна
// сразу: высота строки на экране становится РОВНО TextHeight(scale), то есть
// «размер шрифта 3» — это строка в 24 экранных пикселя у любого файла шрифта.
// Пока делили на высоту запекания, номинал тулкита (8·scale) и реальная строка
// расходились на метрики конкретного шрифта: текст оказывался заметно крупнее
// заданного, вёрстка считалась по одному числу, а рисовалась по другому — и от
// смены шрифта ехал весь экран.
//
// ОКРУГЛЕНИЕ ДО ЦЕЛОГО — ТОЛЬКО ПО ЯВНОЙ ПРОСЬБЕ (UITextStyle::SnapPixels).
// Раньше оно включалось само у «пиксельного» шрифта, и это давало худший из
// возможных ответов на попытку изменить кегль: у мелких размеров масштаб
// упирался в единицу и не менялся вовсе (текст стоял крупнее заданного), а
// потом скачком удваивался. Снаружи — «сначала текст подымается, потом
// становится вдвое больше».
float UIRenderer::FontScale(float scale, const Font* font, bool snapPixels) const {
    const Font* f = font ? font : m_font.get();
    if (!f) return scale;
    const float line = f->LineHeightUnits();
    const float raw = line > 0.0f ? (scale * m_scaleToPixels) / line
                                  : (scale * m_scaleToPixels) / f->PixelHeight();
    if (!snapPixels) return raw;
    // Целый масштаб сохраняет рисунок пиксельного шрифта: дробный растягивает
    // одни штрихи буквы на два экранных пикселя, а соседние на один, и «M»
    // выходит с ножками разной толщины. Цена — размер меняется ступенями,
    // поэтому это и есть отдельная настройка.
    return glm::max(1.0f, glm::floor(raw + 0.001f));
}

// Толщина синтетического жирного: доля от высоты строки, а не постоянное
// число пикселей. Постоянная давала бы жирную сноску и еле заметный заголовок
// — «жирность» обязана расти вместе с кеглем.
float UIRenderer::BoldOffset(float scale, const Font* font) const {
    const Font* f = font ? font : m_font.get();
    if (!f) return glm::max(1.0f, scale * 0.15f);
    return glm::max(1.0f, f->LineHeight(FontScale(scale, f)) * 0.035f);
}

const Font* UIRenderer::LoadFont(const std::string& path, float pixelHeight, bool pixelArt) {
    if (path.empty()) return nullptr;
    const std::string key =
        path + "|" + std::to_string((int)pixelHeight) + "|" + (pixelArt ? "1" : "0");
    auto it = m_fontCache.find(key);
    if (it != m_fontCache.end()) return it->second.get();
    std::unique_ptr<Font> font;
    try {
        // ПУТЬ РАЗРЕШАЕТСЯ ТАК ЖЕ, КАК У КАРТИНКИ ЭЛЕМЕНТА. Слот ассета хранит
        // ссылку ОТНОСИТЕЛЬНО ПРОЕКТА («assets/fonts/pixel.ttf»), а открывается
        // файл из текущего каталога процесса — у игры, редактора и тестов он
        // разный. Перевод делает база ассетов (LocatePath), и ровно им же
        // пользуется ResourceManager для текстур: своя выдумка здесь значила бы
        // шрифт, который виден в редакторе и пропадает в собранной игре.
        std::string file = sage::AssetDatabase::Instance().LocatePath(path);
        std::error_code ec;
        // Движковые шрифты лежат рядом с бинарником, а не в проекте.
        if (!std::filesystem::exists(file, ec)) {
            const std::string engineFile = sage::EngineAssetPath(path);
            if (std::filesystem::exists(engineFile, ec)) file = engineFile;
        }
        font = Font::Load(file, pixelHeight, pixelArt);
    } catch (const std::exception& e) {
        // Жалуемся ОДИН раз на файл: запись кладётся пустой, и следующий кадр
        // уже ничего не пытается открыть. Иначе битый путь у надписи на экране
        // — это строка в логе каждый кадр, то есть лог, в котором не найти
        // ничего другого.
        LOG_WARN("UIRenderer") << "шрифт не открылся (" << path << "): " << e.what();
    }
    const Font* raw = font.get();
    m_fontCache[key] = std::move(font);
    return raw;
}

void UIRenderer::Text(float x, float y, float scale, glm::vec3 color, const std::string& text,
                      float alpha, const UITextStyle& style) {
    if (text.empty()) return;

    const Font* font = FontOf(style);
    if (font) {
        CurrentSegment(nullptr, style.UseFont); // глифы идут в сегмент СВОЕГО шрифта
        // Масштаб API → множитель шрифта относительно базовой высоты запекания.
        const float fontScale = FontScale(scale, font, style.SnapPixels);
        std::vector<Font::PositionedGlyph> quads;
        font->BuildQuads(text, x, y, fontScale, quads);
        // Наклон курсива — 0.21 ≈ 12°, столько же берут настоящие курсивные
        // начертания. Базовая линия у строки одна: глифы наклоняются как одно
        // целое, а не каждый вокруг себя.
        const float slant = style.Italic ? 0.21f : 0.0f;
        const float baseline = y + font->Ascent(fontScale);
        size_t added = 0;
        for (const auto& q : quads) {
            PushGlyphQuad(q.x0, q.y0, q.x1, q.y1, q.uv0, q.uv1, color, alpha, slant, baseline);
            ++added;
        }
        // Жирный — ВТОРОЙ проход со сдвигом вправо: два оттиска буквы, смещённые
        // на доли кегля, дают ту же утолщённую форму, что и настоящий Bold, и
        // не требуют второго файла шрифта (у половины бесплатных наборов его
        // просто нет).
        if (style.Bold) {
            const float d = BoldOffset(scale, font);
            for (const auto& q : quads) {
                PushGlyphQuad(q.x0 + d, q.y0, q.x1 + d, q.y1, q.uv0, q.uv1, color, alpha, slant,
                              baseline);
                ++added;
            }
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

float UIRenderer::LineHeight(float scale, const UITextStyle& style) const {
    if (const Font* font = FontOf(style))
        return font->LineHeight(FontScale(scale, font, style.SnapPixels));
    return TextHeight(scale) * 1.6f; // без шрифта — векторный fallback
}

float UIRenderer::MeasureText(const std::string& text, float scale, const UITextStyle& style) const {
    if (text.empty()) return 0.0f;
    const Font* font = FontOf(style);
    // Жирный шире ровно на свой сдвиг, курсив — на наклон последней строчной
    // высоты. Без этих поправок текст, размеченный по ширине (перенос,
    // выравнивание по правому краю, «ширина по тексту»), обрезал бы себе
    // последнюю букву.
    float extra = 0.0f;
    if (style.Bold) extra += BoldOffset(scale, font);
    if (style.Italic) extra += LineHeight(scale, style) * 0.21f * 0.5f;
    if (font) return font->MeasureWidth(text, FontScale(scale, font, style.SnapPixels)) + extra;
    return stb_easy_font_width(const_cast<char*>(text.c_str())) * scale + extra;
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

    // Вершины лежат в пикселях ЭКРАНА ИНТЕРФЕЙСА; вид переводит их в пиксели
    // КАДРА, ortho — в нормализованные координаты. При тождественном виде это
    // ровно прежняя матрица.
    glm::mat4 proj = glm::ortho(0.0f, (float)m_fbWidth, (float)m_fbHeight, 0.0f, -1.0f, 1.0f);
    proj = glm::translate(proj, glm::vec3(m_viewOrigin, 0.0f));
    proj = glm::scale(proj, glm::vec3(m_viewScale, m_viewScale, 1.0f));
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
            // Ножницы работают в пикселях КАДРА, а маска задана в пикселях
            // экрана интерфейса — значит, к ней надо применить тот же вид, что
            // и к вершинам. Без этого при отдалении холста в редакторе списки
            // обрезались бы по своим прежним местам, а не по нарисованным.
            const float cx = m_viewOrigin.x + seg.Clip.x * m_viewScale;
            const float cy = m_viewOrigin.y + seg.Clip.y * m_viewScale;
            const float cw = seg.Clip.z * m_viewScale;
            const float ch = seg.Clip.w * m_viewScale;
            // glScissor ждёт левый НИЖНИЙ угол — переворачиваем Y.
            const int sy = m_fbHeight - (int)(cy + ch);
            device.SetScissor(true, (int)cx, sy, (int)cw, (int)ch);
        } else {
            device.SetScissor(false);
        }
        if (seg.Image) {
            m_shader.SetInt("uMode", 1);
            seg.Image->Bind(0);
        } else {
            m_shader.SetInt("uMode", 0);
            const Font* font = seg.TextFont ? seg.TextFont : m_font.get();
            if (font) font->Atlas().Bind(0);
        }
        m_geometry->DrawIndexedRange(seg.FirstQuad * 6, seg.QuadCount * 6);
    }
    device.SetScissor(false);

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthTest(true);
    // Смешивание снимает тот же проход, который его поднял, — как ножницы,
    // отсечение и тест глубины строкой выше.
    //
    // Здесь этой строки не было, и цена оказалась несоразмерной пропуску.
    // Смешивание, оставленное включённым, доживало до ПОСТ-ОБРАБОТКИ, а её
    // проходы пишут в буферы, которые ЖИВУТ МЕЖДУ КАДРАМИ (затенение,
    // свечение). Полноэкранный проход, который обязан ЗАМЕНИТЬ содержимое
    // буфера, вместо этого подмешивался к тому, что осталось с прошлого кадра,
    // — и каждый следующий кадр темнел сильнее предыдущего. Снаружи это
    // выглядит как «рендер сначала работает нормально, а потом сам собой
    // начинает деградировать», причём отладочные режимы вида остаются
    // исправными: они идут мимо пост-обработки.
    device.SetBlend(false);
}
