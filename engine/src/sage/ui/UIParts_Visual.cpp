#include "sage/ui/UIPartsDetail.h"

// Части интерфейса, которые рисуют (см. UIPartsDetail.h).
namespace sage::ui::parts {

// --- Подложка ---------------------------------------------------------------

// Подложка — это вид целиком, раскрытый прямо в часть: ключи в файле те же,
// что были у неё всегда (color, rounding, borderThickness...), и старые сцены
// читаются без перевода.
const std::vector<PartField>& FillFields() {
    static const std::vector<PartField> f = [] {
        Fill probe;
        const size_t base = (size_t)(reinterpret_cast<const char*>(static_cast<const Look*>(&probe)) -
                                     reinterpret_cast<const char*>(&probe));
        return ShiftLookFields(nullptr, base, nullptr);
    }();
    return f;
}

// Вид подложки сейчас: оформление движка (из цвета), свой вид состояния или
// сама подложка. storage — куда положить посчитанный вид движка.
const Look& FillLook(const PartDrawContext& c, const Fill& fill, Look& storage, bool& tinted) {
    if (c.EngineSkin) {
        tinted = true;
        storage = EngineLook(fill, FillRole(c));
        // Поле ввода в фокусе — рамка ярче: куда печатают, видно сразу.
        if (c.Focused && c.Sibling<TextInput>()) {
            storage.BorderThickness = 2.0f;
            storage.BorderColor = glm::vec4(glm::mix(glm::vec3(fill.Color), glm::vec3(1.0f), 0.6f), 1.0f);
        }
        return storage;
    }
    return StateLook(c, fill, tinted);
}

void DrawFill(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    bool tinted = true;
    Look engine;
    const Look& look = FillLook(c, fill, engine, tinted);
    const glm::vec3 rgb = tinted ? StateTint(c, glm::vec3(look.Color)) : glm::vec3(look.Color);
    // Свой вид недоступного состояния уже нарисован бледным — второй раз его
    // не бледнят.
    const float alpha = tinted ? AlphaOf(c, look.Color.a) : look.Color.a * c.Alpha;
    DrawLookBody(c, look, c.Rect, rgb, alpha);
}

// Тень и рамка — разными слоями: тень под всем, рамка поверх всего. Иначе
// картинка ложилась бы на рамку, а тень — на соседний элемент.
void DrawFillShadow(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    bool tinted = true;
    Look engine;
    const Look& look = FillLook(c, fill, engine, tinted);
    DrawLookShadow(c, look, c.Rect, c.Alpha);
}

void DrawFillBorder(const PartDrawContext& c) {
    const Fill& fill = *static_cast<const Fill*>(c.Data);
    bool tinted = true;
    Look engine;
    const Look& look = FillLook(c, fill, engine, tinted);
    DrawLookBorder(c, look, c.Rect, tinted ? AlphaOf(c, 1.0f) : c.Alpha);
}

// --- Картинка ---------------------------------------------------------------

const std::vector<PartField>& ImageFields() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"path", SAGE_UI_TEXT("File"), PartField::Kind::String, offsetof(Image, Path), 0.0f, 0.0f, nullptr,
             nullptr, 0, PartField::Widget::Texture},
            {"mode", SAGE_UI_TEXT("How it fits"), PartField::Kind::Enum, offsetof(Image, Fit), 0.0f, 4.0f,
             "Stretch, cut into nine pieces (corners keep their size), repeat at\n"
             "its own size, or keep the aspect ratio: show the whole picture with\n"
             "margins, or fill the element and cut off what does not fit.",
             kImageModeNames, 5},
            {"tint", SAGE_UI_TEXT("Tint"), PartField::Kind::Color, offsetof(Image, Tint)},
            {"sprite", SAGE_UI_TEXT("Sprite (x,y,w,h)"), PartField::Kind::Vec4, offsetof(Image, Sprite), 0.0f,
             4096.0f, "A piece of the sheet in source pixels; width 0 means the whole file."},
            {"sliceBorder", SAGE_UI_TEXT("9-slice (l,t,r,b)"), PartField::Kind::Vec4,
             offsetof(Image, SliceBorder), 0.0f, 512.0f,
             "Fixed corners in source pixels. The 9-slice editor sets them by\n"
             "dragging lines over the picture itself.",
             nullptr, 0, PartField::Widget::NineSliceBorder, "mode", (int)Image::Mode::NineSlice},
            {"sliceCenterFill", SAGE_UI_TEXT("9-slice centre"), PartField::Kind::Enum,
             offsetof(Image, SliceCenterFill), 0.0f, 1.0f, "Stretch or repeat the middle piece.",
             kSliceFillNames, 2, PartField::Widget::Auto, "mode", (int)Image::Mode::NineSlice},
            {"sliceEdgeFill", SAGE_UI_TEXT("9-slice edges"), PartField::Kind::Enum,
             offsetof(Image, SliceEdgeFill), 0.0f, 1.0f,
             "Repeat keeps an ornament crisp; stretch smears it.", kSliceFillNames, 2,
             PartField::Widget::Auto, "mode", (int)Image::Mode::NineSlice},
            {"sliceDrawCenter", SAGE_UI_TEXT("9-slice draws the middle"), PartField::Kind::Bool,
             offsetof(Image, SliceDrawCenter), 0.0f, 1.0f,
             "An outline frame has no middle — what is under it shows through.", nullptr, 0,
             PartField::Widget::Auto, "mode", (int)Image::Mode::NineSlice},
            {"repeat", SAGE_UI_TEXT("Repeat count"), PartField::Kind::Vec2, offsetof(Image, Repeat), 0.0f,
             256.0f, kRepeatTip, nullptr, 0, PartField::Widget::Auto, "mode", (int)Image::Mode::Tile},
            {"pixelScale", SAGE_UI_TEXT("Source pixel size"), PartField::Kind::Float,
             offsetof(Image, PixelScale), 0.0f, 16.0f,
             "How many interface pixels one pixel of the file takes.\n"
             "0 — one to one for 9-slice and tile (corners keep their size),\n"
             "fit to the element for the other modes."},
            {"snapPixels", SAGE_UI_TEXT("Whole-number scale"), PartField::Kind::Bool,
             offsetof(Image, SnapPixels), 0.0f, 1.0f,
             "Rounds the scale down to a whole number, so a frame drawn pixel by\n"
             "pixel does not go wavy. The element is then not filled completely."},
            {"filter", SAGE_UI_TEXT("Filtering"), PartField::Kind::Enum, offsetof(Image, Filtering), 0.0f,
             1.0f, "Smooth for photos and anything scaled down; Nearest for crisp pixels.",
             kImageFilterNames, 2},
            // Спрайты состояний — из тех времён, когда картинку делали
            // кнопкой. Теперь у кнопки свои виды состояний; поля читаются,
            // чтобы старая сцена не потеряла их при сохранении, но не
            // показываются: у картинки состояний нет.
            {"spriteHover", SAGE_UI_TEXT("Sprite on hover"), PartField::Kind::Vec4,
             offsetof(Image, SpriteHover), 0.0f, 4096.0f},
            {"spritePressed", SAGE_UI_TEXT("Sprite when pressed"), PartField::Kind::Vec4,
             offsetof(Image, SpritePressed), 0.0f, 4096.0f},
        };
        v[v.size() - 1].Hidden = true;
        v[v.size() - 2].Hidden = true;
        // Фильтрация — НА ВИДУ: чёткие пиксели или сглаживание решают, глядя
        // на картинку, и прятать этот выбор в «Ещё настройки» значило, что
        // его не находили.
        MarkAdvanced(v, {"sprite", "sliceCenterFill", "sliceEdgeFill", "sliceDrawCenter", "pixelScale",
                         "snapPixels"});
        return v;
    }();
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

    PictureParams p;
    p.Tex = img.Tex.get();
    p.Src = StateSprite(c, img);
    p.Fit = (int)img.Fit;
    p.Slice = img.Slice();
    p.PixelScale = img.PixelScale;
    p.SnapPixels = img.SnapPixels;
    p.Repeat = img.Repeat;
    DrawPicture(ui, p, r, c.Scale, rgb, alpha);
}

