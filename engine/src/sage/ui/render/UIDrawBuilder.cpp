#include "sage/ui/render/UIDrawBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "sage/render/Texture.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/debug/UIDebug.h"
#include "sage/ui/effects/UIEffect.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIIcon.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIShape.h"
#include "sage/ui/visual/UIText.h"
#include "sage/ui/visual/UITextLayout.h"
#include "sage/ui/widgets/UIWidgets.h"

namespace sage::ui {

UIRenderCommand& UIDrawContext::Begin(UIPrimitive kind) const {
    UIRenderCommand& c = List->Add();
    c.Kind = kind;
    c.Rect = Rect;
    c.Clip = Clip;
    c.Blend = Blend;
    c.SortKey = SortKey;
    c.Owner = Resolved ? Resolved->Id : 0;
    if (Resolved && Resolved->Transformed) {
        c.Transform = Resolved->World;
        c.Transformed = true;
    }
    return c;
}

namespace {

// Итоговый цвет: собственный цвет компонента × прозрачность узла × множитель
// эффектов уровня Modulate. Один расчёт на всех, чтобы «полупрозрачная панель»
// не означала «полупрозрачная везде, кроме текста».
UIColor Shade(const UIDrawContext& ctx, const UIColor& c) {
    UIColor out = c * ctx.Modulate;
    out.a *= ctx.Opacity;
    return out;
}

UICorners ScaledCorners(const UICorners& c, float s) {
    return UICorners(c.TL * s, c.TR * s, c.BR * s, c.BL * s);
}

// --- Эмиттеры встроенных компонентов ----------------------------------------

void EmitFill(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIFill& f = static_cast<const UIFill&>(comp);
    if (f.Type == UIFill::Kind::Texture && ctx.Ctx->Textures && !f.TexturePath.empty()) {
        UIRenderCommand& c = ctx.Begin(UIPrimitive::Image);
        c.Tex = ctx.Ctx->Textures->Get(f.TexturePath);
        c.Color = Shade(ctx, f.Color);
        c.Radius = ScaledCorners(f.Radius, ctx.Scale);
        c.Uv = {f.TextureOffset.x, f.TextureOffset.y, f.TextureScale.x, f.TextureScale.y};
        return;
    }
    if (f.Color.a <= 0.0f && !f.Gradient.Active()) return; // §130: невидимое ничего не стоит
    UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
    c.Color = Shade(ctx, f.Color);
    c.Radius = ScaledCorners(f.Radius, ctx.Scale);
    c.Softness = f.Softness * ctx.Scale;
    if (f.Type == UIFill::Kind::Gradient && f.Gradient.Active()) c.Gradient = f.Gradient;
}

void EmitBorder(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIBorder& b = static_cast<const UIBorder&>(comp);
    const float t = std::max(std::max(b.Thickness.L, b.Thickness.T),
                             std::max(b.Thickness.R, b.Thickness.B));
    if (t <= 0.0f || b.Color.a <= 0.0f) return;

    UICorners radius = b.Radius;
    if (radius.TL < 0.0f || radius.TR < 0.0f || radius.BR < 0.0f || radius.BL < 0.0f) {
        // «Взять у заливки» — не догадка рисующего, а объявленное правило
        // компонента: рамка чаще всего обводит именно её.
        UICorners fromFill(0.0f);
        if (ctx.Node) {
            if (const UIFill* f = ctx.Node->Get<UIFill>()) fromFill = f->Radius;
        }
        radius = fromFill;
    }
    UIRenderCommand& c = ctx.Begin(UIPrimitive::Border);
    c.Rect = UIInflate(ctx.Rect, UIEdges::Uniform(b.Inset * ctx.Scale));
    c.Color = Shade(ctx, b.Color);
    c.Radius = ScaledCorners(radius, ctx.Scale);
    c.Thickness = t * ctx.Scale;
    if (b.Gradient.Active()) c.Gradient = b.Gradient;
}

void EmitShape(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIShape& s = static_cast<const UIShape&>(comp);
    if (s.Color.a <= 0.0f && !s.Gradient.Active()) return;
    const UIRect& r = ctx.Rect;

    switch (s.Type) {
        case UIShape::Kind::Rectangle:
        case UIShape::Kind::RoundedRect:
        case UIShape::Kind::Circle:
        case UIShape::Kind::Ellipse: {
            UIRenderCommand& c = ctx.Begin(s.Thickness > 0.0f ? UIPrimitive::Border
                                                              : UIPrimitive::Rect);
            c.Color = Shade(ctx, s.Color);
            c.Softness = s.Softness * ctx.Scale;
            c.Thickness = s.Thickness * ctx.Scale;
            if (s.Gradient.Active()) c.Gradient = s.Gradient;
            if (s.Type == UIShape::Kind::Circle) {
                // Круг — квадрат по меньшей стороне, вписанный по центру: иначе
                // «круг» в широком узле оказывается эллипсом молча.
                const float d = std::min(r.w, r.h);
                c.Rect = {r.x + (r.w - d) * 0.5f, r.y + (r.h - d) * 0.5f, d, d};
                c.Radius = UICorners(d * 0.5f);
            } else if (s.Type == UIShape::Kind::Ellipse) {
                c.Radius = UICorners(std::min(r.w, r.h) * 0.5f);
            } else if (s.Type == UIShape::Kind::RoundedRect) {
                c.Radius = ScaledCorners(s.Radius, ctx.Scale);
            }
            break;
        }
        case UIShape::Kind::Ring:
        case UIShape::Kind::Arc: {
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Ring);
            c.Color = Shade(ctx, s.Color);
            c.Thickness = (s.Thickness > 0.0f ? s.Thickness : 4.0f) * ctx.Scale;
            c.StartAngle = s.StartAngle;
            c.SweepAngle = s.Type == UIShape::Kind::Ring ? 360.0f : s.SweepAngle;
            c.Softness = s.Softness * ctx.Scale;
            break;
        }
        case UIShape::Kind::Line: {
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Line);
            c.Color = Shade(ctx, s.Color);
            c.Thickness = std::max(1.0f, s.Thickness) * ctx.Scale;
            if (s.Points.size() >= 2) {
                for (const glm::vec2& p : s.Points)
                    c.Points.push_back({r.x + p.x * r.w, r.y + p.y * r.h});
            } else {
                c.Points = {{r.x, r.y + r.h * 0.5f}, {r.x + r.w, r.y + r.h * 0.5f}};
            }
            break;
        }
        case UIShape::Kind::Triangle:
        case UIShape::Kind::Polygon: {
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Polygon);
            c.Color = Shade(ctx, s.Color);
            c.Thickness = s.Thickness * ctx.Scale;
            if (!s.Points.empty()) {
                for (const glm::vec2& p : s.Points)
                    c.Points.push_back({r.x + p.x * r.w, r.y + p.y * r.h});
            } else {
                const int sides = s.Type == UIShape::Kind::Triangle ? 3 : std::max(3, s.Sides);
                const glm::vec2 centre = UICenter(r);
                const glm::vec2 rad{r.w * 0.5f, r.h * 0.5f};
                const float base = s.StartAngle * 3.14159265358979f / 180.0f;
                for (int i = 0; i < sides; ++i) {
                    const float a = base - 3.14159265358979f * 0.5f +
                                    (float)i * 6.283185307f / (float)sides;
                    c.Points.push_back({centre.x + std::cos(a) * rad.x,
                                        centre.y + std::sin(a) * rad.y});
                }
            }
            break;
        }
    }
}

