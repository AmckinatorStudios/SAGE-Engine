// ---------------------------------------------------------------------------
// ВСТРОЕННЫЕ ЧАСТИ ЭЛЕМЕНТА — по одной регистрации на каждую.
//
// Здесь и только здесь записано, ЧТО такое подложка, картинка, текст, значок,
// шкала, поле ввода. Движок про них не знает: он перебирает реестр (UIPart.h).
// Своя часть добавляется таким же файлом рядом — с таблицей полей, функцией
// отрисовки и вызовом RegisterPart, — и сама появляется в отрисовке, в записи
// сцены и в редакторе.
//
// ПРАВИЛО, КОТОРОЕ ДЕРЖИТ ВСЮ СИСТЕМУ: часть рисует себя в прямоугольнике
// элемента и НЕ ДВИГАЕТ соседей. Раньше было наоборот — значок сдвигал текст,
// галка сдвигала текст, картинка отменяла подложку, — и каждая новая часть
// обязана была вписаться в этот клубок. Отсюда же следует, как собирать
// «значок слева, подпись справа»: это не одна хитрая часть, а контейнер с
// раскладкой и два ОБЪЕКТА внутри.
// ---------------------------------------------------------------------------
#include "sage/ui/UIPart.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

namespace sage::ui {

namespace {

// --- Общее для всех частей --------------------------------------------------

// Прозрачность части: своя альфа, помноженная на групповую и на бледность
// выключенного элемента.
float AlphaOf(const PartDrawContext& c, float channelAlpha) {
    float a = channelAlpha * c.Alpha;
    if (!c.Enabled) {
        const Interactable* act = c.Sibling<Interactable>();
        a *= act ? act->DisabledAlpha : 0.45f;
    }
    return a;
}

// Подкраска под состояние — для тех, у кого нет своих картинок состояний.
// Множители берутся у самого элемента: «на 12% ярче» подходит не всякому набору.
glm::vec3 StateTint(const PartDrawContext& c, glm::vec3 base) {
    const Interactable* act = c.Sibling<Interactable>();
    if (!act) return base;
    if (!c.Enabled) return glm::mix(base, glm::vec3(0.5f), 0.5f);
    if (c.Pressed) return base * act->PressedBrightness;
    if (c.Hovered) return glm::mix(base, glm::vec3(1.0f), act->HoverBrightness - 1.0f);
    return base;
}

// Скругление подложки, если она есть. Спрашивают трое — картинка-заглушка,
// маска и галка, — и все ЯВНО: это единственный законный вид знания о соседе.
float RoundingOf(const PartDrawContext& c) {
    const Fill* fill = c.Sibling<Fill>();
    return fill ? fill->Rounding * c.Scale : 0.0f;
}

void FillRectImpl(const Fill& fill, const UIRect& r, float rounding, glm::vec3 rgb, float alpha,
                  UIRenderer& ui) {
    if (alpha <= 0.0f) return;
    if (fill.Gradient.a > 0.0f) {
        ui.GradientRect(r.x, r.y, r.w, r.h, rgb, {fill.Gradient.r, fill.Gradient.g, fill.Gradient.b},
                        alpha, fill.Gradient.a * (alpha / std::max(fill.Color.a, 1e-4f)), rounding);
    } else {
        ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha, rounding);
    }
}

// --- Подложка ---------------------------------------------------------------

const std::vector<PartField>& FillFields() {
    static const std::vector<PartField> f = {
        {"color", SAGE_UI_TEXT("Colour"), PartField::Kind::Color, offsetof(Fill, Color)},
        {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Fill, Rounding), 0.0f, 64.0f},
        {"borderThickness", SAGE_UI_TEXT("Border width"), PartField::Kind::Float,
         offsetof(Fill, BorderThickness), 0.0f, 16.0f},
        {"borderColor", SAGE_UI_TEXT("Border colour"), PartField::Kind::Color, offsetof(Fill, BorderColor)},
        {"gradient", SAGE_UI_TEXT("Gradient down"), PartField::Kind::Color, offsetof(Fill, Gradient), 0.0f, 1.0f,
         "Alpha 0 means a flat fill. Flat panels are the first thing that makes\n"
         "an interface look unfinished."},
        {"shadowSize", SAGE_UI_TEXT("Shadow"), PartField::Kind::Float, offsetof(Fill, ShadowSize), 0.0f, 48.0f,
         "Separates the interface from the scene: without it a panel blends into\n"
         "a busy background."},
        {"shadowColor", SAGE_UI_TEXT("Shadow colour"), PartField::Kind::Color, offsetof(Fill, ShadowColor)},
    };
    return f;
}

void DrawFill(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    const glm::vec3 rgb = StateTint(c, glm::vec3(fill.Color));
    FillRectImpl(fill, c.Rect, fill.Rounding * c.Scale, rgb, AlphaOf(c, fill.Color.a), *c.Ui);
}

// Тень и рамка — разными слоями: тень под всем, рамка поверх всего. Иначе
// картинка ложилась бы на рамку, а тень — на соседний элемент.
void DrawFillShadow(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    if (fill.ShadowSize <= 0.0f) return;
    c.Ui->RectShadow(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, fill.Rounding * c.Scale,
                     fill.ShadowSize * c.Scale);
}

