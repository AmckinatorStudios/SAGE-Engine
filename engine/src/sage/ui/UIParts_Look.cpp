#include "sage/ui/UIPartsDetail.h"

// Оформление частей интерфейса (см. UIPartsDetail.h).
namespace sage::ui::parts {


// --- Общее для всех частей --------------------------------------------------

// Прозрачность части: своя альфа, помноженная на групповую и на бледность
// выключенного элемента.
float AlphaOf(const PartDrawContext& c, float channelAlpha) {
    float a = channelAlpha * c.Alpha;
    if (!c.Enabled) {
        const Interactable* act = c.Sibling<Interactable>();
        a *= (act && !c.EngineSkin) ? act->DisabledAlpha : 0.45f;
    }
    return a;
}

// Подкраска под состояние — для тех, у кого нет своих картинок состояний.
// Множители берутся у самого элемента: «на 12% ярче» подходит не всякому набору.
glm::vec3 StateTint(const PartDrawContext& c, glm::vec3 base) {
    const Interactable* act = c.Sibling<Interactable>();
    if (!act) return base;
    if (!c.Enabled) return glm::mix(base, glm::vec3(1.0f) * 0.5f, 0.5f);
    // Оформление движка — свои множители, одни на все элементы.
    const float pressed = c.EngineSkin ? 0.85f : act->PressedBrightness;
    const float hover = c.EngineSkin ? 1.15f : act->HoverBrightness;
    if (c.Pressed) return base * pressed;
    if (c.Hovered) return glm::mix(base, glm::vec3(1.0f), hover - 1.0f);
    return base;
}

// Скругление подложки, если она есть. Спрашивают трое — картинка-заглушка,
// маска и галка, — и все ЯВНО: это единственный законный вид знания о соседе.
float RoundingOf(const PartDrawContext& c) {
    const Fill* fill = c.Sibling<Fill>();
    return fill ? fill->Rounding * c.Scale : 0.0f;
}






void DrawPicture(UIRenderer& ui, const PictureParams& p, const UIRect& r, float canvasScale,
                 glm::vec3 rgb, float alpha) {
    if (!p.Tex || alpha <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    const UIRenderer::Sprite& src = p.Src;
    const auto mode = (Image::Mode)p.Fit;

    // РЕЖИМ РЕШАЕТ, ЧЕМ РИСОВАТЬ: девятина и замощение — нарезкой, остальное
    // одним квадом.
    // Замощение с заданным числом копий — плитка из доли элемента, по осям
    // отдельно; одна пиксельная шкала девятины такое не выражает.
    if (mode == Image::Mode::Tile && (p.Repeat.x > 0.0f || p.Repeat.y > 0.0f)) {
        const float srcW = src.Whole() ? (float)p.Tex->Width() : src.W;
        const float srcH = src.Whole() ? (float)p.Tex->Height() : src.H;
        float tileW = 0.0f, tileH = 0.0f;
        TileSize(srcW, srcH, p.Repeat.x, p.Repeat.y, r.w, r.h,
                 SlicedPixelScale(p.PixelScale, p.SnapPixels, canvasScale), tileW, tileH);
        ui.ImageTiled(r.x, r.y, r.w, r.h, p.Tex, src, tileW, tileH, rgb, alpha);
        return;
    }
    if (mode == Image::Mode::NineSlice || mode == Image::Mode::Tile) {
        NineSlice slice = p.Slice;
        if (mode == Image::Mode::Tile) {
            // Замощение — та же нарезка с нулевыми полями и повторяющейся
            // серединой: второй путь в отрисовке ради него не нужен.
            slice = NineSlice{};
            slice.CenterFill = SliceFill::Tile;
            slice.EdgeFill = SliceFill::Tile;
            slice.DrawCenter = true;
        }
        ui.ImageSliced(r.x, r.y, r.w, r.h, p.Tex, src, slice,
                       SlicedPixelScale(p.PixelScale, p.SnapPixels, canvasScale), rgb, alpha);
        return;
    }

    const float srcW = src.Whole() ? (float)p.Tex->Width() : src.W;
    const float srcH = src.Whole() ? (float)p.Tex->Height() : src.H;

    // СОХРАНЕНИЕ ПРОПОРЦИЙ — ДВА РАЗНЫХ ОТВЕТА, и оба нужны.
    //
    // Fit вписывает картинку ЦЕЛИКОМ: масштаб один на обе оси, по краям
    // остаются поля. Cover заполняет элемент БЕЗ ПОЛЕЙ, обрезая лишнее по
    // длинной стороне — обрезка делается ИСХОДНИКОМ (берём кусок нужных
    // пропорций от середины), а не рисованием за границей: у элемента может не
    // быть обрезки, и картинка вылезла бы на соседей.
    if (mode == Image::Mode::Fit || mode == Image::Mode::Cover) {
        const ImagePlacement fit =
            PlaceImage(r, src.Whole() ? 0.0f : src.X, src.Whole() ? 0.0f : src.Y, srcW, srcH,
                       mode == Image::Mode::Cover, p.SnapPixels);
        ui.ImageSprite(fit.Dst.x, fit.Dst.y, fit.Dst.w, fit.Dst.h, p.Tex,
                       {fit.SrcX, fit.SrcY, fit.SrcW, fit.SrcH}, rgb, alpha);
        return;
    }

    // Растяжение. С заданным размером пикселя или кратным масштабом картинка
    // рисуется своим размером по центру: разный дробный масштаб по осям даёт
    // рваные края у рисунка по пикселям, и элемент, оставшийся больше
    // картинки, честнее.
    UIRect dst = r;
    float pixels = p.PixelScale * canvasScale;
    if ((p.SnapPixels || pixels > 0.0f) && srcW > 0.0f && srcH > 0.0f) {
        if (pixels <= 0.0f) {
            pixels = std::min(r.w / srcW, r.h / srcH);
            if (p.SnapPixels) pixels = std::max(1.0f, std::floor(pixels));
        }
        dst.w = srcW * pixels;
        dst.h = srcH * pixels;
        dst.x = r.x + std::floor((r.w - dst.w) * 0.5f);
        dst.y = r.y + std::floor((r.h - dst.h) * 0.5f);
    }
    ui.ImageSprite(dst.x, dst.y, dst.w, dst.h, p.Tex, src, rgb, alpha);
}

// --- Вид (Look) ---------------------------------------------------------------

// Картинка вида — лениво, при отрисовке: путь правят в инспекторе посреди
// кадра, и повода перечитать файл, кроме отрисовки, нет (так же, как у части
// «Картинка», см. EnsureImageTexture).
const Texture* LookTexture(const Look& look) {
    if (look.Texture.empty()) {
        look.Tex.reset();
        look.TexPath.clear();
        return nullptr;
    }
    if (!look.Tex || look.TexPath != look.Texture || look.TexFiltering != look.Filtering) {
        // Резкая фильтрация — ближайшим соседом и без мипмапов: мипмапы ЛИСТА
        // подмешивают в края куска соседний спрайт.
        look.Tex = look.Sharp() ? ResourceManager::Instance().GetTexture(
                                      look.Texture, TextureFilter::Nearest, /*mipmaps=*/false)
                                : ResourceManager::Instance().GetTexture(look.Texture);
        look.TexPath = look.Texture;
        look.TexFiltering = look.Filtering;
    }
    return look.Tex.get();
}

float LookRounding(const Look& look, const UIRect& r, float scale) {
    return std::min(look.Rounding * scale, std::min(r.w, r.h) * 0.5f);
}

void DrawLookShadow(const PartDrawContext& c, const Look& look, const UIRect& r, float alpha) {
    if (look.ShadowSize <= 0.0f || look.ShadowColor.a <= 0.0f) return;
    c.Ui->RectShadow(r.x, r.y, r.w, r.h, LookRounding(look, r, c.Scale), look.ShadowSize * c.Scale,
                     look.ShadowColor.a * alpha, glm::vec3(look.ShadowColor));
}

// Тело вида: картинка или плашка цветом. rgb — уже подкрашенный под
// состояние цвет, alpha — итоговая прозрачность.
void DrawLookBody(const PartDrawContext& c, const Look& look, const UIRect& r, glm::vec3 rgb,
                  float alpha) {
    if (alpha <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    UIRenderer& ui = *c.Ui;
    if (look.HasTexture()) {
        const Texture* tex = LookTexture(look);
        if (!tex) {
            // Путь задан, а файла нет: «не загрузилось» должно отличаться от
            // «не назначено» — плашка цветом на месте картинки.
            ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha * 0.5f, LookRounding(look, r, c.Scale));
            return;
        }
        PictureParams p;
        p.Tex = tex;
        p.Src = {look.Sprite.x, look.Sprite.y, look.Sprite.z, look.Sprite.w};
        p.Fit = look.Fit;
        p.Slice = look.Slice();
        p.PixelScale = look.PixelScale;
        p.SnapPixels = look.SnapPixels;
        p.Repeat = look.Repeat;
        DrawPicture(ui, p, r, c.Scale, rgb, alpha);
        return;
    }
    const float rounding = LookRounding(look, r, c.Scale);
    if (look.Gradient.a > 0.0f) {
        ui.GradientRect(r.x, r.y, r.w, r.h, rgb, glm::vec3(look.Gradient), alpha,
                        look.Gradient.a * (alpha / std::max(look.Color.a, 1e-4f)), rounding);
    } else {
        ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha, rounding);
    }
}

void DrawLookBorder(const PartDrawContext& c, const Look& look, const UIRect& r, float alpha) {
    const float t = look.BorderThickness * c.Scale;
    if (t <= 0.0f || look.BorderColor.a <= 0.0f || alpha <= 0.0f) return;
    c.Ui->RoundedRectOutline(r.x, r.y, r.w, r.h, LookRounding(look, r, c.Scale), t,
                             glm::vec3(look.BorderColor), look.BorderColor.a * alpha);
}

// КАКОЙ ВИД У ЭЛЕМЕНТА СЕЙЧАС. Свой вид состояния, если он включён; иначе
// обычный, подкрашенный множителями (stateTinted = true).
const Look& StateLook(const PartDrawContext& c, const Look& normal, bool& stateTinted) {
    stateTinted = true;
    const Interactable* act = c.Sibling<Interactable>();
    if (!act || c.EngineSkin) return normal;   // у движка состояния — подкраской
    const Look* chosen = nullptr;
    if (!c.Enabled) {
        if (act->UseDisabledLook) chosen = &act->DisabledLook;
    } else if (c.Pressed && act->UsePressedLook) {
        chosen = &act->PressedLook;
    } else if (c.Focused && act->UseFocusedLook) {
        chosen = &act->FocusedLook;
    } else if (c.Hovered && act->UseHoverLook) {
        chosen = &act->HoverLook;
    }
    if (!chosen) return normal;
    stateTinted = false;
    return *chosen;
}

// Вид целиком — тень, тело, рамка — для составных частей (дорожка, ручка,
// квадратик галки, заполнение полосы). У подложки слои разнесены по порядку
// отрисовки (тень под всем, рамка поверх всего), здесь — сразу подряд.
void DrawLook(const PartDrawContext& c, const Look& look, const UIRect& r, bool tint) {
    const glm::vec3 rgb = tint ? StateTint(c, glm::vec3(look.Color)) : glm::vec3(look.Color);
    const float alpha = AlphaOf(c, 1.0f);
    DrawLookShadow(c, look, r, alpha);
    DrawLookBody(c, look, r, rgb, alpha * look.Color.a);
    DrawLookBorder(c, look, r, alpha);
}

// Пометить поля таблицы по ключам. Списком после таблицы, а не флагом в
// каждой строке инициализатора: что основное, а что редкое, читается одним
// взглядом и правится в одном месте.
void MarkAdvanced(std::vector<PartField>& v, std::initializer_list<const char*> keys) {
    for (PartField& f : v)
        for (const char* k : keys)
            if (f.Key && std::string_view(f.Key) == k) f.Advanced = true;
}
void MarkCustomOnly(std::vector<PartField>& v, std::initializer_list<const char*> keys) {
    for (PartField& f : v)
        for (const char* k : keys)
            if (f.Key && std::string_view(f.Key) == k) f.CustomOnly = true;
}
void MarkTab(std::vector<PartField>& v, const char* tab, const char* need,
             std::initializer_list<const char*> keys) {
    for (PartField& f : v)
        for (const char* k : keys)
            if (f.Key && std::string_view(f.Key) == k) {
                f.Tab = tab;
                if (need) f.Requires = need;
            }
}


Look EngineLook(const Look& src, EngineRole role) {
    Look l;
    l.Color = src.Color;
    const glm::vec3 c(src.Color);
    const float a = src.Color.a;
    auto lighter = [&](float k, float alpha) { return glm::vec4(glm::mix(c, glm::vec3(1.0f), k), alpha); };
    auto darker = [&](float k) { return glm::vec4(c * k, a); };
    switch (role) {
        case EngineRole::Panel:
            l.Rounding = 10.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.18f, 0.45f * a);
            l.Gradient = darker(0.86f);
            l.ShadowSize = 12.0f;
            l.ShadowColor = {0.0f, 0.0f, 0.0f, 0.30f * a};
            break;
        case EngineRole::Button:
            l.Rounding = 8.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.30f, 0.70f * a);
            l.Gradient = darker(0.78f);
            l.ShadowSize = 5.0f;
            l.ShadowColor = {0.0f, 0.0f, 0.0f, 0.30f * a};
            break;
        case EngineRole::Field:
            l.Rounding = 6.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.25f, 0.60f * a);
            break;
        case EngineRole::BarBack:
            l.Rounding = 64.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.20f, 0.45f * a);
            break;
        case EngineRole::BarFill:
        case EngineRole::SliderFill:
            l.Rounding = 64.0f;
            l.Color = lighter(0.12f, a);
            l.Gradient = darker(0.80f);
            break;
        case EngineRole::Track:
            l.Rounding = 64.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.20f, 0.45f * a);
            break;
        case EngineRole::Knob:
            l.Rounding = 64.0f;
            l.BorderThickness = 2.0f;
            l.BorderColor = {1.0f, 1.0f, 1.0f, 0.85f * a};
            l.ShadowSize = 4.0f;
            l.ShadowColor = {0.0f, 0.0f, 0.0f, 0.35f * a};
            break;
        case EngineRole::Box:
            l.Rounding = 5.0f;
            l.BorderThickness = 1.0f;
            l.BorderColor = lighter(0.35f, 0.80f * a);
            break;
        case EngineRole::Check:
            break;
    }
    return l;
}