void EmitImage(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIImage& img = static_cast<const UIImage&>(comp);
    const Texture* tex = img.Resolved;
    if (!tex && ctx.Ctx->Textures && !img.Path.empty()) tex = ctx.Ctx->Textures->Get(img.Path);
    // Отсутствующая картинка — не пустое место: рисуется заглушка, иначе
    // «забыл назначить» и «путь неверный» выглядят одинаково с «так задумано».
    if (!tex) {
        UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
        c.Color = Shade(ctx, UIColor(1.0f, 0.2f, 0.6f, 0.25f));
        c.Radius = ScaledCorners(img.Radius, ctx.Scale);
        return;
    }

    const glm::vec2 texSize{(float)tex->Width(), (float)tex->Height()};
    glm::vec4 uv = img.UvRect;
    if (img.SourceRect.z > 0.0f && img.SourceRect.w > 0.0f && texSize.x > 0.0f &&
        texSize.y > 0.0f) {
        // Спрайт задан в ПИКСЕЛЯХ исходника — перевод в доли делает движок
        // (§18), а не человек с калькулятором.
        uv = {img.SourceRect.x / texSize.x, img.SourceRect.y / texSize.y,
              (img.SourceRect.x + img.SourceRect.z) / texSize.x,
              (img.SourceRect.y + img.SourceRect.w) / texSize.y};
    } else {
        uv = {uv.x, uv.y, uv.z, uv.w};
    }
    if (img.FlipX) std::swap(uv.x, uv.z);
    if (img.FlipY) std::swap(uv.y, uv.w);

    UIRect rect = ctx.Rect;
    const float srcW = img.SourceRect.z > 0.0f ? img.SourceRect.z : texSize.x;
    const float srcH = img.SourceRect.w > 0.0f ? img.SourceRect.w : texSize.y;
    if (srcW > 0.0f && srcH > 0.0f) {
        const float ratio = srcW / srcH;
        switch (img.Fit) {
            case UIImageFit::Contain: {
                float w = rect.w, h = rect.w / ratio;
                if (h > rect.h) { h = rect.h; w = h * ratio; }
                rect = {rect.x + (rect.w - w) * 0.5f, rect.y + (rect.h - h) * 0.5f, w, h};
                break;
            }
            case UIImageFit::Cover: {
                float w = rect.w, h = rect.w / ratio;
                if (h < rect.h) { h = rect.h; w = h * ratio; }
                rect = {rect.x + (rect.w - w) * 0.5f, rect.y + (rect.h - h) * 0.5f, w, h};
                break;
            }
            case UIImageFit::None:
                rect = {rect.x + (rect.w - srcW * ctx.Scale) * 0.5f,
                        rect.y + (rect.h - srcH * ctx.Scale) * 0.5f, srcW * ctx.Scale,
                        srcH * ctx.Scale};
                break;
            default: break; // Stretch и Tile берут прямоугольник как есть
        }
    }

    const bool nine = !img.Slice.Empty();
    UIRenderCommand& c = ctx.Begin(nine ? UIPrimitive::NineSlice : UIPrimitive::Image);
    c.Rect = rect;
    c.Tex = tex;
    c.Uv = uv;
    c.Color = Shade(ctx, img.Tint);
    c.Radius = ScaledCorners(img.Radius, ctx.Scale);
    c.PixelArt = img.PixelArt;
    c.SourceSize = {srcW, srcH};
    if (nine) {
        const float k = img.SliceScale > 0.0f ? img.SliceScale : ctx.Scale;
        c.Slice = UIEdges(img.Slice.L * k, img.Slice.T * k, img.Slice.R * k, img.Slice.B * k);
        // §19: узел меньше суммы углов — углы ужимаются пропорционально, а не
        // рисуются внахлёст.
        if (img.SliceShrink) {
            const float hSum = c.Slice.Horizontal();
            if (hSum > rect.w && hSum > 0.0f) {
                const float f = rect.w / hSum;
                c.Slice.L *= f;
                c.Slice.R *= f;
            }
            const float vSum = c.Slice.Vertical();
            if (vSum > rect.h && vSum > 0.0f) {
                const float f = rect.h / vSum;
                c.Slice.T *= f;
                c.Slice.B *= f;
            }
        }
    }
    if (img.Fit == UIImageFit::Tile && srcW > 0.0f && srcH > 0.0f) {
        // Замощение выражается через UV: повторение делает выборка текстуры, а
        // не десятки квадов.
        const float rx = rect.w / (srcW * ctx.Scale);
        const float ry = rect.h / (srcH * ctx.Scale);
        c.Uv = {0.0f, 0.0f, rx, ry};
    }
}