void DrawFillBorder(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    const float t = fill.BorderThickness * c.Scale;
    if (t <= 0.0f || fill.BorderColor.a <= 0.0f) return;
    const glm::vec4& b = fill.BorderColor;
    c.Ui->RoundedRectOutline(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, fill.Rounding * c.Scale, t,
                             {b.r, b.g, b.b}, AlphaOf(c, b.a));
}

// --- Картинка ---------------------------------------------------------------

const std::vector<PartField>& ImageFields() {
    static const std::vector<PartField> f = {
        {"path", SAGE_UI_TEXT("File"), PartField::Kind::String, offsetof(Image, Path), 0.0f, 0.0f, nullptr,
         nullptr, 0, PartField::Widget::Texture},
        {"tint", SAGE_UI_TEXT("Tint"), PartField::Kind::Color, offsetof(Image, Tint)},
        {"sprite", SAGE_UI_TEXT("Sprite (x,y,w,h)"), PartField::Kind::Vec4, offsetof(Image, Sprite), 0.0f, 4096.0f,
         "A piece of the sheet in source pixels; width 0 means the whole file."},
        {"sliceBorder", SAGE_UI_TEXT("9-slice (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Image, SliceBorder),
         0.0f, 512.0f,
         "Fixed corners in source pixels. Without it a 48x48 panel cannot be\n"
         "stretched to 300x120 — the corners smear along with the middle."},
        {"pixelScale", SAGE_UI_TEXT("Pixel scale"), PartField::Kind::Float, offsetof(Image, PixelScale), 0.0f,
         16.0f, "0 picks it automatically."},
        {"pixelArt", SAGE_UI_TEXT("Pixel art"), PartField::Kind::Bool, offsetof(Image, PixelArt), 0.0f, 1.0f,
         "Nearest neighbour and no mipmaps."},
        {"spriteHover", SAGE_UI_TEXT("Sprite on hover"), PartField::Kind::Vec4, offsetof(Image, SpriteHover),
         0.0f, 4096.0f},
        {"spritePressed", SAGE_UI_TEXT("Sprite when pressed"), PartField::Kind::Vec4, offsetof(Image, SpritePressed),
         0.0f, 4096.0f},
    };
    return f;
}

UIRenderer::Sprite StateSprite(const PartDrawContext& c, const Image& img) {
    const glm::vec4* src = &img.Sprite;
    if (c.Pressed && img.SpritePressed.z > 0.0f) src = &img.SpritePressed;
    else if (c.Hovered && img.SpriteHover.z > 0.0f) src = &img.SpriteHover;
    return {src->x, src->y, src->z, src->w};
}

void DrawImagePart(const PartDrawContext& c) {
    const Image& img = *static_cast<const Image*>(c.Data);
    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const glm::vec3 rgb = StateTint(c, glm::vec3(img.Tint));
    const float alpha = AlphaOf(c, img.Tint.a);

    if (!img.Tex) {
        // Пустой путь — картинки просто нет, и рисовать нечего. Заглушка тут
        // была бы хуже пустоты: она закрасила бы элемент и спрятала остальное.
        if (img.Path.empty()) return;
        // Путь задан, а текстуры нет — вот это стоит показать: иначе «не
        // загрузилось» и «не назначено» выглядят одинаково.
        ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha, RoundingOf(c));
        return;
    }

    const UIRenderer::Sprite src = StateSprite(c, img);
    const bool sliced = img.SliceBorder.x > 0.0f || img.SliceBorder.y > 0.0f ||
                        img.SliceBorder.z > 0.0f || img.SliceBorder.w > 0.0f;
    if (sliced) {
        // Масштаб пикселя: 0 — «подобрать сам». Для пиксель-арта округляется
        // ВНИЗ до целого: дробный масштаб растягивает одни пиксели исходника на
        // два экранных, а соседние на один, и ровная рамка идёт волнами.
        float pixels = img.PixelScale * c.Scale;
        if (pixels <= 0.0f) {
            const float srcH = src.Whole() ? (float)img.Tex->Height() : src.H;
            pixels = srcH > 0.0f ? r.h / srcH : 1.0f;
            if (img.PixelArt) pixels = std::max(1.0f, std::floor(pixels));
        }
        ui.ImageNineSlice(r.x, r.y, r.w, r.h, img.Tex.get(), src, img.SliceBorder, pixels, rgb,
                          alpha);
        return;
    }

    // Спрайт БЕЗ девятины: пиксель-арт нельзя просто растянуть под элемент —
    // разный дробный масштаб по осям даёт рваные края. Берём ЦЕЛЫЙ масштаб и
    // ставим по центру: элемент может остаться больше картинки, и это честнее.
    UIRect dst = r;
    float pixels = img.PixelScale * c.Scale;
    if (img.PixelArt || pixels > 0.0f) {
        const float sw = src.Whole() ? (float)img.Tex->Width() : src.W;
        const float sh = src.Whole() ? (float)img.Tex->Height() : src.H;
        if (sw > 0.0f && sh > 0.0f) {
            if (pixels <= 0.0f) {
                pixels = std::min(r.w / sw, r.h / sh);
                if (img.PixelArt) pixels = std::max(1.0f, std::floor(pixels));
            }
            dst.w = sw * pixels;
            dst.h = sh * pixels;
            dst.x = r.x + std::floor((r.w - dst.w) * 0.5f);
            dst.y = r.y + std::floor((r.h - dst.h) * 0.5f);
        }
    }
    ui.ImageSprite(dst.x, dst.y, dst.w, dst.h, img.Tex.get(), src, rgb, alpha);
}

