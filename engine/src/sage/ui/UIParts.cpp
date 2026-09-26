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
#include "sage/ui/ImageFit.h"
#include "sage/ui/UIPart.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <list>
#include <string_view>
#include <string>
#include <vector>

#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"

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

// Имена значений SliceFill для таблицы полей: enum пишется числом, но человек
// видит слово — и в инспекторе, и в подсказке.
const char* const kSliceFillNames[] = {SAGE_UI_TEXT("Stretch"), SAGE_UI_TEXT("Repeat")};

// Фильтрация — общий список на картинку и на шрифт: настройка одна и та же,
// и два разных набора подписей для неё разошлись бы на первой же правке.
const char* const kImageFilterNames[] = {SAGE_UI_TEXT("Smooth"), SAGE_UI_TEXT("Nearest")};

// Режимы картинки. Имена — про то, ЧТО СТАНЕТ С КАРТИНКОЙ, а не про
// механику: «девятина» без объяснения не говорит ничего, «углы неподвижны» —
// говорит всё.
const char* const kImageModeNames[] = {SAGE_UI_TEXT("Stretch"), SAGE_UI_TEXT("9-slice"),
                                       SAGE_UI_TEXT("Tile"), SAGE_UI_TEXT("Keep aspect"),
                                       SAGE_UI_TEXT("Fill, keep aspect")};

// --- Картинка в прямоугольнике — ОДНА на картинку и на любой вид ---------------
//
// Рисует и часть «Картинка», и вид с текстурой (подложка кнопки, дорожка
// ползунка, отметка галки, заполнение полосы). Одна функция, а не две копии:
// девятина, починенная в одной, иначе осталась бы сломанной в другой.
struct PictureParams {
    const Texture* Tex = nullptr;
    UIRenderer::Sprite Src;
    int Fit = 0;                  // как Image::Mode
    NineSlice Slice;              // для девятины
    float PixelScale = 0.0f;
    bool SnapPixels = false;
};

