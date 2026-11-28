#include "sage/ui/UIPartsDetail.h"

#include <cmath>

#include "sage/render/ResourceManager.h"
#include "sage/ui/ShapePath.h"

// Progress: режимы Radial, Path, Custom Mask и заливка формой (см.
// UIPartsDetail.h). Linear рисует DrawBarPart (UIParts_Visual.cpp).
//
// ВСЕ ТРИ РЕЖИМА — ОДНИМ СПОСОБОМ: строится картинка ПОРЯДКА (когда какой
// пиксель заполняется), и маска шейдера показывает ту её часть, порядок
// которой меньше доли (UIRenderer::Mask::Ordered). Картинка зависит только от
// формы шкалы, а не от значения: полоса здоровья, меняющаяся каждый кадр, не
// перестраивает ничего.
namespace sage::ui {

float BarFraction(const Bar& bar) {
    const float v = bar.HasDisplayed ? bar.Displayed : bar.Value;
    const float span = bar.Max - bar.Min;
    if (std::fabs(span) < 1e-6f) return 0.0f;
    return std::clamp((v - bar.Min) / span, 0.0f, 1.0f);
}

namespace parts {
namespace {

constexpr int kMaxOrderSide = 256;

// Строки — снизу вверх, как у картинок из файла (так их читает маска).
std::shared_ptr<::Texture> MakeOrderTexture(const std::vector<float>& order, const std::vector<float>& cover,
                                            int w, int h) {
    std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t src = (size_t)y * (size_t)w + (size_t)x;
            unsigned char* px = &rgba[((size_t)(h - 1 - y) * (size_t)w + (size_t)x) * 4];
            px[0] = (unsigned char)std::lround(std::clamp(order[src], 0.0f, 1.0f) * 255.0f);
            px[3] = (unsigned char)std::lround(std::clamp(cover[src], 0.0f, 1.0f) * 255.0f);
        }
    return std::make_shared<::Texture>(rgba.data(), w, h, TextureFilter::Bilinear, false);
}

// Круг (сектор или кольцо): порядок — угол от начала в долях дуги.
void BuildRadial(const Bar& bar, int w, int h, float ringShare, std::vector<float>& order,
                 std::vector<float>& cover) {
    const float edge = (float)std::min(w, h) * 0.5f;   // пикселей текстуры на радиус
    const float inner = ringShare > 0.0f ? std::max(0.0f, 1.0f - ringShare) : -1.0f;
    const float arc = 360.0f * std::clamp(bar.FillAmount, 0.01f, 1.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f;
            const float py = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
            const float r = std::sqrt(px * px + py * py);
            float cov = std::clamp((1.0f - r) * edge + 0.5f, 0.0f, 1.0f);
            if (inner >= 0.0f) cov *= std::clamp((r - inner) * edge + 0.5f, 0.0f, 1.0f);
            // От верха по часовой: экранная ось y смотрит вниз.
            float a = std::atan2(px, -py) * 57.29578f - bar.StartAngle;
            if (!bar.Clockwise) a = -a;
            a = std::fmod(std::fmod(a, 360.0f) + 360.0f, 360.0f);
            const float o = a / arc;
            const size_t i = (size_t)y * (size_t)w + (size_t)x;
            order[i] = std::min(o, 1.0f);
            cover[i] = o > 1.0f ? 0.0f : cov;
        }
}

// Путь: порядок — пройденная длина до ближайшей точки пути.
void BuildPath(const Bar& bar, int w, int h, float halfWidth, std::vector<float>& order,
               std::vector<float>& cover) {
    std::vector<PathContour> contours;
    ParseShapePath(bar.Path, contours);
    const std::vector<Polyline> lines = FlattenShapePath(contours, UIRect{0, 0, (float)w, (float)h});
    struct Seg {
        glm::vec2 A, B;
        float Start, Len;
    };
    std::vector<Seg> segs;
    float total = 0.0f;
    for (const Polyline& l : lines) {
        const size_t n = l.Points.size();
        for (size_t i = 0; i + 1 < n + (l.Closed ? 1 : 0); ++i) {
            const glm::vec2 a = l.Points[i], b = l.Points[(i + 1) % n];
            const float len = glm::length(b - a);
            if (len <= 1e-4f) continue;
            segs.push_back({a, b, total, len});
            total += len;
        }
    }
    const float arc = std::clamp(bar.FillAmount, 0.01f, 1.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const glm::vec2 p{(float)x + 0.5f, (float)y + 0.5f};
            float best = 1e9f, at = 0.0f;
            for (const Seg& s : segs) {
                const glm::vec2 d = s.B - s.A;
                const float k = std::clamp(glm::dot(p - s.A, d) / (s.Len * s.Len), 0.0f, 1.0f);
                const float dist = glm::length(p - (s.A + d * k));
                if (dist < best) {
                    best = dist;
                    at = s.Start + s.Len * k;
                }
            }
            float o = total > 0.0f ? at / total : 0.0f;
            if (bar.Reverse) o = 1.0f - o;
            o /= arc;
            const size_t i = (size_t)y * (size_t)w + (size_t)x;
            order[i] = std::min(o, 1.0f);
            cover[i] = segs.empty() || o > 1.0f ? 0.0f : std::clamp(halfWidth - best + 0.5f, 0.0f, 1.0f);
        }
}

// Картинка порядка шкалы под её прямоугольник (экранный). nullptr — нечего
// показывать (нет пути, нет картинки).
std::shared_ptr<const Texture> OrderTexture(const PartDrawContext& c, const Bar& bar, const UIRect& inner, glm::vec4& uv) {
    uv = {0.0f, 1.0f, 1.0f, 0.0f};
    if (bar.Fill == Bar::Kind::CustomMask) {
        if (bar.MaskSpriteRef.Valid()) {
            auto sprite = SpritePicture(bar.MaskSpriteRef, TextureFiltering::Smooth);
            const auto* region = sprite.Data.Region(c.Reg ? SpriteTime(*c.Reg) : 0.0f);
            if (!region || !sprite.Image) return {};
            const auto rect = glm::vec4(region->Rect);
            uv = UIRenderer::MaskUV(*sprite.Image, {rect.x, rect.y, rect.z, rect.w});
            return sprite.Image;
        }
        return PictureTexture(bar.MaskImage, TextureFiltering::Smooth);
    }
    const float scale = c.Scale;
    const int w = std::clamp((int)std::ceil(inner.w), 8, kMaxOrderSide);
    const int h = std::clamp((int)std::ceil(inner.h), 8, kMaxOrderSide);
    // Толщины — в долях: текстура своего размера, экран — своего.
    const float ring = bar.RingWidth * scale / std::max(1.0f, std::min(inner.w, inner.h) * 0.5f);
    const float halfWidth = bar.PathWidth * scale * 0.5f * ((float)w / std::max(inner.w, 1.0f));
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d|%dx%d|%.2f|%d|%.3f|%.3f|%.2f|%d", (int)bar.Fill, w, h, bar.StartAngle,
                  bar.Clockwise ? 1 : 0, bar.FillAmount, ring, halfWidth, bar.Reverse ? 1 : 0);
    const std::string key = std::string(buf) + "|" + (bar.Fill == Bar::Kind::Path ? bar.Path : std::string());
    if (!bar.OrderTex || bar.OrderKey != key) {
        std::vector<float> order((size_t)w * (size_t)h, 1.0f), cover((size_t)w * (size_t)h, 0.0f);
        if (bar.Fill == Bar::Kind::Radial) BuildRadial(bar, w, h, ring, order, cover);
        else BuildPath(bar, w, h, halfWidth, order, cover);
        bar.OrderTex = MakeOrderTexture(order, cover, w, h);
        bar.OrderKey = key;
    }
    return bar.OrderTex;
}

} // namespace