// --- Шкала ------------------------------------------------------------------

const char* const kBarGrow[] = {SAGE_UI_TEXT("Right"), SAGE_UI_TEXT(SAGE_UI_TEXT("Left")), SAGE_UI_TEXT("Up"), SAGE_UI_TEXT("Down")};

const std::vector<PartField>& BarFields() {
    static const std::vector<PartField> f = {
        {"value", SAGE_UI_TEXT("Value"), PartField::Kind::Float, offsetof(Bar, Value), 0.0f, 1.0f},
        {"fillColor", SAGE_UI_TEXT("Fill colour"), PartField::Kind::Color, offsetof(Bar, FillColor)},
        {"grow", SAGE_UI_TEXT("Grows"), PartField::Kind::Enum, offsetof(Bar, Grow), 0.0f, 0.0f,
         "A vertical gauge (mana at the side, a volume column) is impossible without it.",
         kBarGrow, 4},
        {"smoothing", SAGE_UI_TEXT("Smoothing"), PartField::Kind::Float, offsetof(Bar, Smoothing), 0.0f, 8.0f,
         "Units per second; 0 is instant. A health bar that jumps reads worse\n"
         "than one that travels over a quarter of a second."},
    };
    return f;
}

float BarShown(const Bar& bar) {
    return (bar.Smoothing > 0.0f && bar.Displayed >= 0.0f) ? bar.Displayed : bar.Value;
}

void DrawBarPart(const PartDrawContext& c) {
    const Bar& bar = *static_cast<const Bar*>(c.Data);
    const UIRect& r = c.Rect;
    const float t = std::clamp(BarShown(bar), 0.0f, 1.0f);
    const float alpha = AlphaOf(c, bar.FillColor.a);
    if (t <= 0.0f || alpha <= 0.0f) return;

    const float pad = std::min(2.0f, std::min(r.w, r.h) * 0.15f);
    const float rounding = std::max(RoundingOf(c) - pad, 0.0f);
    const UIRect inner{r.x + pad, r.y + pad, r.w - pad * 2.0f, r.h - pad * 2.0f};
    UIRect fillRect = inner;
    switch (bar.Grow) {
        case Bar::Direction::LeftToRight: fillRect.w = inner.w * t; break;
        case Bar::Direction::RightToLeft:
            fillRect.w = inner.w * t;
            fillRect.x = inner.x + inner.w - fillRect.w;
            break;
        case Bar::Direction::BottomToTop:
            fillRect.h = inner.h * t;
            fillRect.y = inner.y + inner.h - fillRect.h;
            break;
        case Bar::Direction::TopToBottom: fillRect.h = inner.h * t; break;
    }
    const glm::vec3 fill{bar.FillColor.r, bar.FillColor.g, bar.FillColor.b};
    // Заполнение всегда с градиентом к более тёмному краю: плоская полоса
    // выглядит нарисованной в редакторе, а не «налитой».
    c.Ui->GradientRect(fillRect.x, fillRect.y, fillRect.w, fillRect.h,
                       glm::mix(fill, glm::vec3(1.0f), 0.22f), fill * 0.78f, alpha, alpha,
                       rounding);
}

// --- Значок -----------------------------------------------------------------

const std::vector<PartField>& IconFields() {
    static const std::vector<PartField> f = {
        {"name", SAGE_UI_TEXT("Icon name"), PartField::Kind::String, offsetof(Icon, Name), 0.0f, 0.0f,
         nullptr, nullptr, 0, PartField::Widget::IconName},
        {"color", SAGE_UI_TEXT("Colour"), PartField::Kind::Color, offsetof(Icon, Color)},
        {"size", SAGE_UI_TEXT("Size"), PartField::Kind::Float, offsetof(Icon, Size), 0.0f, 256.0f,
         "0 means the shorter side of the element."},
    };
    return f;
}

void DrawIconInto(UIRenderer& ui, const std::string& name, float x, float y, float size,
                  glm::vec3 color, float alpha) {
    DrawIcon(ui, name, x, y, size, color, alpha);
}

void DrawIconPart(const PartDrawContext& c) {
    const Icon& icon = *static_cast<const Icon*>(c.Data);
    if (icon.Name.empty() || icon.Color.a <= 0.0f) return;
    const UIRect& r = c.Rect;
    // ЗНАЧОК ЗАНИМАЕТ СВОЙ ЭЛЕМЕНТ ЦЕЛИКОМ и ничего не сдвигает. Раньше он
    // отодвигал текст соседа вправо, и «значок с подписью» был встроенным
    // случаем; теперь это два объекта в контейнере с раскладкой.
    const float side = icon.Size > 0.0f ? icon.Size * c.Scale : std::min(r.w, r.h);
    const glm::vec3 rgb = StateTint(c, glm::vec3(icon.Color));
    DrawIconInto(*c.Ui, icon.Name, r.x + (r.w - side) * 0.5f, r.y + (r.h - side) * 0.5f, side, rgb,
                 AlphaOf(c, icon.Color.a));
}