// --- Шкала ------------------------------------------------------------------

const char* const kBarGrow[] = {SAGE_UI_TEXT("Right"), SAGE_UI_TEXT("Left"), SAGE_UI_TEXT("Up"),
                                SAGE_UI_TEXT("Down")};
const char* const kBarFillMode[] = {SAGE_UI_TEXT("Squeeze"), SAGE_UI_TEXT("Reveal")};

const std::vector<PartField>& BarFields() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"value", SAGE_UI_TEXT("Value"), PartField::Kind::Float, offsetof(Bar, Value), 0.0f, 1.0f},
            {"grow", SAGE_UI_TEXT("Grows"), PartField::Kind::Enum, offsetof(Bar, Grow), 0.0f, 0.0f,
             "A vertical gauge (mana at the side, a volume column) needs Up or Down.", kBarGrow, 4},
            {"fillMode", SAGE_UI_TEXT("Picture shows value by"), PartField::Kind::Enum, offsetof(Bar, Mode),
             0.0f, 1.0f,
             "Squeeze — the fill picture shrinks to the value.\n"
             "Reveal — it stays in place and is uncovered: marks on a health bar\n"
             "do not get squashed when health is low.",
             kBarFillMode, 2, PartField::Widget::Auto, "filled.texture", 1},
            {"padding", SAGE_UI_TEXT("Fill inset (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Bar, Padding),
             0.0f, 64.0f, "How far the fill stays from the edge of the bar."},
            {"smoothing", SAGE_UI_TEXT("Smoothing"), PartField::Kind::Float, offsetof(Bar, Smoothing), 0.0f,
             8.0f, "Units per second; 0 is instant."},
            {"filled", SAGE_UI_TEXT("Fill"), PartField::Kind::Look, offsetof(Bar, Filled)},
        };
        v[0].Content = true;
        MarkAdvanced(v, {"fillMode", "padding", "smoothing"});
        MarkCustomOnly(v, {"fillMode", "padding"});
        return v;
    }();
    return f;
}

