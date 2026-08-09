#include "UISceneSystem.h"

#include "sage/ui/UI.h"
#include "sage/ui/UILegacy.h"
#include "sage/core/Profiler.h"
#include "UIRenderer.h"
#include "UIIcons.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace sage::ui {

bool IsElement(const entt::registry& reg, entt::entity e) {
    return reg.valid(e) && reg.all_of<Transform>(e);
}

namespace {

// Ширина элемента по СОДЕРЖИМОМУ: отступ, значок, отступ, текст, отступ.
// Высота не подгоняется — её задаёт вёрстка (строка интерфейса одна на всех).
//
// Живёт здесь, а не в решателе раскладки (UI.cpp), потому что единственный, кто
// знает ширину строки, — шрифт, а он есть только у отрисовки.
glm::vec2 MeasuredWidth(const entt::registry& reg, entt::entity e, glm::vec2 size,
                        UIRenderer& ui) {
    const Label* label = reg.try_get<Label>(e);
    if (!label || !label->AutoWidth) return size;
    const Icon* icon = reg.try_get<Icon>(e);
    const bool hasIcon = icon && !icon->Name.empty() && icon->Color.a > 0.0f;
    // Значок занимает квадрат в высоту элемента — текст начинается за ним
    // (ровно там же, где его кладёт DrawElement).
    float w = hasIcon ? size.y : label->PadX;
    if (!label->Text.empty() && label->Color.a > 0.0f) w += ui.MeasureText(label->Text, label->Scale);
    return {glm::max(w + label->PadX, size.y), size.y};
}

// Дети сущности с интерфейсной частью, отсортированные по Layer (стабильно;
// при равенстве — по Id, чтобы порядок был детерминирован).
std::vector<entt::entity> SortedUIChildren(Scene& scene, entt::entity parent) {
    std::vector<entt::entity> kids;
    entt::registry& reg = scene.Registry();
    if (const auto* h = reg.try_get<HierarchyComponent>(parent)) {
        for (auto c : h->Children)
            if (IsElement(reg, c)) kids.push_back(c);
    }
    std::stable_sort(kids.begin(), kids.end(), [&reg](entt::entity a, entt::entity b) {
        const Transform& ta = reg.get<Transform>(a);
        const Transform& tb = reg.get<Transform>(b);
        if (ta.Layer != tb.Layer) return ta.Layer < tb.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return kids;
}

// Корневые UI-сущности: без родителя ЛИБО родитель не UI-элемент (3D-сущность
// может «держать» интерфейс — он всё равно якорится к экрану).
//
// Порядок между корнями задаёт СЛОЙ ХОЛСТА (Canvas::SortOrder), а не только
// Layer: HUD должен быть под меню паузы, меню — под диалогом, и раскладывать
// это одним числом на элемент значило подбирать номера так, чтобы случайно не
// перекрыть чужую панель.
std::vector<entt::entity> SortedUIRoots(Scene& scene) {
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.Registry();
    for (auto e : reg.view<Transform>()) {
        entt::entity parent = scene.ParentOf(e);
        if (parent == entt::null || !IsElement(reg, parent)) roots.push_back(e);
    }
    auto canvasOrder = [&reg](entt::entity e) {
        const Canvas* c = reg.try_get<Canvas>(e);
        return c ? c->SortOrder : 0;
    };
    std::stable_sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
        const int ca = canvasOrder(a), cb = canvasOrder(b);
        if (ca != cb) return ca < cb;
        const Transform& ta = reg.get<Transform>(a);
        const Transform& tb = reg.get<Transform>(b);
        if (ta.Layer != tb.Layer) return ta.Layer < tb.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return roots;
}

// Шаг влево/вправо по строке UTF-8. Курсор живёт в БАЙТАХ (строка — байты), но
// двигаться обязан по СИМВОЛАМ: шаг в один байт разрежет кириллическую букву
// пополам, и в поле окажется невалидный UTF-8.
int PrevCharBoundary(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) --i;
    return i;
}
int NextCharBoundary(const std::string& s, int i) {
    const int n = (int)s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) ++i;
    return i;
}
int Utf8Length(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Что показывать вместо содержимого поля-пароля. Точки, а не звёздочки: в
// пиксельных шрифтах звёздочка часто выше строки и ломает базовую линию.
std::string MaskText(const std::string& text) {
    std::string out;
    out.reserve((size_t)Utf8Length(text) * 2);
    for (int i = 0; i < Utf8Length(text); ++i) out += "\u2022";
    return out;
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

// --- ЧТО РИСУЕМ: части элемента ---------------------------------------------
//
// Отрисовка складывает СЛОИ, а не выбирает ветку по «виду элемента». Это и есть
// разница между прежней моделью и нынешней: раньше кнопка, полоса и поле ввода
// были значениями перечисления, и «шкала с иконкой и подписью» не выражалась
// вовсе — вид один, а нужно три. Здесь каждая часть рисует своё поверх
// предыдущих, и любое их сочетание осмысленно само по себе.
struct ElementView {
    const Fill* FillPart = nullptr;
    const Label* LabelPart = nullptr;
    const Image* ImagePart = nullptr;
    const Bar* BarPart = nullptr;
    const Icon* IconPart = nullptr;
    const Interactable* Act = nullptr;
    const TextInput* InputPart = nullptr;
    const Range* RangePart = nullptr;
    // Накопленная прозрачность групп: умножается на КАЖДЫЙ цвет элемента.
    // Отдельным полем, а не правкой цветов на месте: части общие и константные,
    // копировать их ради одного множителя было бы расточительством на каждый
    // элемент каждого кадра.
    float Alpha = 1.0f;

    bool Hovered() const { return Act && Act->Runtime.Hovered; }
    bool Pressed() const { return Act && Act->Runtime.Pressed; }
    bool Focused() const { return Act && Act->Runtime.Focused; }
    bool Enabled() const { return !Act || Act->Enabled; }
};

ElementView ViewOf(const entt::registry& reg, entt::entity e, float alpha) {
    ElementView v;
    v.FillPart = reg.try_get<Fill>(e);
    v.LabelPart = reg.try_get<Label>(e);
    v.ImagePart = reg.try_get<Image>(e);
    v.BarPart = reg.try_get<Bar>(e);
    v.IconPart = reg.try_get<Icon>(e);
    v.Act = reg.try_get<Interactable>(e);
    v.InputPart = reg.try_get<TextInput>(e);
    v.RangePart = reg.try_get<Range>(e);
    v.Alpha = alpha;
    return v;
}

// Цвет «переднего плана»: заполнение ползунка, галочка в квадратике.
//
// Берётся у полосы, если она есть, — так у элемента остаётся ОДНО место, где
// задан его акцентный цвет. Значение по умолчанию совпадает с прежним цветом
// заполнения, поэтому перенесённые из старых сцен галки и ползунки выглядят
// как раньше (см. Decompose: он кладёт их цвет именно в полосу).
glm::vec4 AccentColor(const ElementView& v) {
    if (v.BarPart) return v.BarPart->FillColor;
    return {0.36f, 0.75f, 0.42f, 1.0f};
}

// Значение полосы с учётом сглаживания: показывается то, что доехало.
float BarShown(const Bar& bar) {
    return (bar.Smoothing > 0.0f && bar.Displayed >= 0.0f) ? bar.Displayed : bar.Value;
}

// Доля 0..1 у диапазона — то, чем ползунок и рисуется.
float RangeFraction(const Range& range) {
    const float span = range.Max - range.Min;
    if (std::fabs(span) < 1e-6f) return 0.0f;
    return glm::clamp((range.Value - range.Min) / span, 0.0f, 1.0f);
}

// Заливка прямоугольника подложкой: плоская или градиентная.
void FillRect(const Fill& fill, const UIRect& r, float rounding, glm::vec3 rgb, float alpha,
              UIRenderer& ui) {
    if (alpha <= 0.0f) return;
    if (fill.Gradient.a > 0.0f) {
        ui.GradientRect(r.x, r.y, r.w, r.h, rgb,
                        {fill.Gradient.r, fill.Gradient.g, fill.Gradient.b}, alpha,
                        fill.Gradient.a * (alpha / glm::max(fill.Color.a, 1e-4f)), rounding);
    } else {
        ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha, rounding);
    }
}

// Спрайт под текущее состояние. Набор интерфейса обычно рисует кнопку трижды —
// обычную, под курсором, нажатую, — и подменить картинку правильнее, чем
// осветлить основную: художник уже решил, как выглядит нажатие.
UIRenderer::Sprite StateSprite(const ElementView& v) {
    const Image& img = *v.ImagePart;
    const glm::vec4* src = &img.Sprite;
    if (v.Pressed() && img.SpritePressed.z > 0.0f) src = &img.SpritePressed;
    else if (v.Hovered() && img.SpriteHover.z > 0.0f) src = &img.SpriteHover;
    return {src->x, src->y, src->z, src->w};
}

// Подкраска под состояние — запасной путь для тех, у кого своих спрайтов
// состояний нет (сплошные панели, элементы из примитивов). Множители берутся у
// самого элемента: «на 12% ярче» подходит не всякому набору интерфейса.
glm::vec3 StateTint(const ElementView& v, glm::vec3 base) {
    if (!v.Act) return base;
    if (!v.Act->Enabled) return glm::mix(base, glm::vec3(0.5f), 0.5f);
    if (v.Pressed()) return base * v.Act->PressedBrightness;
    if (v.Hovered()) return glm::mix(base, glm::vec3(1.0f), v.Act->HoverBrightness - 1.0f);
    return base;
}

// Прозрачность части с учётом группы и выключенного состояния.
float AlphaOf(const ElementView& v, float channelAlpha) {
    float a = channelAlpha * v.Alpha;
    if (v.Act && !v.Act->Enabled) a *= v.Act->DisabledAlpha;
    return a;
}

// Картинка: спрайт с листа, девятина, пиксель-арт.
void DrawImage(const ElementView& v, const UIRect& r, glm::vec3 rgb, UIRenderer& ui) {
    const Image& img = *v.ImagePart;
    const float alpha = AlphaOf(v, img.Tint.a);
    if (!img.Tex) {
        // Текстура не задана/не загрузилась — заглушка тоном, чтобы элемент был
        // виден и настраиваем в редакторе.
        ui.RoundedRect(r.x, r.y, r.w, r.h, rgb, alpha, v.FillPart ? v.FillPart->Rounding : 0.0f);
        return;
    }
    const UIRenderer::Sprite src = StateSprite(v);
    const bool sliced = img.SliceBorder.x > 0.0f || img.SliceBorder.y > 0.0f ||
                        img.SliceBorder.z > 0.0f || img.SliceBorder.w > 0.0f;
    if (sliced) {
        // Масштаб пикселя: 0 — «подобрать сам». Для пиксель-арта он округляется
        // ВНИЗ до целого — дробный масштаб растягивает одни пиксели исходника на
        // два экранных, а соседние на один, и ровная рамка идёт волнами.
        float scale = img.PixelScale;
        if (scale <= 0.0f) {
            const float srcH = src.Whole() ? (float)img.Tex->Height() : src.H;
            scale = srcH > 0.0f ? r.h / srcH : 1.0f;
            if (img.PixelArt) scale = glm::max(1.0f, glm::floor(scale));
        }
        ui.ImageNineSlice(r.x, r.y, r.w, r.h, img.Tex.get(), src, img.SliceBorder, scale, rgb,
                          alpha);
        return;
    }
    // Спрайт БЕЗ девятины: у пиксель-арта его нельзя просто растянуть под
    // элемент. Растяжение 24x15 в 36x24 даёт разный масштаб по осям и дробный
    // по каждой — сердце выходит с рваными краями и обрезанным боком. Поэтому
    // берётся ЦЕЛЫЙ масштаб, влезающий в элемент, и спрайт ставится по центру;
    // элемент при этом может остаться больше картинки, и это честнее, чем её
    // испортить.
    UIRect dst = r;
    float scale = img.PixelScale;
    if (img.PixelArt || scale > 0.0f) {
        const float sw = src.Whole() ? (float)img.Tex->Width() : src.W;
        const float sh = src.Whole() ? (float)img.Tex->Height() : src.H;
        if (sw > 0.0f && sh > 0.0f) {
            if (scale <= 0.0f) {
                scale = glm::min(r.w / sw, r.h / sh);
                if (img.PixelArt) scale = glm::max(1.0f, glm::floor(scale));
            }
            dst.w = sw * scale;
            dst.h = sh * scale;
            dst.x = r.x + glm::floor((r.w - dst.w) * 0.5f);
            dst.y = r.y + glm::floor((r.h - dst.h) * 0.5f);
        }
    }
    ui.ImageSprite(dst.x, dst.y, dst.w, dst.h, img.Tex.get(), src, rgb, alpha);
}

// Галка: квадратная коробка у левого края. Квадратная и прижатая потому, что
// рядом обычно стоит подпись, и растягивать саму галку на всю ширину незачем.
UIRect ToggleBox(const UIRect& r) {
    const float side = glm::min(r.w, r.h);
    return {r.x, r.y + (r.h - side) * 0.5f, side, side};
}

void DrawToggle(const ElementView& v, const UIRect& r, glm::vec3 rgb, UIRenderer& ui) {
    const UIRect box = ToggleBox(r);
    const float rounding = v.FillPart ? glm::min(v.FillPart->Rounding, box.h * 0.5f) : 4.0f;
    if (v.FillPart) FillRect(*v.FillPart, box, rounding, rgb, AlphaOf(v, v.FillPart->Color.a), ui);
    if (v.FillPart && v.FillPart->BorderThickness > 0.0f && v.FillPart->BorderColor.a > 0.0f) {
        const glm::vec4& b = v.FillPart->BorderColor;
        ui.RoundedRectOutline(box.x, box.y, box.w, box.h, rounding, v.FillPart->BorderThickness,
                              {b.r, b.g, b.b}, AlphaOf(v, b.a));
    }
    // Включена — если значение ближе к верхнему концу диапазона.
    const Range& range = *v.RangePart;
    if (range.Value >= (range.Min + range.Max) * 0.5f) {
        const float pad = box.h * 0.18f;
        const glm::vec4 accent = AccentColor(v);
        DrawIcon(ui, "check", box.x + pad, box.y + pad, box.h - pad * 2.0f,
                 StateTint(v, {accent.r, accent.g, accent.b}), AlphaOf(v, accent.a));
    }
}

// Ползунок: дорожка тонкая и по центру, ручка — во всю высоту. Попасть в тонкую
// полоску мышью трудно, а ловит нажатие весь элемент.
void DrawSlider(const ElementView& v, const UIRect& r, glm::vec3 rgb, UIRenderer& ui) {
    const float track = glm::max(r.h * 0.28f, 4.0f);
    const UIRect bar{r.x, r.y + (r.h - track) * 0.5f, r.w, track};
    if (v.FillPart) {
        FillRect(*v.FillPart, bar, track * 0.5f, rgb, AlphaOf(v, v.FillPart->Color.a), ui);
    }
    const float t = RangeFraction(*v.RangePart);
    const glm::vec4 accent = AccentColor(v);
    const glm::vec3 fill = StateTint(v, {accent.r, accent.g, accent.b});
    const float fillA = AlphaOf(v, accent.a);
    if (t > 0.0f && fillA > 0.0f) {
        ui.RoundedRect(bar.x, bar.y, bar.w * t, bar.h, fill, fillA, track * 0.5f);
    }
    const float knob = r.h * 0.5f;
    const float kx = r.x + (r.w - knob * 2.0f) * t + knob;
    ui.Circle(kx, r.y + r.h * 0.5f, knob, fill, AlphaOf(v, 1.0f));
    if (v.FillPart && v.FillPart->BorderThickness > 0.0f && v.FillPart->BorderColor.a > 0.0f) {
        const glm::vec4& b = v.FillPart->BorderColor;
        ui.Ring(kx, r.y + r.h * 0.5f, knob, v.FillPart->BorderThickness, {b.r, b.g, b.b},
                AlphaOf(v, b.a));
    }
}

// Полоса: заполнение растёт в свою сторону. Направление — не украшение:
// вертикальная шкала (мана сбоку, столбик громкости) без него невозможна.
void DrawBar(const ElementView& v, const UIRect& r, UIRenderer& ui) {
    const Bar& bar = *v.BarPart;
    const float t = glm::clamp(BarShown(bar), 0.0f, 1.0f);
    const float alpha = AlphaOf(v, bar.FillColor.a);
    if (t <= 0.0f || alpha <= 0.0f) return;

    // Заполнение с небольшим внутренним отступом, радиус — согласованный.
    const float pad = glm::min(2.0f, glm::min(r.w, r.h) * 0.15f);
    const float rounding = v.FillPart ? glm::max(v.FillPart->Rounding - pad, 0.0f) : 0.0f;
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
    ui.GradientRect(fillRect.x, fillRect.y, fillRect.w, fillRect.h,
                    glm::mix(fill, glm::vec3(1.0f), 0.22f), fill * 0.78f, alpha, alpha, rounding);
}

// Смещение строки по горизонтали внутри отведённой полосы.
float AlignX(Label::Align align, float left, float avail, float textWidth) {
    switch (align) {
        case Label::Align::Center: return left + (avail - textWidth) * 0.5f;
        case Label::Align::End: return left + avail - textWidth;
        default: return left;
    }
}

float AlignY(Label::Align align, const UIRect& r, float blockH) {
    switch (align) {
        case Label::Align::Start: return r.y;
        case Label::Align::End: return r.y + r.h - blockH;
        default: return r.y + (r.h - blockH) * 0.5f;
    }
}

// Что показывать в поле ввода: содержимое, подсказку или точки пароля.
std::string InputShownText(const ElementView& v, bool& isPlaceholder) {
    const std::string& text = v.LabelPart ? v.LabelPart->Text : std::string();
    isPlaceholder = text.empty();
    if (isPlaceholder) return v.InputPart->Placeholder;
    return v.InputPart->Password ? MaskText(text) : text;
}

void DrawElement(const ElementView& v, const UIRect& r, UIRenderer& ui) {
    // --- Подложка -----------------------------------------------------------
    const glm::vec4 fillColor = v.FillPart ? v.FillPart->Color : glm::vec4(0.0f);
    const glm::vec3 baseRgb =
        v.ImagePart ? glm::vec3(v.ImagePart->Tint) : glm::vec3(fillColor);
    const glm::vec3 tinted = StateTint(v, baseRgb);
    const float rounding = v.FillPart ? v.FillPart->Rounding : 0.0f;

    // Тень — под всем остальным, поэтому первой.
    if (v.FillPart && v.FillPart->ShadowSize > 0.0f) {
        ui.RectShadow(r.x, r.y, r.w, r.h, rounding, v.FillPart->ShadowSize);
    }

    // ЧТО ИМЕННО занимает фон элемента, решается по его частям — и это
    // единственное место, где такой выбор вообще делается.
    const bool toggle = v.RangePart && v.RangePart->Toggle;
    const bool slider = v.RangePart && !v.RangePart->Toggle;
    if (toggle) {
        DrawToggle(v, r, tinted, ui);
    } else if (slider) {
        DrawSlider(v, r, tinted, ui);
    } else if (v.ImagePart) {
        DrawImage(v, r, tinted, ui);
    } else if (v.FillPart) {
        FillRect(*v.FillPart, r, rounding, tinted, AlphaOf(v, fillColor.a), ui);
    }

    // Полоса рисуется ПОВЕРХ подложки — Fill + Bar это и значит. У диапазона
    // своя отрисовка значения (дорожка, галочка), и вторая шкала поверх неё
    // была бы тем же числом, нарисованным дважды: у ползунка полоса участвует
    // только цветом (см. AccentColor).
    if (v.BarPart && !v.RangePart) DrawBar(v, r, ui);

    // Рамка — по всему элементу; у галки она уже нарисована вокруг квадратика.
    if (!toggle && v.FillPart && v.FillPart->BorderThickness > 0.0f &&
        v.FillPart->BorderColor.a > 0.0f) {
        const glm::vec4& b = v.FillPart->BorderColor;
        ui.RoundedRectOutline(r.x, r.y, r.w, r.h, rounding, v.FillPart->BorderThickness,
                              {b.r, b.g, b.b}, AlphaOf(v, b.a));
    }

    // --- Значок -------------------------------------------------------------
    //
    // Один в элементе — занимает его целиком; рядом с чем-то ещё — квадрат у
    // левого края, и текст начинается за ним. Так «значок + подпись + шкала»
    // остаётся ОДНИМ элементом, а не тремя сущностями, которые надо держать
    // выровненными вручную.
    const float padX = v.LabelPart ? v.LabelPart->PadX : 8.0f;
    float textLeft = r.x + padX;
    if (v.IconPart && !v.IconPart->Name.empty() && v.IconPart->Color.a > 0.0f) {
        const glm::vec4& c = v.IconPart->Color;
        const glm::vec3 rgb{c.r, c.g, c.b};
        const float alpha = AlphaOf(v, c.a);
        const bool alone = !v.LabelPart && !v.ImagePart && !v.BarPart && !v.RangePart;
        if (alone) {
            const float side = v.IconPart->Size > 0.0f ? v.IconPart->Size : glm::min(r.w, r.h);
            DrawIcon(ui, v.IconPart->Name, r.x + (r.w - side) * 0.5f, r.y + (r.h - side) * 0.5f,
                     side, rgb, alpha);
        } else {
            const float pad = glm::min(4.0f, r.h * 0.18f);
            const float side =
                v.IconPart->Size > 0.0f ? v.IconPart->Size : glm::max(r.h - pad * 2.0f, 4.0f);
            DrawIcon(ui, v.IconPart->Name, r.x + pad, r.y + pad, side, rgb, alpha);
            textLeft = r.x + pad * 2.0f + side;
        }
    }
    // У галки текст начинается за квадратиком — иначе подпись легла бы на него.
    if (toggle) textLeft = glm::max(textLeft, ToggleBox(r).x + ToggleBox(r).w + padX);

    if (!v.LabelPart) return;
    const Label& label = *v.LabelPart;

    // --- Поле ввода ---------------------------------------------------------
    if (v.InputPart) {
        bool placeholder = false;
        const std::string draw = InputShownText(v, placeholder);
        const glm::vec3 rgb{label.Color.r, label.Color.g, label.Color.b};
        const float textY = r.y + (r.h - ui.TextHeight(label.Scale)) * 0.5f;
        if (!draw.empty()) {
            // Подсказка бледнее содержимого — иначе пустое поле выглядит
            // заполненным, и человек стирает то, чего не вводил.
            ui.Text(textLeft, textY, label.Scale, rgb, draw,
                    AlphaOf(v, label.Color.a) * (placeholder ? 0.45f : 1.0f));
        }
        // Курсор мигает только в фокусе и только когда поле включено.
        if (v.Focused() && v.Enabled()) {
            const int caret = glm::clamp(v.Act->Runtime.Caret, 0, (int)label.Text.size());
            const std::string head = label.Text.substr(0, (size_t)caret);
            const std::string before = v.InputPart->Password ? MaskText(head) : head;
            const float cx = textLeft + ui.MeasureText(before, label.Scale);
            // Полсекунды виден, полсекунды нет — привычный ритм; после каждой
            // правки счётчик сбрасывается, чтобы курсор не пропал ровно тогда,
            // когда на него смотрят.
            if (std::fmod(v.Act->Runtime.CaretBlink, 1.0f) < 0.5f) {
                const float ch = ui.TextHeight(label.Scale) * 1.15f;
                ui.Rect(cx, r.y + (r.h - ch) * 0.5f, glm::max(label.Scale, 1.0f), ch, rgb,
                        AlphaOf(v, label.Color.a));
            }
        }
        return; // общий путь текста ниже полю ввода не нужен
    }

    // --- Надпись ------------------------------------------------------------
    if (label.Text.empty() || label.Color.a <= 0.0f) return;
    const glm::vec3 textRgb{label.Color.r, label.Color.g, label.Color.b};
    const float textA = AlphaOf(v, label.Color.a);
    const float right = r.x + r.w - padX;
    const float avail = glm::max(right - textLeft, 1.0f);

    std::vector<std::string> lines;
    if (label.Wrap) {
        lines = WrapLines(label.Text, avail, label.Scale, ui);
    } else if (label.Text.find('\n') != std::string::npos) {
        lines = WrapLines(label.Text, 0.0f, label.Scale, ui); // только по явным \n
    } else {
        lines.push_back(label.Text);
    }

    // Однострочный текст центрируется по высоте элемента — иначе подпись рядом
    // с иконкой висит выше неё, и строка «иконка + текст» выглядит
    // развалившейся. Многострочный блок выравнивается целиком.
    const float lineH = ui.LineHeight(label.Scale);
    const float blockH = lineH * (float)(lines.size() - 1) + ui.TextHeight(label.Scale);
    float textY = AlignY(label.Vertical, r, blockH);

    // Перенесённый текст ОБРЕЗАЕТСЯ своим элементом. Он переносился под эту
    // ширину — значит, и по высоте обязан остаться внутри: абзац, вылезший из
    // панели на фон, выглядит хуже, чем обрезанный по её краю, и главное —
    // сразу виден как ошибка вёрстки, а не как «так и было задумано».
    const bool clipText = label.Wrap && blockH > r.h;
    if (clipText) ui.PushClipRect(r.x, r.y, r.w, r.h);
    for (const std::string& line : lines) {
        if (!line.empty()) {
            const float x = AlignX(label.Horizontal, textLeft, avail,
                                   ui.MeasureText(line, label.Scale));
            ui.Text(x, textY, label.Scale, textRgb, line, textA);
        }
        textY += lineH;
    }
    if (clipText) ui.PopClipRect();
}

// --- Один решатель на три задачи ------------------------------------------
//
// Раньше сцену обходили ТРИЖДЫ и каждый раз заново считали прямоугольники: для
// отрисовки, для попадания курсором и для ввода. Обходы жили в разных функциях
// и уже расходились — отрисовка учитывала измеренную ширину текста, а HitTest
// брал заданную, и по кнопке с авто-шириной приходилось попадать не туда, где
// она нарисована. Теперь раскладка считается ОДИН раз за кадр, а рисование,
// попадание и ввод читают её результат.
struct Solved {
    entt::entity Entity;
    // Части элемента, собранные ОДИН раз за проход. Хранятся здесь, а не
    // выбираются заново у каждого потребителя: отрисовка, попадание курсором и
    // ввод смотрят на одно и то же, и собрать это трижды значило бы снова
    // завести три расходящихся взгляда на один элемент.
    ElementView View;
    UIRect Rect;
    UIRect Clip;      // окно обрезки (нулевая ширина/высота — не обрезан)
    bool Clipped = false;
    float Alpha = 1.0f;      // накопленная прозрачность групп
    bool Interactive = true; // группа может запретить ввод всему поддереву
};

// Рекурсивный обход: считает прямоугольники, применяет раскладку, маски и
// групповые свойства. forced — прямоугольник, назначенный раскладкой родителя
// (nullptr — элемент стоит по своему якорю).
void SolveSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer* ui,
                  bool clipped, const UIRect& clip, float alpha, bool interactive,
                  const UIRect* forced, std::vector<Solved>& out) {
    entt::registry& reg = scene.Registry();
    const Transform& t = reg.get<Transform>(ent);
    if (!t.Visible) return; // невидимый прячет и всё поддерево

    // ГЕОМЕТРИЯ БЕРЁТСЯ ИЗ Transform, а не из плоского описания.
    //
    // Плоское описание — это то, чем элемент РИСУЕТСЯ, и растяжения, полей и
    // точки привязки в нём нет: у прежнего компонента их не было вовсе.
    // Считать по нему раскладку значило бы, что панель «во всю ширину экрана»
    // молча остаётся размером 200x56 — то есть самая заметная возможность
    // новой системы не работает, и понять почему неоткуда.
    glm::vec2 size = ResolveSize(t, parentRect);
    // Ширина по содержимому — единственное, что знает шрифт, а не раскладка.
    if (ui) size = MeasuredWidth(reg, ent, size, *ui);
    UIRect r = forced ? *forced : Resolve(t, parentRect, size);
    if (forced) size = {forced->w, forced->h};
    // Фактический размер запоминается в САМОМ элементе: его читают попадание
    // курсором и следующий кадр, когда шрифта под рукой может не оказаться.
    reg.get<Transform>(ent).LayoutSize = size;

    // Групповые свойства накапливаются вниз по дереву: спрятать панель — это
    // одно число на ней, а не проход скриптом по каждому её ребёнку.
    float myAlpha = alpha;
    bool myInteractive = interactive;
    if (const Group* g = reg.try_get<Group>(ent)) {
        myAlpha *= glm::clamp(g->Alpha, 0.0f, 1.0f);
        if (!g->Interactable || !g->BlockRaycasts) myInteractive = false;
    }

    out.push_back(Solved{ent, ViewOf(reg, ent, myAlpha), r, clip, clipped, myAlpha,
                         myInteractive});

    std::vector<entt::entity> kids = SortedUIChildren(scene, ent);
    if (kids.empty()) return;

    // Маска: окно обрезки пересекается с родительским — вложенные маски режут
    // друг друга (список внутри окна виден только на их пересечении).
    bool childClipped = clipped;
    UIRect childClip = clip;
    const Mask* mask = reg.try_get<Mask>(ent);
    if (mask) {
        const UIRect window = MaskWindow(*mask, r);
        if (!mask->ShowOutside) {
            childClip = childClipped ? Intersect(childClip, window) : window;
            childClipped = true;
        }
    }

    // Раскладка: контейнер сам расставляет детей. Их якоря при этом не
    // работают — в том и смысл, что позиции считает родитель.
    if (const Layout* layout = reg.try_get<Layout>(ent)) {
        std::vector<LayoutSlot> slots(kids.size());
        for (size_t i = 0; i < kids.size(); ++i) {
            const Transform& kt = reg.get<Transform>(kids[i]);
            slots[i].Size = ResolveSize(kt, r);
            if (ui) slots[i].Size = MeasuredWidth(reg, kids[i], slots[i].Size, *ui);
        }
        ApplyLayout(*layout, r, slots);
        for (size_t i = 0; i < kids.size(); ++i) {
            const UIRect kr{slots[i].Pos.x, slots[i].Pos.y, slots[i].Size.x, slots[i].Size.y};
            SolveSubtree(scene, kids[i], r, ui, childClipped, childClip, myAlpha, myInteractive,
                         &kr, out);
        }
        return;
    }

    for (auto k : kids) {
        SolveSubtree(scene, k, r, ui, childClipped, childClip, myAlpha, myInteractive, nullptr,
                     out);
    }
}

// Все элементы сцены в ПОРЯДКЕ ОТРИСОВКИ. ui нужен для измерения текста; без
// него берётся размер, посчитанный на прошлом кадре.
std::vector<Solved> SolveScene(Scene& scene, UIRenderer* ui, int screenW, int screenH) {
    std::vector<Solved> out;
    entt::registry& reg = scene.Registry();
    for (auto root : SortedUIRoots(scene)) {
        // Холст задаёт масштаб интерфейса: свёрстанное под 1920x1080 не должно
        // сжиматься вчетверо на 4K.
        UIRect screen{0.0f, 0.0f, (float)screenW, (float)screenH};
        if (const Canvas* c = reg.try_get<Canvas>(root)) {
            const float scale = CanvasScale(*c, {(float)screenW, (float)screenH});
            if (scale > 0.0f && scale != 1.0f) {
                screen.w = (float)screenW / scale;
                screen.h = (float)screenH / scale;
            }
        }
        SolveSubtree(scene, root, screen, ui, false, UIRect{}, 1.0f, true, nullptr, out);
    }
    return out;
}

bool PointIn(const UIRect& r, glm::vec2 p) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

} // namespace