// --- Текст ------------------------------------------------------------------

const char* const kAlign[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT(SAGE_UI_TEXT("Center")), SAGE_UI_TEXT("End")};

const std::vector<PartField>& LabelFields() {
    static const std::vector<PartField> f = {
        {"text", SAGE_UI_TEXT("Text"), PartField::Kind::String, offsetof(Label, Text), 0.0f, 0.0f, nullptr,
         nullptr, 0, PartField::Widget::Multiline},
        {"scale", SAGE_UI_TEXT("Font size"), PartField::Kind::Float, offsetof(Label, Scale), 0.5f, 12.0f},
        {"color", SAGE_UI_TEXT("Colour"), PartField::Kind::Color, offsetof(Label, Color)},
        {"horizontal", SAGE_UI_TEXT("Horizontal"), PartField::Kind::Enum, offsetof(Label, Horizontal), 0.0f,
         0.0f, nullptr, kAlign, 3},
        {"vertical", SAGE_UI_TEXT("Vertical"), PartField::Kind::Enum, offsetof(Label, Vertical), 0.0f, 0.0f,
         nullptr, kAlign, 3},
        {"wrap", SAGE_UI_TEXT("Wrap"), PartField::Kind::Bool, offsetof(Label, Wrap)},
        {"autoWidth", SAGE_UI_TEXT("Width from text"), PartField::Kind::Bool, offsetof(Label, AutoWidth)},
        {"padX", SAGE_UI_TEXT("Side padding"), PartField::Kind::Float, offsetof(Label, PadX), 0.0f, 64.0f},
    };
    return f;
}

float AlignX(Label::Align a, float left, float avail, float textWidth) {
    switch (a) {
        case Label::Align::Center: return left + (avail - textWidth) * 0.5f;
        case Label::Align::End: return left + avail - textWidth;
        default: return left;
    }
}

float AlignY(Label::Align a, const UIRect& r, float blockH) {
    switch (a) {
        case Label::Align::Start: return r.y;
        case Label::Align::End: return r.y + r.h - blockH;
        default: return r.y + (r.h - blockH) * 0.5f;
    }
}

void DrawLabelPart(const PartDrawContext& c) {
    // ЕДИНСТВЕННОЕ место, где часть смотрит на соседа не ради его чисел, а
    // ради того, рисовать ли себя вообще. У поля ввода содержимое показывает
    // САМО ПОЛЕ: оно бывает скрыто точками, заменено подсказкой и обрезано
    // кареткой, и текст об этом знать не должен.
    if (c.Sibling<TextInput>()) return;

    const Label& label = *static_cast<const Label*>(c.Data);
    if (label.Text.empty() || label.Color.a <= 0.0f) return;

    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const float textScale = label.Scale * c.Scale;
    const float padX = label.PadX * c.Scale;
    const float left = r.x + padX;
    const float avail = std::max(r.w - padX * 2.0f, 1.0f);
    const glm::vec3 rgb{label.Color.r, label.Color.g, label.Color.b};
    const float alpha = AlphaOf(c, label.Color.a);

    std::vector<std::string> lines;
    if (label.Wrap) lines = WrapLines(label.Text, avail, textScale, ui);
    else if (label.Text.find('\n') != std::string::npos)
        lines = WrapLines(label.Text, 0.0f, textScale, ui); // только по явным \n
    else lines.push_back(label.Text);

    const float lineH = ui.LineHeight(textScale);
    const float blockH = lineH * (float)(lines.size() - 1) + ui.TextHeight(textScale);
    float y = AlignY(label.Vertical, r, blockH);

    // Перенесённый текст ОБРЕЗАЕТСЯ своим элементом: он переносился под эту
    // ширину, значит и по высоте обязан остаться внутри. Абзац, вылезший на
    // фон, выглядит хуже обрезанного — и не читается как ошибка вёрстки.
    const bool clip = label.Wrap && blockH > r.h;
    if (clip) ui.PushClipRect(r.x, r.y, r.w, r.h);
    for (const std::string& line : lines) {
        if (!line.empty()) {
            ui.Text(AlignX(label.Horizontal, left, avail, ui.MeasureText(line, textScale)), y,
                    textScale, rgb, line, alpha);
        }
        y += lineH;
    }
    if (clip) ui.PopClipRect();
}

// --- Диапазон: галка и ползунок ---------------------------------------------