void EmitIcon(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIIcon& icon = static_cast<const UIIcon&>(comp);
    if (icon.Name.empty() || icon.Color.a <= 0.0f) return;
    const float side = icon.Size > 0.0f ? icon.Size * ctx.Scale
                                        : std::min(ctx.Rect.w, ctx.Rect.h);
    UIMaterialRef material;
    material.Shader = "icon";
    material.Name = icon.Name;
    UIRenderCommand& c = ctx.Begin(UIPrimitive::Custom);
    // Значок всегда вписан в КВАДРАТ и рисуется целиком внутри него, поэтому
    // вёрстка не зависит от того, какой именно значок назначили.
    c.Rect = {ctx.Rect.x + (ctx.Rect.w - side) * 0.5f, ctx.Rect.y + (ctx.Rect.h - side) * 0.5f,
              side, side};
    c.Color = Shade(ctx, icon.Color);
    c.Material = ctx.List->AddMaterial(material);
}

void EmitText(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIText& t = static_cast<const UIText&>(comp);
    if (t.Color.a <= 0.0f) return;
    if (!ctx.Ctx->Fonts) return;

    // Раскладка текста считается в ЛОГИЧЕСКИХ единицах, потом умножается на
    // масштаб холста: иначе на дробном масштабе перенос по словам скачет от
    // кадра к кадру.
    const float scale = ctx.Scale;
    UIText scaled = t;
    scaled.Size = t.Size * scale;
    scaled.LetterSpacing = t.LetterSpacing * scale;
    scaled.MinSize = t.MinSize * scale;
    scaled.MaxSize = t.MaxSize * scale;
    scaled.ParagraphSpacing = t.ParagraphSpacing * scale;
    scaled.Padding = UIEdges(t.Padding.L * scale, t.Padding.T * scale, t.Padding.R * scale,
                             t.Padding.B * scale);
    const UITextLayoutResult layout = UILayoutText(*ctx.Ctx, scaled, ctx.Rect.w, ctx.Rect.h);
    if (layout.Glyphs.empty()) return;

    auto emitRun = [&](glm::vec2 shift, const UIColor& color, float softness) {
        UIRenderCommand& c = ctx.Begin(UIPrimitive::Glyphs);
        c.GlyphFirst = ctx.List->GlyphBase();
        c.Color = color;
        c.Softness = softness;
        for (const UIGlyphPlacement& g : layout.Glyphs) {
            UIGlyphDraw d;
            d.Codepoint = g.Codepoint;
            d.Pos = {ctx.Rect.x + g.X + shift.x, ctx.Rect.y + g.Baseline + shift.y};
            d.Size = layout.FontSize * (g.RunIndex >= 0 && g.RunIndex < (int)t.Runs.size()
                                            ? t.Runs[(size_t)g.RunIndex].SizeScale
                                            : 1.0f);
            d.Font = g.Font;
            UIColor gc = color;
            if (g.RunIndex >= 0 && g.RunIndex < (int)t.Runs.size()) {
                const UITextRun& run = t.Runs[(size_t)g.RunIndex];
                if (run.OverrideColor) {
                    gc = Shade(ctx, run.Color);
                    gc.a *= color.a / std::max(0.0001f, Shade(ctx, t.Color).a);
                }
            }
            d.Color = gc;
            ctx.List->AddGlyph(d);
        }
        c.GlyphCount = (int)layout.Glyphs.size();
    };

    // Порядок: тень → обводка → сам текст. Иначе тень ляжет поверх букв.
    if (t.ShadowColor.a > 0.0f && (t.ShadowOffset.x != 0.0f || t.ShadowOffset.y != 0.0f))
        emitRun(t.ShadowOffset * scale, Shade(ctx, t.ShadowColor), t.ShadowSoftness * scale);
    if (t.OutlineWidth > 0.0f && t.OutlineColor.a > 0.0f) {
        const float w = t.OutlineWidth * scale;
        const glm::vec2 dirs[8] = {{-w, 0}, {w, 0},  {0, -w},  {0, w},
                                   {-w, -w}, {w, -w}, {-w, w}, {w, w}};
        for (const glm::vec2& d : dirs) emitRun(d, Shade(ctx, t.OutlineColor), 0.0f);
    }
    emitRun({0.0f, 0.0f}, Shade(ctx, t.Color), 0.0f);
}