void DrawPicture(UIRenderer& ui, const PictureParams& p, const UIRect& r, float canvasScale,
                 glm::vec3 rgb, float alpha) {
    if (!p.Tex || alpha <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    const UIRenderer::Sprite& src = p.Src;
    const auto mode = (Image::Mode)p.Fit;

    // РЕЖИМ РЕШАЕТ, ЧЕМ РИСОВАТЬ: девятина и замощение — нарезкой, остальное
    // одним квадом.
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
void DrawLook(const PartDrawContext& c, const Look& look, const UIRect& r, bool tint = true) {
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

// --- Оформление движка ------------------------------------------------------------
//
// Одна тема на все элементы: человек задаёт ЦВЕТ, а форму, рамку, градиент и
// тень движок выводит из него сам — по роли поверхности. Числа подобраны так,
// чтобы кнопка, поле, панель и ползунок, поставленные рядом, выглядели одним
// набором, а не четырьмя случайными прямоугольниками.
enum class EngineRole { Panel, Button, Field, BarBack, BarFill, Track, SliderFill, Knob, Box, Check };

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
                         "snapPixels", "filter", "gradient", "borderThickness", "borderColor",
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
        MarkAdvanced(v, {"sprite", "sliceCenterFill", "sliceEdgeFill", "sliceDrawCenter", "pixelScale",
                         "snapPixels", "filter"});
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

// --- Диапазон: галка и ползунок ---------------------------------------------

const std::vector<PartField>& RangeFields() {
    // Поля ползунка и поля галки разведены условием «галка»: у галки нет ни
    // дорожки с ручкой, ни шага, у ползунка — отметки. Показанные всегда, они
    // читались бы как «работает, просто ничего не делает».
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"value", SAGE_UI_TEXT("Value"), PartField::Kind::Float, offsetof(Range, Value), -1000.0f,
             1000.0f},
            {"min", SAGE_UI_TEXT("Minimum"), PartField::Kind::Float, offsetof(Range, Min), -1000.0f, 1000.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"max", SAGE_UI_TEXT("Maximum"), PartField::Kind::Float, offsetof(Range, Max), -1000.0f, 1000.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"step", SAGE_UI_TEXT("Step"), PartField::Kind::Float, offsetof(Range, Step), 0.0f, 100.0f,
             "0 is smooth; above zero snaps to the step (volume in 5% notches).", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            // «Галка» — не настройка, а ТИП: её задаёт заготовка, и в
            // инспекторе её не переключают (галка с дорожкой и ручкой — это
            // уже не галка). Читается и пишется ради старых сцен.
            {"toggle", SAGE_UI_TEXT("Checkbox"), PartField::Kind::Bool, offsetof(Range, Toggle)},
            {"trackThickness", SAGE_UI_TEXT("Track thickness"), PartField::Kind::Float,
             offsetof(Range, TrackThickness), 0.02f, 1.0f, "A share of the element height.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            {"knobSize", SAGE_UI_TEXT("Knob size"), PartField::Kind::Float, offsetof(Range, KnobSize), 0.0f,
             2.0f, "A share of the element height; the knob is square. 0 — no knob.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            {"track", SAGE_UI_TEXT("Track"), PartField::Kind::Look, offsetof(Range, Track), 0.0f, 0.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"filled", SAGE_UI_TEXT("Filled part"), PartField::Kind::Look, offsetof(Range, Filled), 0.0f,
             0.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"knob", SAGE_UI_TEXT("Knob"), PartField::Kind::Look, offsetof(Range, Knob), 0.0f, 0.0f, nullptr,
             nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"box", SAGE_UI_TEXT("Box"), PartField::Kind::Look, offsetof(Range, Track), 0.0f, 0.0f, nullptr,
             nullptr, 0, PartField::Widget::Auto, "toggle", 1},
            {"check", SAGE_UI_TEXT("Check mark"), PartField::Kind::Look, offsetof(Range, Check), 0.0f, 0.0f,
             "Without a picture — the engine's check mark in this colour.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 1},
        };
        v[0].Content = v[1].Content = v[2].Content = v[3].Content = v[4].Content = true;
        v[4].Hidden = true;
        // «Квадратик» галки — это та же дорожка (Track), показанная под своим
        // именем. В файл второй раз не пишется: один вид, одно место.
        v[10].EditorOnly = true;
        MarkAdvanced(v, {"trackThickness", "knobSize"});
        MarkCustomOnly(v, {"trackThickness", "knobSize"});
        return v;
    }();
    return f;
}

float RangeFraction(const Range& range) {
    const float span = range.Max - range.Min;
    if (std::fabs(span) < 1e-6f) return 0.0f;
    return std::clamp((range.Value - range.Min) / span, 0.0f, 1.0f);
}

UIRect ToggleBox(const UIRect& r) {
    const float side = std::min(r.w, r.h);
    return {r.x, r.y + (r.h - side) * 0.5f, side, side};
}

void DrawRangePart(const PartDrawContext& c) {
    const Range& source = *static_cast<const Range*>(c.Data);
    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    // Оформление движка: те же значение и цвета, но форма — из темы.
    Range engine;
    if (c.EngineSkin) {
        engine = source;
        engine.Track = EngineLook(source.Track, source.Toggle ? EngineRole::Box : EngineRole::Track);
        engine.Filled = EngineLook(source.Filled, EngineRole::SliderFill);
        engine.Knob = EngineLook(source.Knob, EngineRole::Knob);
        engine.Check = EngineLook(source.Check, EngineRole::Check);
        engine.TrackThickness = 0.28f;
        engine.KnobSize = 1.0f;
    }
    const Range& range = c.EngineSkin ? engine : source;

    if (range.Toggle) {
        // Галка: квадратик у левого края СВОЕГО элемента. Подпись к нему —
        // отдельный объект рядом, а не сдвинутый текст внутри: так галку можно
        // поставить и справа от подписи, и под ней.
        const UIRect box = ToggleBox(r);
        DrawLook(c, range.Track, box);
        // Включена — если значение ближе к верхнему концу диапазона.
        if (range.Value < (range.Min + range.Max) * 0.5f) return;
        if (range.Check.HasTexture()) {
            DrawLook(c, range.Check, box, /*tint=*/false);
            return;
        }
        const float pad = box.h * 0.18f;
        const glm::vec3 rgb = StateTint(c, glm::vec3(range.Check.Color));
        DrawIcon(ui, "check", box.x + pad, box.y + pad, box.h - pad * 2.0f, rgb,
                 AlphaOf(c, range.Check.Color.a));
        return;
    }

    // Ползунок: дорожка по центру, ручка поверх. Попасть в тонкую полоску
    // мышью трудно, а нажатие ловит весь элемент.
    const float thick = std::max(r.h * std::clamp(range.TrackThickness, 0.0f, 1.0f), 2.0f);
    const UIRect track{r.x, r.y + (r.h - thick) * 0.5f, r.w, thick};
    DrawLook(c, range.Track, track);

    const float t = RangeFraction(range);
    const float knob = r.h * std::max(range.KnobSize, 0.0f);
    // Ручка не выезжает за концы: её центр ходит от половины ручки до
    // «ширина минус половина».
    const float kx = r.x + knob * 0.5f + (r.w - knob) * t;
    if (t > 0.0f) {
        UIRect filled = track;
        filled.w = std::max(0.0f, kx - track.x);
        if (knob <= 0.0f) filled.w = track.w * t;
        DrawLook(c, range.Filled, filled);
    }
    if (knob > 0.0f) {
        DrawLook(c, range.Knob, {kx - knob * 0.5f, r.y + (r.h - knob) * 0.5f, knob, knob});
    }
}

// --- Поле ввода -------------------------------------------------------------

const std::vector<PartField>& TextInputFields() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"placeholder", SAGE_UI_TEXT("Placeholder"), PartField::Kind::String,
             offsetof(TextInput, Placeholder)},
            {"maxLength", SAGE_UI_TEXT("Max length"), PartField::Kind::Int, offsetof(TextInput, MaxLength),
             0.0f, 4096.0f, "0 means no limit."},
            {"password", SAGE_UI_TEXT("Password"), PartField::Kind::Bool, offsetof(TextInput, Password), 0.0f,
             1.0f, "Hide the content behind dots."},
            {"readOnly", SAGE_UI_TEXT("Read only"), PartField::Kind::Bool, offsetof(TextInput, ReadOnly)},
            {"placeholderColor", SAGE_UI_TEXT("Placeholder colour"), PartField::Kind::Color,
             offsetof(TextInput, PlaceholderColor), 0.0f, 1.0f,
             "Alpha 0 — the text colour, half as bright."},
            {"caretColor", SAGE_UI_TEXT("Caret colour"), PartField::Kind::Color,
             offsetof(TextInput, CaretColor), 0.0f, 1.0f, "Alpha 0 — the text colour."},
            {"caretWidth", SAGE_UI_TEXT("Caret width"), PartField::Kind::Float,
             offsetof(TextInput, CaretWidth), 0.5f, 8.0f},
        };
        for (int i = 0; i < 4; ++i) v[(size_t)i].Content = true;
        MarkAdvanced(v, {"readOnly", "placeholderColor", "caretColor", "caretWidth"});
        MarkCustomOnly(v, {"placeholderColor", "caretColor", "caretWidth"});
        return v;
    }();
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
    const UITextStyle style = StyleOf(*label, ui);
    // По реальной высоте строки, а не по номиналу: иначе содержимое поля
    // стоит не по центру, и тем заметнее, чем крупнее кегль.
    const float y = r.y + (r.h - ui.LineHeight(textScale, style)) * 0.5f;
    const bool empty = label->Text.empty();
    if (empty && !input.Placeholder.empty()) {
        // Подсказка бледнее содержимого — иначе пустое поле выглядит
        // заполненным, и человек стирает то, чего не вводил.
        const bool own = input.PlaceholderColor.a > 0.0f && !c.EngineSkin;
        const glm::vec3 prgb = own ? glm::vec3(input.PlaceholderColor) : rgb;
        const float pa = own ? input.PlaceholderColor.a : label->Color.a * 0.45f;
        ui.Text(left, y, textScale, prgb, input.Placeholder, AlphaOf(c, pa), style);
    } else if (!empty) {
        ui.Text(left, y, textScale, rgb, input.Password ? MaskText(label->Text) : label->Text,
                AlphaOf(c, label->Color.a), style);
    }

    // Курсор мигает только в фокусе и только когда поле включено.
    const Interactable* act = c.Sibling<Interactable>();
    if (!c.Focused || !c.Enabled || !act) return;
    const int caret = std::clamp(act->Runtime.Caret, 0, (int)label->Text.size());
    const std::string head = label->Text.substr(0, (size_t)caret);
    const float cx = left + ui.MeasureText(input.Password ? MaskText(head) : head, textScale, style);
    // Полсекунды виден, полсекунды нет; после каждой правки счётчик
    // сбрасывается, чтобы курсор не пропал ровно тогда, когда на него смотрят.
    if (std::fmod(act->Runtime.CaretBlink, 1.0f) < 0.5f) {
        const float ch = ui.LineHeight(textScale, style) * 0.92f;
        const bool own = input.CaretColor.a > 0.0f && !c.EngineSkin;
        const float caretW = c.EngineSkin ? 2.0f : input.CaretWidth;
        ui.Rect(cx, r.y + (r.h - ch) * 0.5f, std::max(caretW * c.Scale, 1.0f), ch,
                own ? glm::vec3(input.CaretColor) : rgb,
                AlphaOf(c, own ? input.CaretColor.a : label->Color.a));
    }
}