float BarShown(const Bar& bar) {
    return (bar.Smoothing > 0.0f && bar.Displayed >= 0.0f) ? bar.Displayed : bar.Value;
}

void DrawBarPart(const PartDrawContext& c) {
    const Bar& bar = *static_cast<const Bar*>(c.Data);
    const UIRect& r = c.Rect;
    const float t = std::clamp(BarShown(bar), 0.0f, 1.0f);
    if (t <= 0.0f) return;

    const glm::vec4 pad = (c.EngineSkin ? glm::vec4(3.0f) : bar.Padding) * c.Scale;
    const Look engineFill = EngineLook(bar.Filled, EngineRole::BarFill);
    const Look& filled = c.EngineSkin ? engineFill : bar.Filled;
    const UIRect inner{r.x + pad.x, r.y + pad.y, std::max(0.0f, r.w - pad.x - pad.z),
                       std::max(0.0f, r.h - pad.y - pad.w)};
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
    // ОТКРЫТЬ, А НЕ СЖАТЬ: картинка заполнения рисуется во всю полосу и
    // обрезается долей. Для плашки цветом разницы нет — сжатие честнее
    // скругляет край.
    if (!c.EngineSkin && bar.Mode == Bar::FillMode::Reveal && filled.HasTexture()) {
        c.Ui->PushClipRect(fillRect.x, fillRect.y, fillRect.w, fillRect.h);
        DrawLook(c, filled, inner, /*tint=*/false);
        c.Ui->PopClipRect();
        return;
    }
    DrawLook(c, filled, fillRect, /*tint=*/false);
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

const char* const kAlign[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("End")};

const char* const kLabelFaces[] = {SAGE_UI_TEXT("Regular"), SAGE_UI_TEXT("Bold"),
                                   SAGE_UI_TEXT("Italic"), SAGE_UI_TEXT("Bold italic")};

const std::vector<PartField>& LabelFields() {
    static const std::vector<PartField> f = [] { std::vector<PartField> v = {
        {"text", SAGE_UI_TEXT("Text"), PartField::Kind::String, offsetof(Label, Text), 0.0f, 0.0f, nullptr,
         nullptr, 0, PartField::Widget::Multiline},
        // ПРЕДЕЛ КЕГЛЯ — 128, а не 12. Прежние 12 упирались примерно в сотню
        // экранных пикселей: заголовок меню, надпись на весь экран, счёт в
        // аркаде — всё, что крупнее, было просто НЕДОСТУПНО (поле ограничивало
        // и ввод числа, а не только ползунок). Нижняя граница тоже опущена:
        // сноска мельче половинного кегля — обычное дело.
        {"scale", SAGE_UI_TEXT("Font size"), PartField::Kind::Float, offsetof(Label, Scale), 0.1f, 128.0f},
        {"color", SAGE_UI_TEXT("Colour"), PartField::Kind::Color, offsetof(Label, Color)},
        {"face", SAGE_UI_TEXT("Face"), PartField::Kind::Enum, offsetof(Label, Face), 0.0f, 3.0f,
         "Bold and italic are drawn by the engine itself, so they work with any\n"
         "font file — including those that ship a single face.",
         kLabelFaces, 4},
        {"font", SAGE_UI_TEXT("Font file"), PartField::Kind::String, offsetof(Label, Font), 0.0f, 0.0f,
         "Empty — the project interface font. A .ttf/.otf next to the game.",
         nullptr, 0, PartField::Widget::Font},
        {"fontPixelHeight", SAGE_UI_TEXT("Font baking size"), PartField::Kind::Float,
         offsetof(Label, FontPixelHeight), 8.0f, 256.0f,
         "How tall glyphs are baked into the atlas. Bigger is sharper at large\n"
         "sizes and costs more texture memory."},
        {"fontSnapPixels", SAGE_UI_TEXT("Whole-number font scale"), PartField::Kind::Bool,
         offsetof(Label, FontSnapPixels), 0.0f, 1.0f,
         "Rounds the glyph scale down to a whole number — what a font drawn\n"
         "pixel by pixel needs. The price is that the size then changes in\n"
         "steps, so it is a choice and not a side effect of filtering."},
        {"fontFilter", SAGE_UI_TEXT("Font filtering"), PartField::Kind::Enum,
         offsetof(Label, FontFiltering), 0.0f, 1.0f,
         "Nearest neighbour, no mipmaps and a whole-number scale: a fractional\n"
         "one stretches some strokes of a letter over two screen pixels and the\n"
         "neighbouring ones over one, and the text goes wavy.",
         kImageFilterNames, 2},
        {"horizontal", SAGE_UI_TEXT("Horizontal"), PartField::Kind::Enum, offsetof(Label, Horizontal), 0.0f,
         0.0f, nullptr, kAlign, 3},
        {"vertical", SAGE_UI_TEXT("Vertical"), PartField::Kind::Enum, offsetof(Label, Vertical), 0.0f, 0.0f,
         nullptr, kAlign, 3},
        {"wrap", SAGE_UI_TEXT("Wrap"), PartField::Kind::Bool, offsetof(Label, Wrap)},
        {"autoWidth", SAGE_UI_TEXT("Width from text"), PartField::Kind::Bool, offsetof(Label, AutoWidth)},
        // БОКОВОЙ ОТСТУП — до 512. Прежние 64 хватало кнопке и не хватало
        // ничему другому: колонка текста в письме, поле с широкой рамкой,
        // отступ под значок слева — всё это десятки и сотни пикселей, и поле
        // не давало их даже ввести числом.
        {"padX", SAGE_UI_TEXT("Side padding"), PartField::Kind::Float, offsetof(Label, PadX), 0.0f, 512.0f},
        {"shadowOffset", SAGE_UI_TEXT("Shadow offset"), PartField::Kind::Vec2,
         offsetof(Label, ShadowOffset), -32.0f, 32.0f,
         "A copy of the text drawn underneath, shifted by this much.\n"
         "0, 0 — no shadow."},
        {"shadowColor", SAGE_UI_TEXT("Shadow colour"), PartField::Kind::Color, offsetof(Label, ShadowColor)},
        {"outline", SAGE_UI_TEXT("Outline width"), PartField::Kind::Float, offsetof(Label, OutlineWidth),
         0.0f, 16.0f, "0 — no outline."},
        {"outlineColor", SAGE_UI_TEXT("Outline colour"), PartField::Kind::Color,
         offsetof(Label, OutlineColor)},
        {"stateColors", SAGE_UI_TEXT("Colour follows the button"), PartField::Kind::Bool,
         offsetof(Label, StateColors), 0.0f, 1.0f,
         "The caption changes colour with the nearest button above it: lighter\n"
         "on hover, darker when pressed, faded when disabled."},
        {"hoverColor", SAGE_UI_TEXT("Colour on hover"), PartField::Kind::Color, offsetof(Label, HoverColor),
         0.0f, 1.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "stateColors", 1},
        {"pressedColor", SAGE_UI_TEXT("Colour when pressed"), PartField::Kind::Color,
         offsetof(Label, PressedColor), 0.0f, 1.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
         "stateColors", 1},
        {"disabledColor", SAGE_UI_TEXT("Colour when disabled"), PartField::Kind::Color,
         offsetof(Label, DisabledColor), 0.0f, 1.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
         "stateColors", 1},
    };
    v[0].Content = true;
    MarkAdvanced(v, {"font", "fontPixelHeight", "fontSnapPixels", "fontFilter", "autoWidth", "padX",
                     "shadowOffset", "shadowColor", "outline", "outlineColor", "stateColors",
                     "hoverColor", "pressedColor", "disabledColor"});
    MarkCustomOnly(v, {"fontPixelHeight", "fontSnapPixels", "fontFilter", "shadowOffset", "shadowColor",
                       "outline", "outlineColor", "stateColors", "hoverColor", "pressedColor",
                       "disabledColor"});
    return v; }();
    return f;
}

// Что и чем писать: свой шрифт надписи (если задан и открылся) плюс
// начертание. Одной функцией, потому что текст надписи рисует не только
// DrawLabelPart: поле ввода показывает то же содержимое ТЕМ ЖЕ шрифтом, и
// разойтись здесь значило бы получить в поле ввода другой шрифт, чем в
// подписи рядом.
UITextStyle StyleOf(const Label& label, UIRenderer& ui) {
    UITextStyle style;
    if (!label.Font.empty())
        style.UseFont = ui.LoadFont(label.Font, label.FontPixelHeight, label.Sharp());
    style.SnapPixels = label.FontSnapPixels;
    style.Bold = label.Face == Label::Style::Bold || label.Face == Label::Style::BoldItalic;
    style.Italic = label.Face == Label::Style::Italic || label.Face == Label::Style::BoldItalic;
    return style;
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
    // Цвет по состоянию хозяина: надпись кнопки — её ребёнок, и светлеть
    // при наведении она должна вместе с кнопкой.
    glm::vec4 color = label.Color;
    if (label.StateColors && !c.EngineSkin) {
        if (!c.OwnerEnabled) color = label.DisabledColor;
        else if (c.OwnerPressed) color = label.PressedColor;
        else if (c.OwnerHovered) color = label.HoverColor;
    }
    const glm::vec3 rgb{color.r, color.g, color.b};
    const float alpha = AlphaOf(c, color.a);
    const UITextStyle style = StyleOf(label, ui);

    std::vector<std::string> lines;
    if (label.Wrap) lines = WrapLines(label.Text, avail, textScale, ui, &style);
    else if (label.Text.find('\n') != std::string::npos)
        lines = WrapLines(label.Text, 0.0f, textScale, ui, &style); // только по явным \n
    else lines.push_back(label.Text);

    // ВЫСОТА БЛОКА — ЦЕЛИКОМ ПО ШРИФТУ. Раньше последняя строка считалась
    // номиналом тулкита (8·scale), а остальные — высотой строки шрифта:
    // вёрстка мерила блок одним числом, а рисовала его другим, и надпись
    // съезжала вверх тем сильнее, чем крупнее кегль.
    const float lineH = ui.LineHeight(textScale, style);
    const float blockH = lineH * (float)lines.size();
    float y = AlignY(label.Vertical, r, blockH);

    // Перенесённый текст ОБРЕЗАЕТСЯ своим элементом: он переносился под эту
    // ширину, значит и по высоте обязан остаться внутри. Абзац, вылезший на
    // фон, выглядит хуже обрезанного — и не читается как ошибка вёрстки.
    const bool clip = label.Wrap && blockH > r.h;
    if (clip) ui.PushClipRect(r.x, r.y, r.w, r.h);
    // Сдвиги тени и обводки — ЦЕЛЫМИ пикселями у шрифта с целым масштабом:
    // дробный сдвиг раскладывает пиксельную тень на два экранных пикселя
    // полупрозрачно, и она выглядит грязной каймой, а не тенью.
    auto px = [&](float v) {
        const float scaled = v * c.Scale;
        return label.FontSnapPixels ? std::round(scaled) : scaled;
    };
    const glm::vec2 shadow{px(label.ShadowOffset.x), px(label.ShadowOffset.y)};
    // Тень и обводка — своё оформление; у движка текст чистый.
    const bool hasShadow =
        !c.EngineSkin && (shadow.x != 0.0f || shadow.y != 0.0f) && label.ShadowColor.a > 0.0f;
    const float outline = label.OutlineWidth > 0.0f ? std::max(px(label.OutlineWidth), 1.0f) : 0.0f;
    const bool hasOutline = !c.EngineSkin && outline > 0.0f && label.OutlineColor.a > 0.0f;
    for (const std::string& line : lines) {
        if (!line.empty()) {
            const float x = AlignX(label.Horizontal, left, avail, ui.MeasureText(line, textScale, style));
            // Порядок — снизу вверх: тень, обводка, сам текст.
            if (hasShadow) {
                ui.Text(x + shadow.x, y + shadow.y, textScale,
                        {label.ShadowColor.r, label.ShadowColor.g, label.ShadowColor.b}, line,
                        AlphaOf(c, label.ShadowColor.a * label.Color.a), style);
            }
            if (hasOutline) {
                // Восемь копий по кругу: четырёх (крест) не хватает — на
                // диагональных штрихах в углах остаются просветы.
                const glm::vec3 oc{label.OutlineColor.r, label.OutlineColor.g, label.OutlineColor.b};
                const float oa = AlphaOf(c, label.OutlineColor.a * label.Color.a);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dx != 0 || dy != 0)
                            ui.Text(x + dx * outline, y + dy * outline, textScale, oc, line, oa, style);
            }
            ui.Text(x, y, textScale, rgb, line, alpha, style);
        }
        y += lineH;
    }
    if (clip) ui.PopClipRect();
}


} // namespace sage::ui::parts