void DrawSceneUI(Scene& scene, UIRenderer& ui, int screenW, int screenH) {
    SAGE_PROFILE("Интерфейс сцены");
    const std::vector<Solved> items = SolveScene(scene, &ui, screenW, screenH);

    for (const Solved& it : items) {
        if (it.Clipped) {
            if (it.Clip.w <= 0.0f || it.Clip.h <= 0.0f) continue; // полностью обрезан
            ui.PushClipRect(it.Clip.x, it.Clip.y, it.Clip.w, it.Clip.h);
        }
        // Прозрачность группы едет внутри вида и множится на КАЖДЫЙ цвет: панель
        // с полупрозрачным фоном не должна становиться непрозрачной от того, что
        // группу показали наполовину.
        DrawElement(it.View, it.Rect, ui);
        if (it.Clipped) ui.PopClipRect();
    }
}

int HitTest(Scene& scene, float x, float y, int screenW, int screenH) {
    const std::vector<Solved> items = SolveScene(scene, nullptr, screenW, screenH);
    const entt::registry& reg = scene.Registry();
    int bestId = -1;
    for (const Solved& it : items) {
        if (it.Clipped && !PointIn(it.Clip, {x, y})) continue;
        if (PointIn(it.Rect, {x, y})) bestId = reg.get<IdComponent>(it.Entity).Id;
    }
    return bestId;
}