// --- Реакция на мышь -----------------------------------------------------------

const std::vector<PartField>& InteractableFields() {
    // Вид состояния показывается под своей галкой: выключена — подложка
    // подкрашивается множителями (их поля и видны), включена — рисуется свой
    // вид целиком (и видны его поля).
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"enabled", SAGE_UI_TEXT("Enabled"), PartField::Kind::Bool, offsetof(Interactable, Enabled)},
            {"action", SAGE_UI_TEXT("Action"), PartField::Kind::String, offsetof(Interactable, Action), 0.0f,
             0.0f,
             "A name for the game: a script asks whether Continue was pressed, not\n"
             "whether entity 37 was."},
            {"cursor", SAGE_UI_TEXT("Cursor"), PartField::Kind::String, offsetof(Interactable, Cursor)},
            {"hoverBrightness", SAGE_UI_TEXT("Brighter on hover"), PartField::Kind::Float,
             offsetof(Interactable, HoverBrightness), 0.5f, 2.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "useHoverLook", 0},
            {"pressedBrightness", SAGE_UI_TEXT("Darker when pressed"), PartField::Kind::Float,
             offsetof(Interactable, PressedBrightness), 0.5f, 2.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "usePressedLook", 0},
            {"disabledAlpha", SAGE_UI_TEXT("Disabled alpha"), PartField::Kind::Float,
             offsetof(Interactable, DisabledAlpha), 0.0f, 1.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "useDisabledLook", 0},
            {"pressedOffset", SAGE_UI_TEXT("Content shift when pressed"), PartField::Kind::Vec2,
             offsetof(Interactable, PressedOffset), -16.0f, 16.0f,
             "Moves everything inside the button while it is held — a caption\n"
             "that sinks together with a pressed picture."},
            {"useHoverLook", SAGE_UI_TEXT("Own look on hover"), PartField::Kind::Bool,
             offsetof(Interactable, UseHoverLook)},
            {"hoverLook", SAGE_UI_TEXT("On hover"), PartField::Kind::Look, offsetof(Interactable, HoverLook),
             0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "useHoverLook", 1},
            {"usePressedLook", SAGE_UI_TEXT("Own look when pressed"), PartField::Kind::Bool,
             offsetof(Interactable, UsePressedLook)},
            {"pressedLook", SAGE_UI_TEXT("When pressed"), PartField::Kind::Look,
             offsetof(Interactable, PressedLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "usePressedLook", 1},
            {"useFocusedLook", SAGE_UI_TEXT("Own look when focused"), PartField::Kind::Bool,
             offsetof(Interactable, UseFocusedLook)},
            {"focusedLook", SAGE_UI_TEXT("When focused"), PartField::Kind::Look,
             offsetof(Interactable, FocusedLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "useFocusedLook", 1},
            {"useDisabledLook", SAGE_UI_TEXT("Own look when disabled"), PartField::Kind::Bool,
             offsetof(Interactable, UseDisabledLook)},
            {"disabledLook", SAGE_UI_TEXT("When disabled"), PartField::Kind::Look,
             offsetof(Interactable, DisabledLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "useDisabledLook", 1},
            {"events", SAGE_UI_TEXT("Events"), PartField::Kind::Bindings, offsetof(Interactable, Events), 0.0f,
             0.0f,
             "What the element does itself: send an event, call a method on another\n"
             "object. Without this a button needs a script polling it every frame."},
        };
        v[0].Content = v[1].Content = v[2].Content = true;
        v.back().Content = true;
        // Состояния — ВКЛАДКАМИ, и свой вид состояния рисует подложка: без
        // неё (у ползунка, галки) эти поля не делают ничего и не показываются.
        MarkTab(v, SAGE_UI_TEXT("On hover"), nullptr, {"hoverBrightness"});
        MarkTab(v, SAGE_UI_TEXT("On hover"), "fill", {"useHoverLook", "hoverLook"});
        MarkTab(v, SAGE_UI_TEXT("When pressed"), nullptr, {"pressedBrightness", "pressedOffset"});
        MarkTab(v, SAGE_UI_TEXT("When pressed"), "fill", {"usePressedLook", "pressedLook"});
        MarkTab(v, SAGE_UI_TEXT("When focused"), "textInput", {"useFocusedLook", "focusedLook"});
        MarkTab(v, SAGE_UI_TEXT("When disabled"), nullptr, {"disabledAlpha"});
        MarkTab(v, SAGE_UI_TEXT("When disabled"), "fill", {"useDisabledLook", "disabledLook"});
        MarkAdvanced(v, {"cursor"});
        // Виды и подкраска состояний — своё оформление; у движка они свои.
        MarkCustomOnly(v, {"hoverBrightness", "pressedBrightness", "disabledAlpha", "pressedOffset",
                           "useHoverLook", "hoverLook", "usePressedLook", "pressedLook",
                           "useFocusedLook", "focusedLook", "useDisabledLook", "disabledLook"});
        return v;
    }();
    return f;
}