const std::vector<PartField>& RangeFields() {
    static const std::vector<PartField> f = {
        {"min", SAGE_UI_TEXT("Minimum"), PartField::Kind::Float, offsetof(Range, Min), -1000.0f, 1000.0f},
        {"max", SAGE_UI_TEXT("Maximum"), PartField::Kind::Float, offsetof(Range, Max), -1000.0f, 1000.0f},
        {"value", SAGE_UI_TEXT("Value"), PartField::Kind::Float, offsetof(Range, Value), -1000.0f, 1000.0f},
        {"step", SAGE_UI_TEXT("Step"), PartField::Kind::Float, offsetof(Range, Step), 0.0f, 100.0f,
         "0 is smooth; above zero snaps to the step (volume in 5% notches)."},
        {"toggle", SAGE_UI_TEXT("Checkbox"), PartField::Kind::Bool, offsetof(Range, Toggle),
         0.0f, 1.0f, "The same 0..1 range with step 1, drawn as a box."},
        {"trackColor", SAGE_UI_TEXT("Track colour"), PartField::Kind::Color, offsetof(Range, TrackColor)},
        {"accentColor", SAGE_UI_TEXT("Accent colour"), PartField::Kind::Color, offsetof(Range, AccentColor)},
        {"borderColor", SAGE_UI_TEXT("Border colour"), PartField::Kind::Color, offsetof(Range, BorderColor)},
        {"borderThickness", SAGE_UI_TEXT("Border thickness"), PartField::Kind::Float,
         offsetof(Range, BorderThickness), 0.0f, 8.0f},
        {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Range, Rounding), 0.0f, 64.0f},
    };
    return f;
}

float RangeFraction(const Range& range) {
    const float span = range.Max - range.Min;
    if (std::fabs(span) < 1e-6f) return 0.0f;
    return std::clamp((range.Value - range.Min) / span, 0.0f, 1.0f);
}

// Акцентный цвет: у шкалы, если она есть, — так у элемента остаётся ОДНО
// место, где задан его цвет заполнения.

UIRect ToggleBox(const UIRect& r) {
    const float side = std::min(r.w, r.h);
    return {r.x, r.y + (r.h - side) * 0.5f, side, side};
}

void DrawRangePart(const PartDrawContext& c) {
    const Range& range = *static_cast<const Range*>(c.Data);
    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const glm::vec3 accentRgb = StateTint(c, glm::vec3(range.AccentColor));
    const glm::vec3 trackRgb = StateTint(c, glm::vec3(range.TrackColor));
    const float border = range.BorderThickness * c.Scale;
    const bool hasBorder = border > 0.0f && range.BorderColor.a > 0.0f;
    const glm::vec3 borderRgb{range.BorderColor.r, range.BorderColor.g, range.BorderColor.b};

    if (range.Toggle) {
        // Галка: квадратик у левого края СВОЕГО элемента. Подпись к нему —
        // отдельный объект рядом, а не сдвинутый текст внутри: так галку можно
        // поставить и справа от подписи, и под ней.
        const UIRect box = ToggleBox(r);
        const float rounding = std::min(range.Rounding * c.Scale, box.h * 0.5f);
        const float trackA = AlphaOf(c, range.TrackColor.a);
        if (trackA > 0.0f) ui.RoundedRect(box.x, box.y, box.w, box.h, trackRgb, trackA, rounding);
        if (hasBorder) {
            ui.RoundedRectOutline(box.x, box.y, box.w, box.h, rounding, border, borderRgb,
                                  AlphaOf(c, range.BorderColor.a));
        }
        // Включена — если значение ближе к верхнему концу диапазона.
        if (range.Value >= (range.Min + range.Max) * 0.5f) {
            const float pad = box.h * 0.18f;
            DrawIconInto(ui, "check", box.x + pad, box.y + pad, box.h - pad * 2.0f, accentRgb,
                         AlphaOf(c, range.AccentColor.a));
        }
        return;
    }

    // Ползунок: дорожка тонкая и по центру, ручка во всю высоту. Попасть в
    // тонкую полоску мышью трудно, а нажатие ловит весь элемент.
    const float track = std::max(r.h * 0.28f, 4.0f);
    const UIRect bar{r.x, r.y + (r.h - track) * 0.5f, r.w, track};
    const float trackA = AlphaOf(c, range.TrackColor.a);
    if (trackA > 0.0f) ui.RoundedRect(bar.x, bar.y, bar.w, bar.h, trackRgb, trackA, track * 0.5f);

    const float t = RangeFraction(range);
    const float fillA = AlphaOf(c, range.AccentColor.a);
    if (t > 0.0f && fillA > 0.0f) {
        ui.RoundedRect(bar.x, bar.y, bar.w * t, bar.h, accentRgb, fillA, track * 0.5f);
    }
    const float knob = r.h * 0.5f;
    const float kx = r.x + (r.w - knob * 2.0f) * t + knob;
    ui.Circle(kx, r.y + r.h * 0.5f, knob, accentRgb, AlphaOf(c, 1.0f));
    if (hasBorder) {
        ui.Ring(kx, r.y + r.h * 0.5f, knob, border, borderRgb, AlphaOf(c, range.BorderColor.a));
    }
}

// --- Поле ввода -------------------------------------------------------------

const std::vector<PartField>& TextInputFields() {
    static const std::vector<PartField> f = {
        {"placeholder", SAGE_UI_TEXT("Placeholder"), PartField::Kind::String, offsetof(TextInput, Placeholder)},
        {"maxLength", SAGE_UI_TEXT("Max length"), PartField::Kind::Int, offsetof(TextInput, MaxLength), 0.0f,
         4096.0f, "0 means no limit."},
        {"password", SAGE_UI_TEXT("Password"), PartField::Kind::Bool, offsetof(TextInput, Password),
         0.0f, 1.0f, "Hide the content behind dots."},
        {"readOnly", SAGE_UI_TEXT("Read only"), PartField::Kind::Bool, offsetof(TextInput, ReadOnly)},
    };
    return f;
}