void EmitProgress(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIProgress& p = static_cast<const UIProgress&>(comp);
    const UIRect box = UIDeflate(ctx.Rect, UIEdges(p.Padding.L * ctx.Scale, p.Padding.T * ctx.Scale,
                                                   p.Padding.R * ctx.Scale,
                                                   p.Padding.B * ctx.Scale));
    if (p.TrackColor.a > 0.0f) {
        UIRenderCommand& track = ctx.Begin(UIPrimitive::Rect);
        track.Rect = box;
        track.Color = Shade(ctx, p.TrackColor);
        track.Radius = ScaledCorners(p.Radius, ctx.Scale);
    }
    const float t = p.Displayed >= 0.0f ? p.Displayed : p.Normalized();
    if (t <= 0.0f) return;

    UIRect fill = box;
    switch (p.Grow) {
        case UIProgress::Direction::LeftToRight: fill.w = box.w * t; break;
        case UIProgress::Direction::RightToLeft:
            fill.w = box.w * t;
            fill.x = UIRight(box) - fill.w;
            break;
        case UIProgress::Direction::TopToBottom: fill.h = box.h * t; break;
        case UIProgress::Direction::BottomToTop:
            fill.h = box.h * t;
            fill.y = UIBottom(box) - fill.h;
            break;
        case UIProgress::Direction::Radial: {
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Ring);
            c.Rect = box;
            c.Color = Shade(ctx, p.FillColor);
            c.Thickness = std::min(box.w, box.h) * 0.15f;
            c.StartAngle = 0.0f;
            c.SweepAngle = 360.0f * t;
            return;
        }
    }
    UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
    c.Rect = fill;
    c.Color = Shade(ctx, p.FillColor);
    c.Radius = ScaledCorners(p.Radius, ctx.Scale);
}