const char* const kMaskShape[] = {SAGE_UI_TEXT("Rectangle"), SAGE_UI_TEXT("Rounded")};

const std::vector<PartField>& MaskFields() {
    // Обрезка прямоугольная: форма и скругление в файле есть (старые сцены),
    // но отрисовка режет только прямоугольником — показывать их значило бы
    // обещать то, чего не происходит.
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"form", SAGE_UI_TEXT("Shape"), PartField::Kind::Enum, offsetof(Mask, Form), 0.0f, 0.0f, nullptr,
             kMaskShape, 2},
            {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Mask, Rounding), -1.0f,
             64.0f},
            {"padding", SAGE_UI_TEXT("Clip inset (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Mask, Padding),
             0.0f, 256.0f, "Shrinks the clipping window inside the container."},
            {"showOutside", SAGE_UI_TEXT("Do not clip"), PartField::Kind::Bool, offsetof(Mask, ShowOutside)},
        };
        v[0].Hidden = v[1].Hidden = v[3].Hidden = true;
        return v;
    }();
    return f;
}

const char* const kFlow[] = {SAGE_UI_TEXT("Row"), SAGE_UI_TEXT("Column"), SAGE_UI_TEXT("Grid")};
const char* const kJustify[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("End"),
                                SAGE_UI_TEXT("Even")};