void DrawProgressSource(const PartDrawContext& c, const Bar& bar, const Look& filled, const UIRect& inner) {
    if (bar.FillSource == Bar::Source::Shape) {
        // Заливка ФОРМОЙ элемента: та же геометрия, что у его оформления, —
        // сердце-шкала здоровья наливается сердцем.
        Shape s;
        if (const Shape* own = c.Sibling<Shape>()) s = *own;
        else s.Form = Shape::Geometry::RoundedRectangle, s.CornerRadius = filled.Rounding;
        s.Fill = Shape::FillKind::Solid;
        s.Color = filled.Color;
        s.StrokeWidth = 0.0f;
        DrawShape(c, s, inner);
        return;
    }
    DrawLook(c, filled, inner, /*tint=*/false);
}

void DrawProgressShaped(const PartDrawContext& c, const Bar& bar, const Look& filled, const UIRect& inner,
                        glm::vec2 range) {
    if (range.y <= range.x || inner.w <= 0.0f || inner.h <= 0.0f) return;
    glm::vec4 uv;
    auto tex = OrderTexture(c, bar, inner, uv);
    if (!tex) return;
    UIRenderer::Mask m;
    m.Rect = {inner.x, inner.y, inner.w, inner.h};
    m.Image = tex.get();
    m.TextureOwner = std::move(tex);
    m.UV = uv;
    m.Ordered = true;
    // Свой рисунок с другого конца — тот же порядок, видна верхняя часть.
    m.Range = bar.Fill == Bar::Kind::CustomMask && bar.Reverse ? glm::vec2(1.0f - range.y, 1.0f - range.x) : range;
    if (bar.Fill == Bar::Kind::CustomMask) m.Range *= std::clamp(bar.FillAmount, 0.01f, 1.0f);
    c.Ui->PushMask(m);
    DrawProgressSource(c, bar, filled, inner);
    c.Ui->PopMask();
}

} // namespace parts
} // namespace sage::ui