void EmitRange(const UIDrawContext& ctx, const UIComponent& comp) {
    const UIRangeValue& r = static_cast<const UIRangeValue&>(comp);
    const UIRect& box = ctx.Rect;
    const float t = r.Normalized();

    if (r.Toggle) {
        // Галка — квадрат по меньшей стороне плюс отметка. Отдельного вида
        // элемента для двух значений заводить незачем (§57).
        const float side = std::min(box.w, box.h);
        const UIRect sq{box.x, box.y + (box.h - side) * 0.5f, side, side};
        UIRenderCommand& bg = ctx.Begin(UIPrimitive::Rect);
        bg.Rect = sq;
        bg.Color = Shade(ctx, r.TrackColor);
        bg.Radius = ScaledCorners(r.Radius, ctx.Scale);
        if (t >= 0.5f) {
            UIRenderCommand& mark = ctx.Begin(UIPrimitive::Rect);
            mark.Rect = UIDeflate(sq, UIEdges::Uniform(side * 0.25f));
            mark.Color = Shade(ctx, r.AccentColor);
            mark.Radius = UICorners(side * 0.08f);
        }
        return;
    }

    const float thickness = std::min(r.Vertical ? box.w : box.h, 8.0f * ctx.Scale);
    UIRect track = box;
    if (r.Vertical) {
        track.x = box.x + (box.w - thickness) * 0.5f;
        track.w = thickness;
    } else {
        track.y = box.y + (box.h - thickness) * 0.5f;
        track.h = thickness;
    }
    UIRenderCommand& bg = ctx.Begin(UIPrimitive::Rect);
    bg.Rect = track;
    bg.Color = Shade(ctx, r.TrackColor);
    bg.Radius = UICorners(thickness * 0.5f);

    UIRect filled = track;
    if (r.Vertical) {
        filled.h = track.h * t;
        filled.y = UIBottom(track) - filled.h;
    } else {
        filled.w = track.w * t;
    }
    UIRenderCommand& fg = ctx.Begin(UIPrimitive::Rect);
    fg.Rect = filled;
    fg.Color = Shade(ctx, r.AccentColor);
    fg.Radius = UICorners(thickness * 0.5f);

    const float hs = r.HandleSize * ctx.Scale;
    const glm::vec2 centre =
        r.Vertical ? glm::vec2(track.x + track.w * 0.5f, UIBottom(track) - track.h * t)
                   : glm::vec2(track.x + track.w * t, track.y + track.h * 0.5f);
    UIRenderCommand& handle = ctx.Begin(UIPrimitive::Rect);
    handle.Rect = {centre.x - hs * 0.5f, centre.y - hs * 0.5f, hs, hs};
    handle.Color = Shade(ctx, r.AccentColor);
    handle.Radius = UICorners(hs * 0.5f);
}