UIInputResult UpdateSceneUI(Scene& scene, const UIInputState& input, int screenW, int screenH) {
    UIInputResult result;
    entt::registry& reg = scene.Registry();
    const std::vector<Solved> items = SolveScene(scene, nullptr, screenW, screenH);

    // Состояние взаимодействия живёт в Interactable::Runtime, и его НЕТ у
    // элементов, которые мышь не ловят. Это не мелочь: раньше поля Hovered,
    // Pressed и Caret были у каждой надписи и каждой рамки, и «под курсором» у
    // подписи означало ровно ничего — но проверить это было нельзя, потому что
    // поле есть у всех.
    auto stateOf = [&reg](entt::entity e) -> State* {
        Interactable* act = reg.try_get<Interactable>(e);
        return act ? &act->Runtime : nullptr;
    };
    auto usable = [&reg](entt::entity e) {
        const Interactable* act = reg.try_get<Interactable>(e);
        return act && act->Enabled;
    };
    // Текст поля ввода — это надпись элемента: у поля без надписи набирать
    // некуда, и заводить ей отдельное хранилище значило бы держать две строки,
    // из которых видна одна.
    auto textOf = [&reg](entt::entity e) -> Label* { return reg.try_get<Label>(e); };

    // Кто под курсором: последний нарисованный из тех, кто ловит мышь и не
    // обрезан своей маской.
    entt::entity hovered = entt::null;
    for (const Solved& it : items) {
        if (!it.Interactive || !usable(it.Entity)) continue;
        if (it.Clipped && !PointIn(it.Clip, input.Mouse)) continue;
        if (PointIn(it.Rect, input.Mouse)) hovered = it.Entity;
    }

    // Флаги «за этот кадр» гасим у всех: их читает игра сразу после нас, и
    // оставшийся с прошлого кадра Clicked сработал бы второй раз.
    for (const Solved& it : items) {
        if (State* st = stateOf(it.Entity)) {
            st->Clicked = false;
            st->Changed = false;
            st->Hovered = (it.Entity == hovered);
            // Pressed здесь НЕ сбрасываем: в кадре отпускания кнопка уже не
            // удерживается, и сброс до разбора отпускания съел бы сам щелчок.
        }
    }

    // Нажатие: назначает фокус (полю ввода) и «прижимает» элемент.
    if (input.MousePressed) {
        for (const Solved& it : items) {
            State* st = stateOf(it.Entity);
            if (!st) continue;
            const bool hit = (it.Entity == hovered);
            if (st->Focused && !hit) st->Focused = false; // клик мимо снимает фокус
            if (!hit) continue;
            st->Pressed = true;
            if (reg.all_of<TextInput>(it.Entity)) {
                st->Focused = true;
                const Label* lbl = textOf(it.Entity);
                st->Caret = lbl ? (int)lbl->Text.size() : 0;
                st->CaretBlink = 0.0f;
            }
        }
    }

    // Отпускание НАД тем же элементом — это и есть щелчок. Отпускание в стороне
    // щелчком не считается: увести палец с кнопки — общепринятый способ
    // передумать, и ломать его нельзя.
    if (input.MouseReleased && hovered != entt::null) {
        if (State* st = stateOf(hovered)) {
            if (st->Pressed) {
                st->Clicked = true;
                result.ClickedId = reg.get<IdComponent>(hovered).Id;
                result.ClickedAction = reg.get<Interactable>(hovered).Action;
                // Галка — тот же диапазон, у которого два конца: щелчок
                // перекидывает значение между ними.
                if (Range* range = reg.try_get<Range>(hovered); range && range->Toggle) {
                    const float mid = (range->Min + range->Max) * 0.5f;
                    range->Value = range->Value >= mid ? range->Min : range->Max;
                    st->Changed = true;
                }
            }
        }
    }

    // Ползунок: тянется, пока кнопка удерживается, даже если курсор ушёл за
    // пределы дорожки — иначе значение срывается от малейшего движения вбок.
    for (const Solved& it : items) {
        Range* range = reg.try_get<Range>(it.Entity);
        State* st = stateOf(it.Entity);
        if (!range || range->Toggle || !st || !usable(it.Entity)) continue;
        if (!st->Pressed || !input.MouseDown) continue;
        const float w = glm::max(it.Rect.w, 1.0f);
        const float t = glm::clamp((input.Mouse.x - it.Rect.x) / w, 0.0f, 1.0f);
        float v = range->Min + t * (range->Max - range->Min);
        // Шаг: громкость по 5% должна прилипать к пятёркам, иначе ползунок
        // выдаёт 0.4732 там, где человек ждёт 0.45.
        if (range->Step > 0.0f) v = range->Min + std::round((v - range->Min) / range->Step) * range->Step;
        v = glm::clamp(v, glm::min(range->Min, range->Max), glm::max(range->Min, range->Max));
        if (v != range->Value) { range->Value = v; st->Changed = true; }
        result.WantsMouse = true;
    }

    // Ввод текста — только в поле с фокусом.
    for (const Solved& it : items) {
        const TextInput* field = reg.try_get<TextInput>(it.Entity);
        State* st = stateOf(it.Entity);
        Label* lbl = textOf(it.Entity);
        if (!field || !st || !lbl || !st->Focused || !usable(it.Entity)) continue;
        result.WantsKeyboard = true;
        st->CaretBlink += input.DeltaTime;
        st->Caret = glm::clamp(st->Caret, 0, (int)lbl->Text.size());
        if (field->ReadOnly) continue;   // показывать можно, править нельзя

        if (!input.TypedText.empty()) {
            const bool room = field->MaxLength <= 0 ||
                              Utf8Length(lbl->Text) + Utf8Length(input.TypedText) <= field->MaxLength;
            if (room) {
                lbl->Text.insert((size_t)st->Caret, input.TypedText);
                st->Caret += (int)input.TypedText.size();
                st->Changed = true;
                st->CaretBlink = 0.0f;
            }
        }
        if (input.Backspace && st->Caret > 0) {
            const int prev = PrevCharBoundary(lbl->Text, st->Caret);
            lbl->Text.erase((size_t)prev, (size_t)(st->Caret - prev));
            st->Caret = prev;
            st->Changed = true;
            st->CaretBlink = 0.0f;
        }
        if (input.Delete && st->Caret < (int)lbl->Text.size()) {
            const int next = NextCharBoundary(lbl->Text, st->Caret);
            lbl->Text.erase((size_t)st->Caret, (size_t)(next - st->Caret));
            st->Changed = true;
            st->CaretBlink = 0.0f;
        }
        if (input.Left) { st->Caret = PrevCharBoundary(lbl->Text, st->Caret); st->CaretBlink = 0.0f; }
        if (input.Right) { st->Caret = NextCharBoundary(lbl->Text, st->Caret); st->CaretBlink = 0.0f; }
        if (input.Home) { st->Caret = 0; st->CaretBlink = 0.0f; }
        if (input.End) { st->Caret = (int)lbl->Text.size(); st->CaretBlink = 0.0f; }
        if (input.Enter || input.Escape) st->Focused = false;
    }

    // Сглаживание полос: значение едет к цели, а не прыгает.
    for (const Solved& it : items) {
        if (Bar* bar = reg.try_get<Bar>(it.Entity)) {
            if (bar->Smoothing <= 0.0f) { bar->Displayed = bar->Value; continue; }
            if (bar->Displayed < 0.0f) bar->Displayed = bar->Value;
            const float step = bar->Smoothing * input.DeltaTime;
            const float diff = bar->Value - bar->Displayed;
            bar->Displayed += glm::clamp(diff, -step, step);
        }
    }

    // Кнопка отпущена — гасим «прижатие» у всех. ПОСЛЕ разбора отпускания:
    // до него Pressed ещё нужен, чтобы отличить щелчок от «отпустил в стороне».
    if (!input.MouseDown) {
        for (const Solved& it : items) {
            if (State* st = stateOf(it.Entity)) st->Pressed = false;
        }
    }

    if (hovered != entt::null) result.WantsMouse = true;
    return result;
}

} // namespace sage::ui