// Роль подложки — по устройству элемента: поле ввода, кнопка, фон полосы или
// просто панель.
EngineRole FillRole(const PartDrawContext& c) {
    if (c.Sibling<TextInput>()) return EngineRole::Field;
    if (c.Sibling<Bar>()) return EngineRole::BarBack;
    if (c.Sibling<Interactable>()) return EngineRole::Button;
    return EngineRole::Panel;
}

// Таблица полей ОДНОГО вида — со смещениями от начала Look.
//
// Условия показа выстроены цепочкой: режим «как ложится» виден, только когда
// выбрана картинка; поля девятины — только в режиме девятины; скругление и
// градиент — только у плашки цветом (у картинки форму задаёт сам рисунок).
const std::vector<PartField>& LookFieldTable() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"color", SAGE_UI_TEXT("Colour"), PartField::Kind::Color, offsetof(Look, Color), 0.0f, 1.0f,
             "The plate colour. With a picture — the tint it is multiplied by."},
            {"texture", SAGE_UI_TEXT("Picture"), PartField::Kind::String, offsetof(Look, Texture), 0.0f,
             0.0f, "Empty — a plain colour plate.", nullptr, 0, PartField::Widget::Texture},
            {"fit", SAGE_UI_TEXT("How it fits"), PartField::Kind::Enum, offsetof(Look, Fit), 0.0f, 4.0f,
             "Stretch, cut into nine pieces (corners keep their size), repeat at\n"
             "its own size, or keep the aspect ratio.",
             kImageModeNames, 5, PartField::Widget::Auto, "texture", 1},
            {"sprite", SAGE_UI_TEXT("Sprite (x,y,w,h)"), PartField::Kind::Vec4, offsetof(Look, Sprite), 0.0f,
             4096.0f, "A piece of the sheet in source pixels; width 0 means the whole file.", nullptr, 0,
             PartField::Widget::Auto, "texture", 1},
            {"sliceBorder", SAGE_UI_TEXT("9-slice (l,t,r,b)"), PartField::Kind::Vec4,
             offsetof(Look, SliceBorder), 0.0f, 512.0f,
             "Fixed corners in source pixels. The 9-slice editor sets them by\n"
             "dragging lines over the picture itself.",
             nullptr, 0, PartField::Widget::NineSliceBorder, "fit", (int)Image::Mode::NineSlice},
            {"sliceCenterFill", SAGE_UI_TEXT("9-slice centre"), PartField::Kind::Enum,
             offsetof(Look, SliceCenterFill), 0.0f, 1.0f, "Stretch or repeat the middle piece.",
             kSliceFillNames, 2, PartField::Widget::Auto, "fit", (int)Image::Mode::NineSlice},
            {"sliceEdgeFill", SAGE_UI_TEXT("9-slice edges"), PartField::Kind::Enum,
             offsetof(Look, SliceEdgeFill), 0.0f, 1.0f,
             "Repeat keeps an ornament crisp; stretch smears it.", kSliceFillNames, 2,
             PartField::Widget::Auto, "fit", (int)Image::Mode::NineSlice},
            {"sliceDrawCenter", SAGE_UI_TEXT("9-slice draws the middle"), PartField::Kind::Bool,
             offsetof(Look, SliceDrawCenter), 0.0f, 1.0f,
             "An outline frame has no middle — what is under it shows through.", nullptr, 0,
             PartField::Widget::Auto, "fit", (int)Image::Mode::NineSlice},
            {"repeat", SAGE_UI_TEXT("Repeat count"), PartField::Kind::Vec2, offsetof(Look, Repeat), 0.0f,
             256.0f, kRepeatTip, nullptr, 0, PartField::Widget::Auto, "fit", (int)Image::Mode::Tile},
            {"pixelScale", SAGE_UI_TEXT("Source pixel size"), PartField::Kind::Float,
             offsetof(Look, PixelScale), 0.0f, 16.0f,
             "How many interface pixels one pixel of the file takes.\n"
             "0 — one to one for 9-slice and tile (corners keep their size),\n"
             "fit to the element for the other modes.",
             nullptr, 0, PartField::Widget::Auto, "texture", 1},
            {"snapPixels", SAGE_UI_TEXT("Whole-number scale"), PartField::Kind::Bool,
             offsetof(Look, SnapPixels), 0.0f, 1.0f,
             "Rounds the scale down to a whole number, so a frame drawn pixel by\n"
             "pixel does not go wavy.",
             nullptr, 0, PartField::Widget::Auto, "texture", 1},
            {"filter", SAGE_UI_TEXT("Filtering"), PartField::Kind::Enum, offsetof(Look, Filtering), 0.0f,
             1.0f, "Smooth for photos and anything scaled down; Nearest for crisp pixels.",
             kImageFilterNames, 2, PartField::Widget::Auto, "texture", 1},
            {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Look, Rounding), 0.0f,
             64.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "texture", 0},
            {"gradient", SAGE_UI_TEXT("Gradient down"), PartField::Kind::Color, offsetof(Look, Gradient),
             0.0f, 1.0f, "Alpha 0 means a flat fill.", nullptr, 0, PartField::Widget::Auto, "texture", 0},
            {"borderThickness", SAGE_UI_TEXT("Border width"), PartField::Kind::Float,
             offsetof(Look, BorderThickness), 0.0f, 16.0f},
            {"borderColor", SAGE_UI_TEXT("Border colour"), PartField::Kind::Color,
             offsetof(Look, BorderColor)},
            {"shadowSize", SAGE_UI_TEXT("Shadow"), PartField::Kind::Float, offsetof(Look, ShadowSize), 0.0f,
             48.0f, "Separates the element from a busy background. 0 — no shadow."},
            {"shadowColor", SAGE_UI_TEXT("Shadow colour"), PartField::Kind::Color,
             offsetof(Look, ShadowColor)},
        };
        // Основное у вида — цвет, картинка, как она ложится, нарезка и
        // скругление. Остальное трогают редко.
        MarkAdvanced(v, {"sprite", "sliceCenterFill", "sliceEdgeFill", "sliceDrawCenter", "pixelScale",
                         "snapPixels", "gradient", "borderThickness", "borderColor",
                         "shadowSize", "shadowColor"});
        // В оформлении движка у вида только цвет: форму, рамку, тень и
        // картинку задаёт движок.
        for (PartField& f : v)
            if (std::string_view(f.Key) != "color") f.CustomOnly = true;
        return v;
    }();
    return f;
}