void EmitTextField(const UIDrawContext& ctx, const UIComponent& comp) {
    const UITextField& f = static_cast<const UITextField&>(comp);
    if (!ctx.Ctx->Fonts || !ctx.Node) return;

    const UIText* style = ctx.Node->Get<UIText>();
    UIText t;
    if (style) t = *style;
    t.Key.clear();
    t.Wrap = f.Multiline ? UITextWrap::Word : UITextWrap::None;

    std::string shown = f.Value;
    if (f.Password) {
        // Звёздочки делает поле, а не игра: иначе игра хранит одно, а показывает
        // другое, и рано или поздно покажет не то.
        shown.clear();
        for (int i = 0, n = UIUtf8Length(f.Value); i < n; ++i) UIUtf8Append(shown, 0x2022);
    }
    const bool placeholder = shown.empty() && !f.PlaceholderKey.empty();
    t.Text = placeholder ? ctx.Ctx->Text(f.PlaceholderKey) : shown;
    if (placeholder) t.Color.a *= 0.45f;

    UIDrawContext sub = ctx;
    EmitText(sub, t);

    // Каретка. Мигание — свойство состояния поля, а не «магия рисующего».
    const bool focused = ctx.Node->Get<UIInteraction>() &&
                         ctx.Node->Get<UIInteraction>()->Is(UIState_Focused);
    if (focused && f.Blink < 0.5f) {
        UIText scaled = t;
        scaled.Size = t.Size * ctx.Scale;
        const UITextLayoutResult layout = UILayoutText(*ctx.Ctx, scaled, ctx.Rect.w, ctx.Rect.h);
        float h = scaled.Size;
        const glm::vec2 caret = UITextCaretPos(layout, f.Caret, h);
        UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
        c.Rect = {ctx.Rect.x + caret.x, ctx.Rect.y + caret.y, std::max(1.0f, ctx.Scale), h};
        c.Color = Shade(ctx, t.Color);
    }
}

} // namespace

// --- Реестр эмиттеров -------------------------------------------------------

UIDrawRegistry& UIDrawRegistry::Instance() {
    static UIDrawRegistry r;
    return r;
}

void UIDrawRegistry::Register(std::string_view componentId, UIDrawEmitter emitter) {
    for (auto& e : m_entries)
        if (e.Id == componentId) { e.Fn = emitter; return; }
    m_entries.push_back({std::string(componentId), emitter});
}

UIDrawEmitter UIDrawRegistry::Find(std::string_view componentId) const {
    EnsureBuiltins();
    for (const auto& e : m_entries)
        if (e.Id == componentId) return e.Fn;
    return nullptr;
}

void UIDrawRegistry::EnsureBuiltins() const {
    if (m_builtinsDone) return;
    m_builtinsDone = true;
    RegisterBuiltinUIEmitters();
}

void RegisterBuiltinUIEmitters() {
    static bool done = false;
    if (done) return;
    done = true;
    UIDrawRegistry& r = UIDrawRegistry::Instance();
    r.Register("fill", &EmitFill);
    r.Register("border", &EmitBorder);
    r.Register("shape", &EmitShape);
    r.Register("image", &EmitImage);
    r.Register("icon", &EmitIcon);
    r.Register("text", &EmitText);
    r.Register("progress", &EmitProgress);
    r.Register("range", &EmitRange);
    r.Register("textfield", &EmitTextField);
}