const char* const kCross[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("End"),
                              SAGE_UI_TEXT("Stretch")};

const std::vector<PartField>& LayoutFields() {
    // Поля сетки видны только у сетки, перенос — только у ряда и столбца:
    // «столбцы» у ряда и «перенос» у сетки не значат ничего.
    static const std::vector<PartField> f = {
        {"direction", SAGE_UI_TEXT("Direction"), PartField::Kind::Enum, offsetof(Stack, Direction), 0.0f, 0.0f,
         nullptr, kFlow, 3},
        {"justify", SAGE_UI_TEXT("Along"), PartField::Kind::Enum, offsetof(Stack, Justify), 0.0f, 0.0f,
         "Where the children gather along the main direction when there is\n"
         "room left. Children with Grow take that room instead.",
         kJustify, 4},
        {"cross", SAGE_UI_TEXT("Across"), PartField::Kind::Enum, offsetof(Stack, Cross), 0.0f, 0.0f,
         "Where each child stands across the main direction — or stretch it\n"
         "to the full width (height) of the container.",
         kCross, 4},
        {"spacing", SAGE_UI_TEXT("Spacing"), PartField::Kind::Float, offsetof(Stack, Spacing), 0.0f, 128.0f},
        {"padding", SAGE_UI_TEXT("Padding (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Stack, Padding), 0.0f,
         256.0f},
        {"wrap", SAGE_UI_TEXT("Wrap"), PartField::Kind::Bool, offsetof(Stack, Wrap), 0.0f, 1.0f,
         "Children that do not fit go on to the next line.", nullptr, 0, PartField::Widget::Auto,
         "direction", ShowIfNot((int)Stack::Flow::Grid)},
        {"columns", SAGE_UI_TEXT("Columns"), PartField::Kind::Int, offsetof(Stack, Columns), 0.0f, 32.0f,
         "0 — as many as fit at the cell size.", nullptr, 0, PartField::Widget::Auto, "direction",
         (int)Stack::Flow::Grid},
        {"cellSize", SAGE_UI_TEXT("Cell size"), PartField::Kind::Vec2, offsetof(Stack, CellSize), 0.0f,
         2048.0f, "0 — width from the columns, height from the tallest child.", nullptr, 0,
         PartField::Widget::Auto, "direction", (int)Stack::Flow::Grid},
        {"fitContent", SAGE_UI_TEXT("Size from content"), PartField::Kind::Bool,
         offsetof(Stack, FitContent)},
    };
    static const bool marked = [] {
        MarkAdvanced(const_cast<std::vector<PartField>&>(f), {"cellSize", "fitContent", "padding"});
        return true;
    }();
    (void)marked;
    return f;
}

const std::vector<PartField>& ScrollFields() {
    static const std::vector<PartField> f = {
        {"offset", SAGE_UI_TEXT("Offset"), PartField::Kind::Vec2, offsetof(Scroll, Offset), -100000.0f,
         100000.0f, "How far the content has moved. Also the starting position."},
        {"horizontal", SAGE_UI_TEXT("Scroll sideways"), PartField::Kind::Bool,
         offsetof(Scroll, Horizontal)},
        {"vertical", SAGE_UI_TEXT("Scroll up and down"), PartField::Kind::Bool,
         offsetof(Scroll, Vertical)},
        {"speed", SAGE_UI_TEXT("Wheel step"), PartField::Kind::Float, offsetof(Scroll, Speed), 1.0f,
         400.0f},
        {"clamp", SAGE_UI_TEXT("Stop at the edges"), PartField::Kind::Bool, offsetof(Scroll, Clamp),
         0.0f, 1.0f, "Off where the edge is deliberate: a map, an endless feed."},
    };
    static const bool marked = [] {
        MarkAdvanced(const_cast<std::vector<PartField>&>(f), {"offset", "speed", "clamp"});
        return true;
    }();
    (void)marked;
    return f;
}

const char* const kCanvasScale[] = {SAGE_UI_TEXT("Pixels"), SAGE_UI_TEXT("Scale to reference"),
                                    SAGE_UI_TEXT("Whole-number scale (pixel art)")};

const std::vector<PartField>& CanvasFields() {
    static const std::vector<PartField> f = {
        {"mode", SAGE_UI_TEXT("Scale mode"), PartField::Kind::Enum, offsetof(Canvas, Mode), 0.0f, 0.0f, nullptr,
         kCanvasScale, 3},
        {"reference", SAGE_UI_TEXT("Reference resolution"), PartField::Kind::Vec2, offsetof(Canvas, Reference),
         64.0f, 8192.0f},
        {"matchWidthOrHeight", SAGE_UI_TEXT("Follow"), PartField::Kind::Float,
         offsetof(Canvas, MatchWidthOrHeight), 0.0f, 1.0f,
         "0 follows the width, 1 the height, 0.5 the average."},
        {"maxScale", SAGE_UI_TEXT("Largest scale"), PartField::Kind::Int, offsetof(Canvas, MaxScale), 0.0f,
         16.0f, "Whole-number scale only. 0 — as large as fits."},
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

    // Реакция на мышь — УСТРОЙСТВО ТИПА (кнопка, галка, ползунок, поле
    // ввода), а не добавка: её не включают галкой на картинке. Рисовать ей
    // нечего — виды состояний рисует подложка.
    PartType act = MakePart<Interactable>("interactable", SAGE_UI_TEXT("Interaction"), 100,
                                          &InteractableFields());
    act.Icon = "cube";
    act.Hint = SAGE_UI_TEXT("Hover, press, click, looks for each state and what the element does");
    RegisterPart(act);

    // --- Части КОНТЕЙНЕРОВ: невидимая раскладка детей ------------------------
    PartType mask = MakePart<Mask>("mask", SAGE_UI_TEXT("Clipping"), 100, &MaskFields());
    mask.Icon = "rect";
    mask.Hint = SAGE_UI_TEXT("Children are clipped by this container");
    mask.Container = true;
    RegisterPart(mask);

    PartType layout = MakePart<Stack>("layout", SAGE_UI_TEXT("Layout"), 100, &LayoutFields());
    layout.Icon = "layout";
    layout.Hint = SAGE_UI_TEXT("Children stand in a row, a column or a grid");
    layout.Container = true;
    RegisterPart(layout);

    PartType scroll = MakePart<Scroll>("scroll", SAGE_UI_TEXT("Scrolling"), 100, &ScrollFields());
    scroll.Icon = "list";
    scroll.Hint = SAGE_UI_TEXT("The content moves inside the container and is clipped by it");
    scroll.Container = true;
    RegisterPart(scroll);

    // Холст корня — из тех времён, когда корнем интерфейса был элемент. Теперь
    // масштаб задаёт сам интерфейс; часть читается ради старых сцен.
    PartType canvas = MakePart<Canvas>("canvas", SAGE_UI_TEXT("Canvas"), 100, &CanvasFields());
    canvas.Icon = "grid";
    canvas.Hint = SAGE_UI_TEXT("A UI root: reference resolution and order between roots");
    canvas.Hidden = true;
    RegisterPart(canvas);

    // Прозрачность и приём ввода всего поддерева — общая настройка ЛЮБОГО
    // элемента (инспектор показывает её в разделе «Элемент»), а не добавка.
    PartType group = MakePart<Group>("group", SAGE_UI_TEXT("Opacity and input"), 100, &GroupFields());
    group.Icon = "copy";
    group.Hint = SAGE_UI_TEXT("Opacity and input for the whole subtree");
    group.Hidden = true;
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
                                   UIRenderer& ui, const UITextStyle* styleOrNull) {
    const UITextStyle style = styleOrNull ? *styleOrNull : UITextStyle{};
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
            if (ui.MeasureText(candidate, scale, style) <= maxWidth || line.empty()) {
                if (ui.MeasureText(candidate, scale, style) > maxWidth && line.empty()) {
                    // Слово шире строки — режем по символам (по границам UTF-8).
                    std::string chunk;
                    size_t c = 0;
                    while (c < word.size()) {
                        const size_t next = (size_t)NextCharBoundary(word, (int)c);
                        const std::string grown = chunk + word.substr(c, next - c);
                        if (!chunk.empty() && ui.MeasureText(grown, scale, style) > maxWidth) {
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

void CopyField(const PartField& f, const void* src, void* dst) {
    if (!src || !dst) return;
    const char* s = static_cast<const char*>(src) + f.Offset;
    char* d = static_cast<char*>(dst) + f.Offset;
    switch (f.Type) {
        case PartField::Kind::Bool:   *reinterpret_cast<bool*>(d) = *reinterpret_cast<const bool*>(s); break;
        // Enum хранится int'ом — тем же, что и Int: перечисление в компоненте
        // объявлено через int, иначе offsetof по нему не работал бы.
        case PartField::Kind::Int:
        case PartField::Kind::Enum:   *reinterpret_cast<int*>(d) = *reinterpret_cast<const int*>(s); break;
        case PartField::Kind::Float:  *reinterpret_cast<float*>(d) = *reinterpret_cast<const float*>(s); break;
        case PartField::Kind::String: *reinterpret_cast<std::string*>(d) = *reinterpret_cast<const std::string*>(s); break;
        case PartField::Kind::Vec2:   *reinterpret_cast<glm::vec2*>(d) = *reinterpret_cast<const glm::vec2*>(s); break;
        case PartField::Kind::Color:
        case PartField::Kind::Vec4:   *reinterpret_cast<glm::vec4*>(d) = *reinterpret_cast<const glm::vec4*>(s); break;
        case PartField::Kind::Bindings:
            *reinterpret_cast<sage::events::Bindings*>(d) =
                *reinterpret_cast<const sage::events::Bindings*>(s);
            break;
        case PartField::Kind::Look:
            *reinterpret_cast<Look*>(d) = *reinterpret_cast<const Look*>(s);
            break;
    }
}

const std::vector<PartField>& LookFields() { return LookFieldTable(); }

void BakeEngineSkin(entt::registry& reg, entt::entity e) {
    // Роль подложки — тем же правилом, что при отрисовке.
    PartDrawContext c;
    c.Reg = &reg;
    c.Entity = e;
    if (Fill* fill = reg.try_get<Fill>(e)) {
        const Look l = EngineLook(*fill, FillRole(c));
        static_cast<Look&>(*fill) = l;
    }
    if (Bar* bar = reg.try_get<Bar>(e)) {
        bar->Filled = EngineLook(bar->Filled, EngineRole::BarFill);
        bar->Padding = glm::vec4(3.0f);
        bar->Mode = Bar::FillMode::Stretch;
    }
    if (Range* r = reg.try_get<Range>(e)) {
        r->Track = EngineLook(r->Track, r->Toggle ? EngineRole::Box : EngineRole::Track);
        r->Filled = EngineLook(r->Filled, EngineRole::SliderFill);
        r->Knob = EngineLook(r->Knob, EngineRole::Knob);
        r->Check = EngineLook(r->Check, EngineRole::Check);
        r->TrackThickness = 0.28f;
        r->KnobSize = 1.0f;
    }
    if (Interactable* act = reg.try_get<Interactable>(e)) {
        act->HoverBrightness = 1.15f;
        act->PressedBrightness = 0.85f;
        act->DisabledAlpha = 0.45f;
        act->UseHoverLook = act->UsePressedLook = act->UseFocusedLook = act->UseDisabledLook = false;
    }
    if (Label* label = reg.try_get<Label>(e)) {
        label->ShadowOffset = glm::vec2(0.0f);
        label->OutlineWidth = 0.0f;
        label->StateColors = false;
    }
    if (TextInput* in = reg.try_get<TextInput>(e)) {
        in->CaretColor = glm::vec4(0.0f);
        in->PlaceholderColor = glm::vec4(0.0f);
        in->CaretWidth = 2.0f;
    }
}

const std::vector<PartField>& LookFieldsOf(const PartField& lookField) {
    // Таблица на каждый вид строится один раз: редактор спрашивает её каждый
    // кадр, а ключи в ней — постоянные строки.
    static std::list<std::pair<std::string, std::vector<PartField>>> cache;
    const std::string id = std::string(lookField.Key ? lookField.Key : "") + "@" +
                           std::to_string(lookField.Offset);
    for (const auto& [key, fields] : cache)
        if (key == id) return fields;
    cache.emplace_back(id, ShiftLookFields(Intern(lookField.Key ? lookField.Key : ""),
                                           lookField.Offset, &lookField));
    return cache.back().second;
}

std::vector<PartField> EditableFields(const std::vector<PartField>& fields) {
    std::vector<PartField> out;
    for (const PartField& f : fields) {
        out.push_back(f);
        if (f.Type != PartField::Kind::Look) continue;
        const std::vector<PartField>& inner = LookFieldsOf(f);
        out.insert(out.end(), inner.begin(), inner.end());
    }
    return out;
}

const PartType* FindPart(std::string_view id) {
    for (const PartType& p : Parts())
        if (p.Id && id == p.Id) return &p;
    return nullptr;
}

} // namespace sage::ui