// Постоянные строки для сдвинутых таблиц: ключи вида «hoverLook.color»
// составные, а поле таблицы хранит const char*. Список, а не вектор: адреса
// уже выданных строк обязаны пережить добавление новых.
const char* Intern(const std::string& s) {
    static std::list<std::string> pool;
    for (const std::string& have : pool)
        if (have == s) return have.c_str();
    pool.push_back(s);
    return pool.back().c_str();
}

// Поля вида, пересаженные внутрь компонента: смещение сдвинуто на место вида,
// ключи — с приставкой. Поля без своего условия показа наследуют условие
// самого вида («свой вид при наведении» — только когда он включён).
std::vector<PartField> ShiftLookFields(const char* prefix, size_t offset, const PartField* owner) {
    const char* showIfKey = owner ? owner->ShowIfKey : nullptr;
    const int showIfValue = owner ? owner->ShowIfValue : 0;
    std::vector<PartField> out;
    for (PartField f : LookFieldTable()) {
        // Вкладка и требование — у вида целиком: поля вида при наведении
        // живут на вкладке «при наведении» и нужны только при подложке.
        if (owner) {
            f.Tab = owner->Tab;
            f.Requires = owner->Requires;
            // Вид, который целиком только для своего оформления (вид при
            // наведении), — и все его поля тоже.
            if (owner->CustomOnly) f.CustomOnly = true;
        }
        f.Offset += offset;
        if (prefix && *prefix) {
            f.Key = Intern(std::string(prefix) + "." + f.Key);
            if (f.ShowIfKey) f.ShowIfKey = Intern(std::string(prefix) + "." + f.ShowIfKey);
            f.LookKey = prefix;
        }
        if (!f.ShowIfKey && showIfKey) {
            f.ShowIfKey = showIfKey;
            f.ShowIfValue = showIfValue;
        }
        out.push_back(f);
    }
    return out;
}


} // namespace sage::ui::parts