// --- Сборка кадра -----------------------------------------------------------

namespace {

// Эффекты уровня Modulate складываются в один множитель цвета: за них не
// платят ни проходом, ни целью (§131).
UIColor CollectModulate(const UINode& node) {
    UIColor m(1.0f);
    const UIEffects* fx = node.Get<UIEffects>();
    if (!fx) return m;
    for (const auto& e : fx->Items) {
        if (!e->Enabled || e->Type().Stage != UIEffectStage::Modulate) continue;
        if (&e->Type() == &UIColorEffect::StaticType()) {
            const UIColorEffect& ce = static_cast<const UIColorEffect&>(*e);
            m *= ce.Tint;
            m.r *= ce.Brightness;
            m.g *= ce.Brightness;
            m.b *= ce.Brightness;
            m.a *= ce.Opacity;
        }
    }
    return m;
}

// Форма узла, вокруг которой строятся тень и свечение (§41: вокруг ФАКТИЧЕСКОЙ
// формы, а не вокруг прямоугольника).
UICorners NodeShapeRadius(const UINode& node, float scale) {
    if (const UIFill* f = node.Get<UIFill>()) return ScaledCorners(f->Radius, scale);
    if (const UIImage* i = node.Get<UIImage>()) return ScaledCorners(i->Radius, scale);
    if (const UIShape* s = node.Get<UIShape>()) {
        if (s->Type == UIShape::Kind::Circle || s->Type == UIShape::Kind::Ellipse)
            return UICorners(9999.0f);
        return ScaledCorners(s->Radius, scale);
    }
    return UICorners(0.0f);
}

void EmitBehindEffects(const UIDrawContext& ctx, const UINode& node) {
    const UIEffects* fx = node.Get<UIEffects>();
    if (!fx) return;
    const UICorners radius = NodeShapeRadius(node, ctx.Scale);
    for (const auto& e : fx->Items) {
        if (!e->Enabled || e->Type().Stage != UIEffectStage::Behind) continue;
        if (&e->Type() == &UIDropShadow::StaticType()) {
            const UIDropShadow& s = static_cast<const UIDropShadow&>(*e);
            // ОДНА команда с мягким краем, а не десяток расширяющихся
            // прямоугольников (§39): десяток — это десяток квадов на каждую
            // панель и заметная ступенчатость на большом радиусе.
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
            c.Rect = UIInflate(ctx.Rect, UIEdges::Uniform(s.Spread * ctx.Scale));
            c.Rect.x += s.Offset.x * ctx.Scale;
            c.Rect.y += s.Offset.y * ctx.Scale;
            c.Color = Shade(ctx, s.Color);
            c.Radius = radius;
            c.Softness = std::max(0.5f, s.Blur * ctx.Scale);
        } else if (&e->Type() == &UIGlow::StaticType()) {
            const UIGlow& g = static_cast<const UIGlow&>(*e);
            if (g.Inner) continue; // внутреннее свечение рисуется поверх
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
            c.Rect = UIInflate(ctx.Rect, UIEdges::Uniform(g.Radius * 0.35f * ctx.Scale));
            UIColor col = Shade(ctx, g.Color);
            col.a *= g.Intensity;
            c.Color = col;
            c.Radius = radius;
            c.Softness = std::max(0.5f, g.Radius * ctx.Scale);
            c.Blend = UIBlendMode::Add; // свечение складывается, а не закрывает
        }
    }
}

void EmitFrontEffects(const UIDrawContext& ctx, const UINode& node) {
    const UIEffects* fx = node.Get<UIEffects>();
    if (!fx) return;
    const UICorners radius = NodeShapeRadius(node, ctx.Scale);
    for (const auto& e : fx->Items) {
        if (!e->Enabled || e->Type().Stage != UIEffectStage::Front) continue;
        if (&e->Type() == &UIInnerShadow::StaticType()) {
            const UIInnerShadow& s = static_cast<const UIInnerShadow&>(*e);
            UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
            c.Rect = ctx.Rect;
            c.Rect.x += s.Offset.x * ctx.Scale;
            c.Rect.y += s.Offset.y * ctx.Scale;
            c.Color = Shade(ctx, s.Color);
            c.Radius = radius;
            c.Softness = std::max(0.5f, s.Blur * ctx.Scale);
            c.Inner = true; // тень считается ВНУТРЬ фигуры
            c.Thickness = s.Spread * ctx.Scale;
        }
    }
    for (const auto& e : fx->Items) {
        if (!e->Enabled) continue;
        if (&e->Type() != &UIGlow::StaticType()) continue;
        const UIGlow& g = static_cast<const UIGlow&>(*e);
        if (!g.Inner) continue;
        UIRenderCommand& c = ctx.Begin(UIPrimitive::Rect);
        c.Color = Shade(ctx, g.Color);
        c.Radius = radius;
        c.Softness = std::max(0.5f, g.Radius * ctx.Scale);
        c.Inner = true;
        c.Blend = UIBlendMode::Add;
    }
}

} // namespace