std::string MaskText(const std::string& text) {
    std::string out;
    const int n = Utf8Length(text);
    out.reserve((size_t)n * 3);
    for (int i = 0; i < n; ++i) out += "•";
    return out;
}

void DrawTextInputPart(const PartDrawContext& c) {
    const TextInput& input = *static_cast<const TextInput*>(c.Data);
    const Label* label = c.Sibling<Label>();
    if (!label) return; // без текста показывать нечего: поле хранит его в нём

    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const float textScale = label->Scale * c.Scale;
    const float left = r.x + label->PadX * c.Scale;
    const glm::vec3 rgb{label->Color.r, label->Color.g, label->Color.b};
    const float y = r.y + (r.h - ui.TextHeight(textScale)) * 0.5f;

    const bool empty = label->Text.empty();
    const std::string shown =
        empty ? input.Placeholder : (input.Password ? MaskText(label->Text) : label->Text);
    if (!shown.empty()) {
        // Подсказка бледнее содержимого — иначе пустое поле выглядит
        // заполненным, и человек стирает то, чего не вводил.
        ui.Text(left, y, textScale, rgb, shown, AlphaOf(c, label->Color.a) * (empty ? 0.45f : 1.0f));
    }

    // Курсор мигает только в фокусе и только когда поле включено.
    const Interactable* act = c.Sibling<Interactable>();
    if (!c.Focused || !c.Enabled || !act) return;
    const int caret = std::clamp(act->Runtime.Caret, 0, (int)label->Text.size());
    const std::string head = label->Text.substr(0, (size_t)caret);
    const float cx = left + ui.MeasureText(input.Password ? MaskText(head) : head, textScale);
    // Полсекунды виден, полсекунды нет; после каждой правки счётчик
    // сбрасывается, чтобы курсор не пропал ровно тогда, когда на него смотрят.
    if (std::fmod(act->Runtime.CaretBlink, 1.0f) < 0.5f) {
        const float ch = ui.TextHeight(textScale) * 1.15f;
        ui.Rect(cx, r.y + (r.h - ch) * 0.5f, std::max(textScale, 1.0f), ch, rgb,
                AlphaOf(c, label->Color.a));
    }
}

// --- Невидимые части: поведение и раскладка ---------------------------------

const std::vector<PartField>& InteractableFields() {
    static const std::vector<PartField> f = {
        {"action", SAGE_UI_TEXT("Action"), PartField::Kind::String, offsetof(Interactable, Action), 0.0f, 0.0f,
         "A name for the game: a script asks whether Continue was pressed, not\n"
         "whether entity 37 was."},
        {"enabled", SAGE_UI_TEXT("Enabled"), PartField::Kind::Bool, offsetof(Interactable, Enabled)},
        {"cursor", SAGE_UI_TEXT("Cursor"), PartField::Kind::String, offsetof(Interactable, Cursor)},
        {"hoverBrightness", SAGE_UI_TEXT("Brighter on hover"), PartField::Kind::Float,
         offsetof(Interactable, HoverBrightness), 0.5f, 2.0f},
        {"pressedBrightness", SAGE_UI_TEXT("Darker when pressed"), PartField::Kind::Float,
         offsetof(Interactable, PressedBrightness), 0.5f, 2.0f},
        {"disabledAlpha", SAGE_UI_TEXT("Disabled alpha"), PartField::Kind::Float,
         offsetof(Interactable, DisabledAlpha), 0.0f, 1.0f},
        {"events", SAGE_UI_TEXT("Events"), PartField::Kind::Bindings, offsetof(Interactable, Events),
         0.0f, 0.0f,
         "What the button does itself: send an event, call a method on another\n"
         "object. Without this a button needs a script polling it every frame."},
    };
    return f;
}

const char* const kMaskShape[] = {SAGE_UI_TEXT("Rectangle"), SAGE_UI_TEXT(SAGE_UI_TEXT("Rounded"))};

