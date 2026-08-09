#include "UISceneSystem.h"

#include "sage/ui/UI.h"
#include "sage/ui/UIBridge.h"
#include "sage/core/Profiler.h"
#include "UIRenderer.h"
#include "UIIcons.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace sage::ui {

UIRect ResolveElementRect(const UIElementComponent& e, const UIRect& parent, glm::vec2 size) {
    glm::vec2 pos = ResolveAnchored(e.Anchor, e.Offset, size, parent);
    return {pos.x, pos.y, size.x, size.y};
}

UIRect ResolveElementRect(const UIElementComponent& e, const UIRect& parent) {
    // Посчитанный размер важнее объявленного: у AutoWidth-элемента Size.x —
    // лишь запасное значение до первого кадра.
    glm::vec2 size = e.LayoutSize.x > 0.0f ? e.LayoutSize : e.Size;
    return ResolveElementRect(e, parent, size);
}

namespace {

// Ширина элемента по содержимому: отступ, иконка, отступ, текст, отступ.
// Высота не подгоняется — её задаёт вёрстка (строка интерфейса одна на всех).
glm::vec2 MeasuredSize(const UIElementComponent& e, UIRenderer& ui) {
    if (!e.AutoWidth) return e.Size;
    bool hasIcon = !e.Icon.empty() && e.IconColor.a > 0.0f;
    // Иконка занимает квадрат в высоту элемента — текст начинается за ним
    // (ровно там же, где его кладёт DrawElement).
    float w = hasIcon ? e.Size.y : e.PadX;
    if (!e.Text.empty() && e.TextColor.a > 0.0f) w += ui.MeasureText(e.Text, e.TextScale);
    return {glm::max(w + e.PadX, e.Size.y), e.Size.y};
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

// Заливка элемента: плоская или градиентная. Вынесено, потому что одинаково
// нужно и панели, и полосе, и заглушке картинки.
void FillRect(const UIElementComponent& e, const UIRect& r, UIRenderer& ui) {
    if (e.Color.a <= 0.0f) return;
    glm::vec3 top{e.Color.r, e.Color.g, e.Color.b};
    if (e.GradientColor.a > 0.0f) {
        ui.GradientRect(r.x, r.y, r.w, r.h, top,
                        {e.GradientColor.r, e.GradientColor.g, e.GradientColor.b},
                        e.Color.a, e.GradientColor.a, e.Rounding);
    } else {
        ui.RoundedRect(r.x, r.y, r.w, r.h, top, e.Color.a, e.Rounding);
    }
}

// Спрайт под текущее состояние. Набор интерфейса обычно рисует кнопку трижды —
// обычную, под курсором, нажатую, — и подменить картинку правильнее, чем
// осветлить основную: художник уже решил, как выглядит нажатие.
UIRenderer::Sprite StateSprite(const UIElementComponent& e) {
    const glm::vec4* src = &e.Sprite;
    if (e.Pressed && e.SpritePressed.z > 0.0f) src = &e.SpritePressed;
    else if (e.Hovered && e.SpriteHover.z > 0.0f) src = &e.SpriteHover;
    return {src->x, src->y, src->z, src->w};
}

// Подкраска под состояние — запасной путь для тех, у кого своих спрайтов
// состояний нет (сплошные панели, элементы из примитивов).
glm::vec3 StateTint(const UIElementComponent& e, glm::vec3 base) {
    if (!e.Enabled) return glm::mix(base, glm::vec3(0.5f), 0.5f);
    if (!e.Interactive) return base;
    if (e.Pressed) return base * 0.82f;
    if (e.Hovered) return glm::mix(base, glm::vec3(1.0f), 0.16f);
    return base;
}

void DrawElement(const UIElementComponent& e, const UIRect& r, UIRenderer& ui) {
    using Kind = UIElementComponent::Kind;
    glm::vec3 fillRgb = StateTint(e, {e.Color.r, e.Color.g, e.Color.b});

    // Тень — под всем остальным, поэтому первой.
    if (e.ShadowSize > 0.0f) {
        ui.RectShadow(r.x, r.y, r.w, r.h, e.Rounding, e.ShadowSize);
    }

    switch (e.Type) {
        case Kind::Panel:
            FillRect(e, r, ui);
            break;
        case Kind::Label:
            break; // только текст (ниже)
        case Kind::Icon:
            break; // только иконка (ниже) — подложки у неё нет
        case Kind::Image:
            if (e.Tex) {
                UIRenderer::Sprite src = StateSprite(e);
                const bool sliced = e.SliceBorder.x > 0.0f || e.SliceBorder.y > 0.0f ||
                                    e.SliceBorder.z > 0.0f || e.SliceBorder.w > 0.0f;
                if (sliced) {
                    // Масштаб пикселя: 0 — «подобрать сам». Для пиксель-арта он
                    // округляется ВНИЗ до целого — дробный масштаб растягивает
                    // одни пиксели исходника на два экранных, а соседние на
                    // один, и ровная рамка идёт волнами.
                    float scale = e.PixelScale;
                    if (scale <= 0.0f) {
                        const float srcH = src.Whole() ? (float)e.Tex->Height() : src.H;
                        scale = srcH > 0.0f ? r.h / srcH : 1.0f;
                        if (e.PixelArt) scale = glm::max(1.0f, glm::floor(scale));
                    }
                    ui.ImageNineSlice(r.x, r.y, r.w, r.h, e.Tex.get(), src, e.SliceBorder, scale,
                                      fillRgb, e.Color.a);
                } else {
                    // Спрайт БЕЗ девятины: у пиксель-арта его нельзя просто
                    // растянуть под элемент. Растяжение 24x15 в 36x24 даёт
                    // разный масштаб по осям и дробный по каждой — сердце
                    // выходит с рваными краями и обрезанным боком. Поэтому
                    // берётся ЦЕЛЫЙ масштаб, влезающий в элемент, и спрайт
                    // ставится по центру; элемент при этом может остаться
                    // больше картинки, и это честнее, чем её испортить.
                    UIRect dst = r;
                    float scale = e.PixelScale;
                    if (e.PixelArt || scale > 0.0f) {
                        const float sw = src.Whole() ? (float)e.Tex->Width() : src.W;
                        const float sh = src.Whole() ? (float)e.Tex->Height() : src.H;
                        if (sw > 0.0f && sh > 0.0f) {
                            if (scale <= 0.0f) {
                                scale = glm::min(r.w / sw, r.h / sh);
                                if (e.PixelArt) scale = glm::max(1.0f, glm::floor(scale));
                            }
                            dst.w = sw * scale;
                            dst.h = sh * scale;
                            dst.x = r.x + glm::floor((r.w - dst.w) * 0.5f);
                            dst.y = r.y + glm::floor((r.h - dst.h) * 0.5f);
                        }
                    }
                    ui.ImageSprite(dst.x, dst.y, dst.w, dst.h, e.Tex.get(), src, fillRgb,
                                   e.Color.a);
                }
            } else {
                // Текстура не задана/не загрузилась — заглушка цветом, чтобы
                // элемент был виден и настраиваем в редакторе.
                FillRect(e, r, ui);
            }
            break;
        case Kind::Input: {
            FillRect(e, r, ui);
            break; // текст и курсор — ниже, вместе с общим текстом
        }
        case Kind::Checkbox: {
            // Коробка квадратная и прижата к левому краю: рядом обычно стоит
            // подпись, и растягивать саму галку на всю ширину элемента незачем.
            const float side = glm::min(r.w, r.h);
            const UIRect box{r.x, r.y + (r.h - side) * 0.5f, side, side};
            UIElementComponent boxStyle = e;
            boxStyle.Rounding = glm::min(e.Rounding, side * 0.5f);
            FillRect(boxStyle, box, ui);
            if (e.BorderThickness > 0.0f && e.BorderColor.a > 0.0f) {
                ui.RoundedRectOutline(box.x, box.y, box.w, box.h, boxStyle.Rounding,
                                      e.BorderThickness,
                                      {e.BorderColor.r, e.BorderColor.g, e.BorderColor.b},
                                      e.BorderColor.a);
            }
            if (e.Value >= 0.5f) {
                const float pad = side * 0.18f;
                glm::vec3 mark{e.BarFillColor.r, e.BarFillColor.g, e.BarFillColor.b};
                DrawIcon(ui, "check", box.x + pad, box.y + pad, side - pad * 2.0f,
                         StateTint(e, mark), e.BarFillColor.a);
            }
            break;
        }
        case Kind::Slider: {
            // Дорожка тонкая и по центру, ручка — во всю высоту: попасть в
            // тонкую полоску мышью трудно, а ловит нажатие весь элемент.
            const float track = glm::max(r.h * 0.28f, 4.0f);
            const UIRect bar{r.x, r.y + (r.h - track) * 0.5f, r.w, track};
            UIElementComponent trackStyle = e;
            trackStyle.Rounding = track * 0.5f;
            FillRect(trackStyle, bar, ui);
            const float v = glm::clamp(e.Value, 0.0f, 1.0f);
            glm::vec3 fill = StateTint(e, {e.BarFillColor.r, e.BarFillColor.g, e.BarFillColor.b});
            if (v > 0.0f && e.BarFillColor.a > 0.0f) {
                ui.RoundedRect(bar.x, bar.y, bar.w * v, bar.h, fill, e.BarFillColor.a,
                               track * 0.5f);
            }
            const float knob = r.h * 0.5f;
            const float kx = r.x + (r.w - knob * 2.0f) * v + knob;
            ui.Circle(kx, r.y + r.h * 0.5f, knob, fill, 1.0f);
            if (e.BorderColor.a > 0.0f && e.BorderThickness > 0.0f) {
                ui.Ring(kx, r.y + r.h * 0.5f, knob, e.BorderThickness,
                        {e.BorderColor.r, e.BorderColor.g, e.BorderColor.b}, e.BorderColor.a);
            }
            break;
        }
        case Kind::Bar: {
            FillRect(e, r, ui);
            float v = glm::clamp(e.Value, 0.0f, 1.0f);
            if (v > 0.0f && e.BarFillColor.a > 0.0f) {
                // Заполнение с небольшим внутренним отступом, радиус — согласованный.
                float pad = glm::min(2.0f, glm::min(r.w, r.h) * 0.15f);
                float innerR = glm::max(e.Rounding - pad, 0.0f);
                glm::vec3 fill{e.BarFillColor.r, e.BarFillColor.g, e.BarFillColor.b};
                // Заполнение всегда с градиентом к более тёмному низу: плоская
                // полоса выглядит нарисованной в редакторе, а не «налитой».
                ui.GradientRect(r.x + pad, r.y + pad, (r.w - 2 * pad) * v, r.h - 2 * pad,
                                glm::mix(fill, glm::vec3(1.0f), 0.22f), fill * 0.78f,
                                e.BarFillColor.a, e.BarFillColor.a, innerR);
            }
            break;
        }
    }

    if (e.BorderThickness > 0.0f && e.BorderColor.a > 0.0f) {
        ui.RoundedRectOutline(r.x, r.y, r.w, r.h, e.Rounding, e.BorderThickness,
                              {e.BorderColor.r, e.BorderColor.g, e.BorderColor.b}, e.BorderColor.a);
    }

    // Иконка: у Kind::Icon занимает весь элемент, у остальных — квадрат у
    // левого края, и текст начинается за ней.
    float textLeft = r.x + e.PadX;
    if (!e.Icon.empty() && e.IconColor.a > 0.0f) {
        glm::vec3 iconRgb{e.IconColor.r, e.IconColor.g, e.IconColor.b};
        if (e.Type == Kind::Icon) {
            float side = glm::min(r.w, r.h);
            DrawIcon(ui, e.Icon, r.x + (r.w - side) * 0.5f, r.y + (r.h - side) * 0.5f, side,
                     iconRgb, e.IconColor.a);
        } else {
            float pad = glm::min(4.0f, r.h * 0.18f);
            float side = glm::max(r.h - pad * 2.0f, 4.0f);
            DrawIcon(ui, e.Icon, r.x + pad, r.y + pad, side, iconRgb, e.IconColor.a);
            textLeft = r.x + r.h; // = pad*2 + side
        }
    }

    // Поле ввода: своё содержимое, своя подсказка, свой курсор.
    if (e.Type == Kind::Input) {
        const bool empty = e.Text.empty();
        const std::string shown = e.Password ? MaskText(e.Text) : e.Text;
        const std::string draw = empty ? e.Placeholder : shown;
        const float textY = r.y + (r.h - ui.TextHeight(e.TextScale)) * 0.5f;
        if (!draw.empty()) {
            // Подсказка бледнее содержимого — иначе пустое поле выглядит
            // заполненным, и человек стирает то, чего не вводил.
            glm::vec3 rgb{e.TextColor.r, e.TextColor.g, e.TextColor.b};
            const float a = e.TextColor.a * (empty ? 0.45f : 1.0f);
            ui.Text(textLeft, textY, e.TextScale, rgb, draw, a);
        }
        // Курсор мигает только в фокусе и только когда поле включено.
        if (e.Focused && e.Enabled) {
            const std::string before = e.Password
                ? MaskText(e.Text.substr(0, (size_t)glm::clamp(e.Caret, 0, (int)e.Text.size())))
                : e.Text.substr(0, (size_t)glm::clamp(e.Caret, 0, (int)e.Text.size()));
            const float cx = textLeft + ui.MeasureText(before, e.TextScale);
            // Полсекунды виден, полсекунды нет — привычный ритм; после каждой
            // правки счётчик сбрасывается, чтобы курсор не пропал ровно тогда,
            // когда на него смотрят.
            if (std::fmod(e.CaretBlink, 1.0f) < 0.5f) {
                const float ch = ui.TextHeight(e.TextScale) * 1.15f;
                ui.Rect(cx, r.y + (r.h - ch) * 0.5f, glm::max(e.TextScale, 1.0f), ch,
                        {e.TextColor.r, e.TextColor.g, e.TextColor.b}, e.TextColor.a);
            }
        }
        return; // общий путь текста ниже полю ввода не нужен
    }

    if (!e.Text.empty() && e.TextColor.a > 0.0f) {
        glm::vec3 textRgb{e.TextColor.r, e.TextColor.g, e.TextColor.b};
        std::vector<std::string> lines;
        if (e.WrapText) {
            const float avail = e.TextCentered ? r.w - e.PadX * 2.0f : r.x + r.w - textLeft - e.PadX;
            lines = WrapLines(e.Text, avail, e.TextScale, ui);
        } else if (e.Text.find('\n') != std::string::npos) {
            lines = WrapLines(e.Text, 0.0f, e.TextScale, ui); // только по явным \n
        } else {
            lines.push_back(e.Text);
        }
        // Однострочный текст центрируется по высоте элемента — иначе подпись
        // рядом с иконкой висит выше неё, и строка «иконка + текст» выглядит
        // развалившейся. Многострочный блок центрируется целиком: считать его
        // высоту здесь уже есть чем.
        const float lineH = ui.LineHeight(e.TextScale);
        const float blockH = lineH * (float)(lines.size() - 1) + ui.TextHeight(e.TextScale);
        float textY = r.y + (r.h - blockH) * 0.5f;
        // Перенесённый текст ОБРЕЗАЕТСЯ своим элементом. Он переносился под эту
        // ширину — значит, и по высоте обязан остаться внутри: абзац, вылезший
        // из панели на фон, выглядит хуже, чем обрезанный по её краю, и главное
        // сразу виден как ошибка вёрстки, а не как «так и было задумано».
        const bool clipText = e.WrapText && blockH > r.h;
        if (clipText) ui.PushClipRect(r.x, r.y, r.w, r.h);
        for (const std::string& line : lines) {
            if (!line.empty()) {
                if (e.TextCentered) {
                    ui.TextCentered(r.x + r.w * 0.5f, textY, e.TextScale, textRgb, line,
                                    e.TextColor.a);
                } else {
                    ui.Text(textLeft, textY, e.TextScale, textRgb, line, e.TextColor.a);
                }
            }
            textY += lineH;
        }
        if (clipText) ui.PopClipRect();
    }
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
    // Плоское описание элемента, собранное из его компонентов ОДИН раз за
    // проход (см. UIBridge.h). Хранится здесь, а не пересобирается у каждого
    // потребителя: отрисовка, попадание курсором и ввод смотрят на одно и то
    // же, и собрать его трижды значило бы снова завести три расходящихся
    // взгляда на один элемент.
    UIElementComponent Flat;
    UIRect Rect;
    UIRect Clip;      // окно обрезки (нулевая ширина/высота — не обрезан)
    bool Clipped = false;
    float Alpha = 1.0f;      // накопленная прозрачность групп
    bool Interactive = true; // группа может запретить ввод всему поддереву
};

// Размер элемента по содержимому: отступ, значок, отступ, текст, отступ.
// Высота не подгоняется — её задаёт вёрстка.
glm::vec2 MeasureElement(const UIElementComponent& e, UIRenderer* ui) {
    if (!e.AutoWidth || !ui) return (e.LayoutSize.x > 0.0f) ? e.LayoutSize : e.Size;
    return MeasuredSize(e, *ui);
}

// Рекурсивный обход: считает прямоугольники, применяет раскладку, маски и
// групповые свойства. forced — прямоугольник, назначенный раскладкой родителя
// (nullptr — элемент стоит по своему якорю).
void SolveSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer* ui,
                  bool clipped, const UIRect& clip, float alpha, bool interactive,
                  const UIRect* forced, std::vector<Solved>& out) {
    entt::registry& reg = scene.Registry();
    const Transform& t = reg.get<Transform>(ent);
    UIElementComponent e = Compose(reg, ent);
    if (!e.Visible) return; // невидимый прячет и всё поддерево

    // ГЕОМЕТРИЯ БЕРЁТСЯ ИЗ Transform, а не из плоского описания.
    //
    // Плоское описание — это то, чем элемент РИСУЕТСЯ, и растяжения, полей и
    // точки привязки в нём нет: у прежнего компонента их не было вовсе.
    // Считать по нему раскладку значило бы, что панель «во всю ширину экрана»
    // молча остаётся размером 200x56 — то есть самая заметная возможность
    // новой системы не работает, и понять почему неоткуда.
    glm::vec2 size = ResolveSize(t, parentRect);
    // Ширина по содержимому — единственное, что знает шрифт, а не раскладка.
    if (e.AutoWidth && ui) size.x = MeasuredSize(e, *ui).x;
    UIRect r = forced ? *forced : Resolve(t, parentRect, size);
    if (forced) size = {forced->w, forced->h};
    // Фактический размер запоминается в САМОМ элементе: его читают попадание
    // курсором и следующий кадр, когда шрифта под рукой может не оказаться.
    e.LayoutSize = size;
    reg.get<Transform>(ent).LayoutSize = size;

    // Групповые свойства накапливаются вниз по дереву: спрятать панель — это
    // одно число на ней, а не проход скриптом по каждому её ребёнку.
    float myAlpha = alpha;
    bool myInteractive = interactive;
    if (const Group* g = reg.try_get<Group>(ent)) {
        myAlpha *= glm::clamp(g->Alpha, 0.0f, 1.0f);
        if (!g->Interactable || !g->BlockRaycasts) myInteractive = false;
    }

    out.push_back(Solved{ent, e, r, clip, clipped, myAlpha, myInteractive});

    std::vector<entt::entity> kids = SortedUIChildren(scene, ent);
    if (kids.empty()) return;

    // Маска: окно обрезки пересекается с родительским — вложенные маски режут
    // друг друга (список внутри окна виден только на их пересечении).
    bool childClipped = clipped;
    UIRect childClip = clip;
    const Mask* mask = reg.try_get<Mask>(ent);
    if (mask || e.ClipChildren) {
        const UIRect window = mask ? MaskWindow(*mask, r) : r;
        if (!mask || !mask->ShowOutside) {
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
            const UIElementComponent kid = Compose(reg, kids[i]);
            if (kid.AutoWidth && ui) slots[i].Size.x = MeasuredSize(kid, *ui).x;
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
        UIElementComponent flat = it.Flat;
        if (it.Alpha < 1.0f) {
            // Прозрачность группы умножается на все цвета элемента, а не
            // подменяет их: панель с полупрозрачным фоном не должна становиться
            // непрозрачной от того, что группу показали наполовину.
            flat.Color.a *= it.Alpha;
            flat.TextColor.a *= it.Alpha;
            flat.BorderColor.a *= it.Alpha;
            flat.IconColor.a *= it.Alpha;
            flat.BarFillColor.a *= it.Alpha;
            flat.GradientColor.a *= it.Alpha;
        }
        if (it.Clipped) {
            if (it.Clip.w <= 0.0f || it.Clip.h <= 0.0f) continue; // полностью обрезан
            ui.PushClipRect(it.Clip.x, it.Clip.y, it.Clip.w, it.Clip.h);
        }
        DrawElement(flat, it.Rect, ui);
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