void UIBuildDrawList(UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
                     UIRenderList& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out.Clear();
    RegisterBuiltinUIEmitters();

    // Узлы уже посчитаны и уже в правильном порядке обхода; остаётся
    // отсортировать по ключу (§26). Сортируем НОМЕРА, а не сами узлы: они
    // тяжёлые, а порядок нужен только здесь.
    std::vector<int> order;
    order.reserve(layout.Nodes().size());
    for (int i = 0; i < (int)layout.Nodes().size(); ++i) {
        const UIResolvedNode& r = layout.Nodes()[(size_t)i];
        if (!r.Visible || r.Culled || r.Opacity <= 0.001f) continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return layout.Nodes()[(size_t)a].SortKey < layout.Nodes()[(size_t)b].SortKey;
    });
    out.Stats().CulledNodes = layout.Stats().Culled;

    const UIDrawRegistry& reg = UIDrawRegistry::Instance();

    for (int idx : order) {
        const UIResolvedNode& r = layout.Nodes()[(size_t)idx];
        UINode* node = doc.Find(r.Id);
        if (!node) continue;

        UIDrawContext dc;
        dc.Ctx = &ctx;
        dc.Node = node;
        dc.Resolved = &r;
        dc.List = &out;
        dc.Rect = r.Rect;
        dc.Scale = r.Scale;
        dc.Opacity = r.Opacity;
        dc.Blend = r.Blend;
        dc.SortKey = r.SortKey;
        dc.Modulate = CollectModulate(*node);
        dc.Clip.HasScissor = r.Clipped;
        dc.Clip.Scissor = r.Clip;
        dc.Clip.MaskState = r.MaskState;

        const UIEffects* fx = node->Get<UIEffects>();
        const bool offscreen = fx && fx->NeedsOffscreen() && ctx.AllowOffscreen;
        if (offscreen) {
            // Явная стоимость (§131): промежуточная цель появляется в списке
            // команд отдельной операцией и видна в профайлере.
            UIRenderCommand& begin = dc.Begin(UIPrimitive::Custom);
            begin.Op = UIPassOp::BeginOffscreen;
        }

        EmitBehindEffects(dc, *node);

        for (UIComponent* comp : node->DrawOrder()) {
            if (UIDrawEmitter fn = reg.Find(comp->Type().Id)) fn(dc, *comp);
        }

        EmitFrontEffects(dc, *node);

        if (offscreen) {
            UIRenderCommand& end = dc.Begin(UIPrimitive::Custom);
            end.Op = UIPassOp::EndOffscreen;
        }
    }

    if (ctx.Debug != UIDebug_None) UIAppendDebugOverlay(doc, layout, ctx, out);

    out.Build();
    const auto t1 = std::chrono::steady_clock::now();
    out.Stats().PrepareMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace sage::ui