const std::vector<PartField>& MaskFields() {
    static const std::vector<PartField> f = {
        {"form", SAGE_UI_TEXT("Shape"), PartField::Kind::Enum, offsetof(Mask, Form), 0.0f, 0.0f, nullptr,
         kMaskShape, 2},
        {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Mask, Rounding), -1.0f, 64.0f,
         "Below zero takes the rounding from the fill."},
        {"padding", SAGE_UI_TEXT("Padding (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Mask, Padding), 0.0f, 256.0f},
        {"showOutside", SAGE_UI_TEXT("Do not clip"), PartField::Kind::Bool, offsetof(Mask, ShowOutside)},
    };
    return f;
}

const char* const kFlow[] = {SAGE_UI_TEXT("Row"), SAGE_UI_TEXT(SAGE_UI_TEXT("Column")), SAGE_UI_TEXT("Grid")};
const char* const kJustify[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT(SAGE_UI_TEXT("Center")), SAGE_UI_TEXT("End"), SAGE_UI_TEXT("Even")};

const std::vector<PartField>& LayoutFields() {
    static const std::vector<PartField> f = {
        {"direction", SAGE_UI_TEXT("Direction"), PartField::Kind::Enum, offsetof(Layout, Direction), 0.0f, 0.0f,
         nullptr, kFlow, 3},
        {"justify", SAGE_UI_TEXT("Justify"), PartField::Kind::Enum, offsetof(Layout, Justify), 0.0f, 0.0f,
         nullptr, kJustify, 4},
        {"spacing", SAGE_UI_TEXT("Spacing"), PartField::Kind::Float, offsetof(Layout, Spacing), 0.0f, 128.0f},
        {"padding", SAGE_UI_TEXT("Padding (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Layout, Padding), 0.0f,
         256.0f},
        {"columns", SAGE_UI_TEXT("Columns"), PartField::Kind::Int, offsetof(Layout, Columns), 1.0f, 32.0f},
        {"stretchCross", SAGE_UI_TEXT("Stretch across"), PartField::Kind::Bool,
         offsetof(Layout, StretchCross)},
        {"fitContent", SAGE_UI_TEXT("Size from content"), PartField::Kind::Bool,
         offsetof(Layout, FitContent)},
    };
    return f;
}

const char* const kCanvasScale[] = {SAGE_UI_TEXT("Pixels"), SAGE_UI_TEXT(SAGE_UI_TEXT("Scale to reference"))};

const std::vector<PartField>& CanvasFields() {
    static const std::vector<PartField> f = {
        {"mode", SAGE_UI_TEXT("Scale mode"), PartField::Kind::Enum, offsetof(Canvas, Mode), 0.0f, 0.0f, nullptr,
         kCanvasScale, 2},
        {"reference", SAGE_UI_TEXT("Reference resolution"), PartField::Kind::Vec2, offsetof(Canvas, Reference),
         64.0f, 8192.0f},
        {"matchWidthOrHeight", SAGE_UI_TEXT("Follow"), PartField::Kind::Float,
         offsetof(Canvas, MatchWidthOrHeight), 0.0f, 1.0f,
         "0 follows the width, 1 the height, 0.5 the average."},
        {"sortOrder", SAGE_UI_TEXT("Root order"), PartField::Kind::Int, offsetof(Canvas, SortOrder), -100.0f,
         100.0f, "The HUD under the pause menu, the menu under a dialog."},
    };
    return f;
}

const std::vector<PartField>& GroupFields() {
    static const std::vector<PartField> f = {
        {"alpha", SAGE_UI_TEXT("Alpha"), PartField::Kind::Float, offsetof(Group, Alpha), 0.0f, 1.0f},
        {"interactable", SAGE_UI_TEXT("Catches the mouse"), PartField::Kind::Bool, offsetof(Group, Interactable)},
        {"blockRaycasts", SAGE_UI_TEXT("Block raycasts"), PartField::Kind::Bool,
         offsetof(Group, BlockRaycasts)},
    };
    return f;
}

// --- Регистрация ------------------------------------------------------------

void RegisterBuiltins() {
    // Порядок: тень 5, подложка 10, картинка 20, шкала 30, диапазон 40,
    // значок 50, текст 60, поле ввода 70. Шаг в десять — чтобы чужая часть
    // могла встать между своими, не переписывая чужие числа.
    PartType fill = MakePart<Fill>("fill", SAGE_UI_TEXT("Fill"), 10, &FillFields(), DrawFill, DrawFillBorder);
    fill.Icon = "rect";
    fill.Hint = SAGE_UI_TEXT("Background, rounding, border, gradient, shadow");
    RegisterPart(fill);

    // Тень — отдельной записью того же компонента: она обязана лечь ПОД всё, а
    // подложка рисуется уже над ней. Так порядок задан числом, а не порядком
    // строк внутри одной функции.
    PartType shadow = MakePart<Fill>("fill.shadow", SAGE_UI_TEXT("Fill shadow"), 5, nullptr, DrawFillShadow);
    RegisterPart(shadow);

    PartType image = MakePart<Image>("image", SAGE_UI_TEXT("Image"), 20, &ImageFields(), DrawImagePart);
    image.Icon = "texture";
    image.Hint = SAGE_UI_TEXT("A file, a piece of a sprite sheet, 9-slice, pixel art");
    RegisterPart(image);

    PartType bar = MakePart<Bar>("bar", SAGE_UI_TEXT("Bar"), 30, &BarFields(), DrawBarPart);
    bar.Icon = "wire";
    bar.Hint = SAGE_UI_TEXT("Health, progress, loading");
    RegisterPart(bar);

    PartType range = MakePart<Range>("range", SAGE_UI_TEXT("Range"), 40, &RangeFields(), DrawRangePart);
    range.Icon = "align";
    range.Hint = SAGE_UI_TEXT("A slider or a checkbox");
    RegisterPart(range);

    PartType icon = MakePart<Icon>("icon", SAGE_UI_TEXT("Icon"), 50, &IconFields(), DrawIconPart);
    icon.Icon = "sun";
    icon.Hint = SAGE_UI_TEXT("A vector icon of the engine");
    RegisterPart(icon);

    PartType label = MakePart<Label>("label", SAGE_UI_TEXT("Text"), 60, &LabelFields(), DrawLabelPart);
    label.Icon = "file";
    label.Hint = SAGE_UI_TEXT("A caption: size, colour, alignment, wrapping");
    RegisterPart(label);

    PartType input = MakePart<TextInput>("textInput", SAGE_UI_TEXT("Text Input"), 70, &TextInputFields(),
                                         DrawTextInputPart);
    input.Icon = "code";
    input.Hint = SAGE_UI_TEXT("Edited from the keyboard; the value lives in the Text part");
    RegisterPart(input);

    // Невидимые части: рисовать нечего, но в реестре они нужны — по нему идут и
    // запись в файл, и список «добавить часть» в редакторе.
    PartType act = MakePart<Interactable>("interactable", SAGE_UI_TEXT("Interactable"), 100,
                                          &InteractableFields());
    act.Icon = "cube";
    act.Hint = SAGE_UI_TEXT("Hover, press, click, and an action name for the game");
    RegisterPart(act);

    PartType mask = MakePart<Mask>("mask", SAGE_UI_TEXT("Mask"), 100, &MaskFields());
    mask.Icon = "rect";
    mask.Hint = SAGE_UI_TEXT("The subtree is clipped by this element's rectangle");
    RegisterPart(mask);

    PartType layout = MakePart<Layout>("layout", SAGE_UI_TEXT("Layout"), 100, &LayoutFields());
    layout.Icon = "layout";
    layout.Hint = SAGE_UI_TEXT("The parent lays its children out: row, column, grid");
    RegisterPart(layout);

    PartType canvas = MakePart<Canvas>("canvas", SAGE_UI_TEXT("Canvas"), 100, &CanvasFields());
    canvas.Icon = "grid";
    canvas.Hint = SAGE_UI_TEXT("A UI root: reference resolution and order between roots");
    RegisterPart(canvas);

    PartType group = MakePart<Group>("group", SAGE_UI_TEXT("Group"), 100, &GroupFields());
    group.Icon = "copy";
    group.Hint = SAGE_UI_TEXT("Alpha and input for the whole subtree");
    RegisterPart(group);
}

std::vector<PartType>& Registry() {
    static std::vector<PartType> parts;
    return parts;
}

bool& Registered() {
    static bool done = false;
    return done;
}

} // namespace

// --- Общие помощники по тексту (объявлены в UIPart.h) -----------------------

namespace {
// Граница следующего символа UTF-8: резать длинное слово можно только по ней,
// иначе символ разваливается пополам и строка перестаёт быть валидной.
int NextCharBoundary(const std::string& s, int i) {
    const int n = (int)s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) ++i;
    return i;
}
} // namespace

int Utf8Length(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Разбивает текст на строки по ширине. Перенос по СЛОВАМ; слово, которое не
// влезает целиком, режется по символам — иначе одно длинное слово молча уехало
// бы за край, то есть ровно то, от чего перенос и спасает. Явные \n уважаются.
std::vector<std::string> WrapLines(const std::string& text, float maxWidth, float scale,
                                   UIRenderer& ui) {
    std::vector<std::string> lines;
    if (maxWidth <= 0.0f) { lines.push_back(text); return lines; }

    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string paragraph = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                                : nl - start);
        std::string line;
        size_t i = 0;
        while (i < paragraph.size()) {
            // Следующее слово вместе с ведущими пробелами.
            size_t wordEnd = paragraph.find(' ', i);
            if (wordEnd == std::string::npos) wordEnd = paragraph.size();
            const std::string word = paragraph.substr(i, wordEnd - i);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (ui.MeasureText(candidate, scale) <= maxWidth || line.empty()) {
                if (ui.MeasureText(candidate, scale) > maxWidth && line.empty()) {
                    // Слово шире строки — режем по символам (по границам UTF-8).
                    std::string chunk;
                    size_t c = 0;
                    while (c < word.size()) {
                        const size_t next = (size_t)NextCharBoundary(word, (int)c);
                        const std::string grown = chunk + word.substr(c, next - c);
                        if (!chunk.empty() && ui.MeasureText(grown, scale) > maxWidth) {
                            lines.push_back(chunk);
                            chunk.clear();
                            continue;
                        }
                        chunk = grown;
                        c = next;
                    }
                    line = chunk;
                } else {
                    line = candidate;
                }
            } else {
                lines.push_back(line);
                line = word;
            }
            i = wordEnd + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

void RegisterPart(const PartType& type) {
    std::vector<PartType>& parts = Registry();
    for (PartType& p : parts) {
        // Тот же ключ — замена: так игра подменяет встроенную часть своей, а не
        // получает две записи, из которых рисуются обе.
        if (p.Id && type.Id && std::string(p.Id) == type.Id) {
            p = type;
            std::stable_sort(parts.begin(), parts.end(),
                             [](const PartType& a, const PartType& b) { return a.Order < b.Order; });
            return;
        }
    }
    parts.push_back(type);
    std::stable_sort(parts.begin(), parts.end(),
                     [](const PartType& a, const PartType& b) { return a.Order < b.Order; });
}

const std::vector<PartType>& Parts() {
    // Регистрация встроенных — ЛЕНИВАЯ, а не в статических конструкторах:
    // порядок инициализации статиков между единицами трансляции не определён, и
    // реестр, собранный до своего вектора, — это падение на старте.
    if (!Registered()) {
        Registered() = true;
        RegisterBuiltins();
    }
    return Registry();
}

const PartType* FindPart(std::string_view id) {
    for (const PartType& p : Parts())
        if (p.Id && id == p.Id) return &p;
    return nullptr;
}

} // namespace sage::ui
