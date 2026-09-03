#include "ProjectTemplateCover.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

#include "imgui.h"

#include "Localization.h"

namespace {

ImU32 Col(float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

// Цвет, умноженный на яркость: грани одного куба отличаются только светом, и
// заводить под каждую свой набор чисел — верный способ получить куб из трёх
// разных материалов при первой же правке палитры.
ImU32 Shade(const ImVec4& c, float k) {
    return Col(std::min(1.0f, c.x * k), std::min(1.0f, c.y * k), std::min(1.0f, c.z * k), c.w);
}

// Небо и земля РАЗНЫМИ прямоугольниками, с явной линией горизонта.
//
// Сначала градиент шёл на всю обложку, а земля клалась поверх — и горизонта не
// получалось вовсе: светлая часть градиента оказывалась ровно там, где её
// закрывала земля, а видимая (верхняя) половина выходила однотонно-тёмной. На
// картинке размером с ноготь горизонт — единственное, по чему видно, что это
// сцена, а не абстрактный прямоугольник.
void SkyAndGround(ImDrawList* dl, ImVec2 a, ImVec2 b, float horizon) {
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, horizon), Col(0.14f, 0.17f, 0.24f),
                                Col(0.14f, 0.17f, 0.24f), Col(0.52f, 0.58f, 0.66f),
                                Col(0.52f, 0.58f, 0.66f));
    dl->AddRectFilled(ImVec2(a.x, horizon), b, Col(0.19f, 0.20f, 0.24f));
}

// Площадка под объектами: трапеция «в перспективе» — дальний край уже ближнего.
// Цвет тот же, что у пола демо-сцены (0.30, 0.32, 0.36).
void Ground(ImDrawList* dl, ImVec2 c, float w, float yFar, float yNear) {
    const ImVec2 pts[4] = {{c.x - w * 0.26f, yFar},
                           {c.x + w * 0.26f, yFar},
                           {c.x + w * 0.60f, yNear},
                           {c.x - w * 0.60f, yNear}};
    dl->AddConvexPolyFilled(pts, 4, Col(0.30f, 0.32f, 0.36f));
}

// Куб «в две с половиной оси»: передняя грань, верх и правый бок. Настоящую
// проекцию здесь заводить незачем — обложка размером с ноготь, а разница между
// честной изометрией и тремя четырёхугольниками на ней не видна.
void Box(ImDrawList* dl, ImVec2 base, float w, float h, const ImVec4& c) {
    const float hw = w * 0.5f;
    const float off = w * 0.34f;  // сдвиг дальней грани вправо-вверх
    const float dy = w * 0.24f;

    const ImVec2 front[4] = {{base.x - hw, base.y},
                             {base.x + hw, base.y},
                             {base.x + hw, base.y - h},
                             {base.x - hw, base.y - h}};
    const ImVec2 side[4] = {{base.x + hw, base.y},
                            {base.x + hw + off, base.y - dy},
                            {base.x + hw + off, base.y - h - dy},
                            {base.x + hw, base.y - h}};
    const ImVec2 top[4] = {{base.x - hw, base.y - h},
                           {base.x + hw, base.y - h},
                           {base.x + hw + off, base.y - h - dy},
                           {base.x - hw + off, base.y - h - dy}};
    dl->AddConvexPolyFilled(side, 4, Shade(c, 0.66f));
    dl->AddConvexPolyFilled(front, 4, Shade(c, 1.0f));
    dl->AddConvexPolyFilled(top, 4, Shade(c, 1.32f));
}

// Полоска интерфейса: подложка и заполненная часть. Ею на обложке показан и
// худ (полоса здоровья), и кнопка меню — разница только в цвете и доле.
void UIBar(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 back, ImU32 fill, float part) {
    const float r = (b.y - a.y) * 0.35f;
    dl->AddRectFilled(a, b, back, r);
    if (part > 0.0f)
        dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * part, b.y), fill, r);
}

// Пунктирная рамка — «здесь ничего нет, и место свободно». Сплошная читалась
// бы как край панели, то есть как ещё один объект.
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash) {
    auto line = [&](ImVec2 p, ImVec2 q) {
        const float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        if (len <= 0.0f) return;
        const ImVec2 dir{(q.x - p.x) / len, (q.y - p.y) / len};
        for (float t = 0.0f; t < len; t += dash * 2.0f) {
            const float e = std::min(t + dash, len);
            dl->AddLine(ImVec2(p.x + dir.x * t, p.y + dir.y * t),
                        ImVec2(p.x + dir.x * e, p.y + dir.y * e), col, 1.0f);
        }
    };
    line(a, ImVec2(b.x, a.y));
    line(ImVec2(b.x, a.y), b);
    line(b, ImVec2(a.x, b.y));
    line(ImVec2(a.x, b.y), a);
}

} // namespace

void DrawProjectTemplateCover(ProjectTemplateKind kind, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(x, y), b(x + w, y + h);
    const float rounding = std::floor(h * 0.08f);

    // Обрезка по обложке: рисунок собирается из примитивов «на глазок», и
    // единственная надёжная гарантия, что ничего не вылезет на подпись, —
    // ножницы, а не аккуратность каждой координаты.
    dl->PushClipRect(a, b, true);

    if (kind == ProjectTemplateKind::Empty) {
        // ПУСТО — и обложка обязана быть пустой. Ни объекта, ни интерфейса, ни
        // горизонта: всё это шаблон как раз и не создаёт.
        dl->AddRectFilled(a, b, Col(0.10f, 0.11f, 0.13f), rounding);
        DashedRect(dl, ImVec2(a.x + w * 0.10f, a.y + h * 0.14f),
                   ImVec2(b.x - w * 0.10f, b.y - h * 0.14f), Col(0.42f, 0.45f, 0.52f, 0.75f),
                   std::max(2.0f, w * 0.03f));
    } else {
        const float horizon = y + h * 0.54f;
        const float bottom = y + h * 0.94f;
        SkyAndGround(dl, a, b, horizon);
        Ground(dl, ImVec2(x + w * 0.5f, y), w, horizon, bottom);

        // Солнце — оно есть в обоих непустых шаблонах.
        dl->AddCircleFilled(ImVec2(x + w * 0.83f, y + h * 0.18f), h * 0.075f,
                            Col(1.0f, 0.93f, 0.72f, 0.95f));

        if (kind == ProjectTemplateKind::Copy) {
            // ВИТРИНА: платформа с участками. Ряд подиумов уходит вглубь, на
            // каждом свой предмет — это и есть обещание шаблона: не одна сцена,
            // а несколько готовых уголков, по которым ходят.
            const float base = y + h * 0.84f;
            const float ph = h * 0.06f;   // высота подиума
            struct Zone { float u; float scale; ImVec4 color; int shape; };
            const Zone zones[] = {
                {0.16f, 1.00f, {0.82f, 0.42f, 0.34f, 1}, 0},   // куб
                {0.38f, 0.88f, {0.42f, 0.70f, 0.85f, 1}, 1},   // шар
                {0.60f, 0.78f, {0.55f, 0.80f, 0.48f, 1}, 2},   // столб
                {0.80f, 0.68f, {0.86f, 0.74f, 0.40f, 1}, 1},
            };
            for (const Zone& z : zones) {
                const float cx = x + w * z.u;
                const float top = base - (1.0f - z.scale) * h * 0.16f;
                const float pw = w * 0.13f * z.scale;
                // Подиум — плоская плита: она отделяет участок от пола и
                // читается как «здесь что-то показывают».
                dl->AddRectFilled(ImVec2(cx - pw, top - ph), ImVec2(cx + pw, top),
                                  Col(0.30f, 0.32f, 0.38f, 1.0f), ph * 0.35f);
                const float s = h * 0.17f * z.scale;
                if (z.shape == 0) {
                    Box(dl, ImVec2(cx, top - ph), s, s, z.color);
                } else if (z.shape == 1) {
                    dl->AddCircleFilled(ImVec2(cx, top - ph - s * 0.5f), s * 0.5f, Shade(z.color, 1.0f));
                } else {
                    Box(dl, ImVec2(cx, top - ph), s * 0.45f, s * 1.5f, z.color);
                }
            }
            // Худ витрины в углу — по нему видно, что интерфейс в проекте тоже
            // есть и тоже на скриптах.
            UIBar(dl, ImVec2(x + w * 0.06f, y + h * 0.09f), ImVec2(x + w * 0.40f, y + h * 0.17f),
                  Col(0.0f, 0.0f, 0.0f, 0.55f), Col(0.42f, 0.62f, 0.85f), 0.62f);
        } else if (kind == ProjectTemplateKind::Demo) {
            // Те же цвета, что у объектов демо-сцены (см. EditorLayer::NewScene).
            const float base = y + h * 0.80f;
            const float cube = h * 0.22f;
            // Башня рисуется ПЕРВОЙ и стоит выше по экрану: в сцене она дальше
            // остальных, а порядок отрисовки здесь и есть глубина.
            Box(dl, ImVec2(x + w * 0.52f, y + h * 0.68f), cube * 0.55f, cube * 1.7f,
                ImVec4(0.90f, 0.80f, 0.35f, 1));
            Box(dl, ImVec2(x + w * 0.32f, base), cube, cube, ImVec4(0.85f, 0.30f, 0.30f, 1));
            Box(dl, ImVec2(x + w * 0.50f, base), cube, cube, ImVec4(0.35f, 0.75f, 0.40f, 1));
            Box(dl, ImVec2(x + w * 0.68f, base), cube, cube, ImVec4(0.35f, 0.55f, 0.90f, 1));
            // Сфера — витрина криволинейных примитивов, стоит на полу слева.
            dl->AddCircleFilled(ImVec2(x + w * 0.17f, base - cube * 0.45f), cube * 0.45f,
                                Col(0.85f, 0.55f, 0.25f));
            // Демо-худ в углу — он тоже входит в шаблон.
            UIBar(dl, ImVec2(x + w * 0.06f, y + h * 0.10f), ImVec2(x + w * 0.36f, y + h * 0.18f),
                  Col(0.0f, 0.0f, 0.0f, 0.55f), Col(0.85f, 0.30f, 0.30f), 0.72f);
        } else {
            // Стартер интерфейса: сцена почти пустая, зато экраны готовы.
            // Меню по центру — четыре кнопки, как в демо «menu», и первая
            // подсвечена: в меню всегда есть выбранный пункт, и без него ряд
            // читается как четыре одинаковых серых полоски.
            const float mw = w * 0.40f;
            const float mx = x + (w - mw) * 0.5f;
            const float bh = h * 0.115f;
            for (int i = 0; i < 4; ++i) {
                const float by = y + h * 0.28f + (bh + h * 0.045f) * (float)i;
                const bool first = i == 0;
                UIBar(dl, ImVec2(mx, by), ImVec2(mx + mw, by + bh),
                      Col(0.22f, 0.25f, 0.36f, 0.98f),
                      first ? Col(0.42f, 0.52f, 0.75f, 1.0f) : Col(0, 0, 0, 0),
                      first ? 1.0f : 0.0f);
            }
            // И худ в углу — второй экран шаблона.
            UIBar(dl, ImVec2(x + w * 0.06f, y + h * 0.10f), ImVec2(x + w * 0.32f, y + h * 0.18f),
                  Col(0.0f, 0.0f, 0.0f, 0.55f), Col(0.85f, 0.30f, 0.30f), 0.72f);
        }
    }

    dl->PopClipRect();
    dl->AddRect(a, b, Col(0.0f, 0.0f, 0.0f, 0.55f), rounding);
}

float ProjectTemplateCardHeight(float width) {
    const ImGuiStyle& st = ImGui::GetStyle();
    // Обложка в пропорции 16:9 плюс строка названия и рамка карточки.
    return std::floor(width * 9.0f / 16.0f) + ImGui::GetTextLineHeight() +
           st.FramePadding.y * 2.0f + st.ItemSpacing.y;
}

bool ProjectTemplateCard(const ProjectTemplate& tpl, bool selected, float width) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float height = ProjectTemplateCardHeight(width);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Кнопка ПОДАЁТСЯ ПЕРВОЙ, а рисуется поверх неё: так вся карточка целиком —
    // одна нажимаемая область, и попасть в неё проще, чем в кружок радиокнопки.
    // ШАБЛОН, КОТОРОГО НЕТ НА ДИСКЕ, ВЫГЛЯДИТ ИНАЧЕ.
    //
    // Карточка витрины показывалась одинаково, стоит её папка рядом с
    // редактором или нет; выбор проходил, а «Создать» отказывало сообщением на
    // английском — то есть шаблон «есть, но не работает», и почему, узнать было
    // неоткуда. Недоступность обязана быть видна ДО нажатия и объяснена там же.
    const bool available = ProjectTemplateAvailable(tpl);

    ImGui::PushID(tpl.Id.c_str());
    const bool pressed = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        if (available) {
            ImGui::SetTooltip("%s", T(tpl.Summary));
        } else {
            ImGui::SetTooltip("%s\n\n%s\n%s", T(tpl.Summary),
                              T("This template is not installed next to the editor:"),
                              (ProjectTemplatesRoot() / tpl.SourceDir).string().c_str());
        }
    }
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = origin;
    const ImVec2 b(origin.x + width, origin.y + height);

    // Выбранная карточка — рамкой и подложкой акцентного цвета, а не только
    // цветом текста: цветом текста «выбрано» не читается на трёх карточках
    // подряд, а именно это и был единственный признак у радиокнопок.
    const ImVec4 accent(0.35f, 0.55f, 0.90f, 1.0f);
    if (selected) dl->AddRectFilled(a, b, Col(accent.x, accent.y, accent.z, 0.22f), 5.0f);
    else if (hovered) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 5.0f);
    dl->AddRect(a, b, selected ? Col(accent.x, accent.y, accent.z, 1.0f)
                               : Col(1.0f, 1.0f, 1.0f, 0.14f),
                5.0f, 0, selected ? 2.0f : 1.0f);

    const float pad = st.FramePadding.x;
    const float coverW = width - pad * 2.0f;
    const float coverH = std::floor(width * 9.0f / 16.0f) - st.FramePadding.y;
    DrawProjectTemplateCover(tpl.Kind, a.x + pad, a.y + st.FramePadding.y, coverW, coverH);

    // Недоступный — притушен и подписан поперёк обложки. Притушить мало: серая
    // карточка читается как «не выбрана», а не как «нельзя».
    if (!available) {
        dl->AddRectFilled(ImVec2(a.x + pad, a.y + st.FramePadding.y),
                          ImVec2(a.x + pad + coverW, a.y + st.FramePadding.y + coverH),
                          Col(0.06f, 0.07f, 0.09f, 0.72f), 3.0f);
        const char* mark = T("not installed");
        const ImVec2 ms = ImGui::CalcTextSize(mark);
        dl->AddText(ImVec2(a.x + pad + (coverW - ms.x) * 0.5f,
                           a.y + st.FramePadding.y + (coverH - ms.y) * 0.5f),
                    Col(1.0f, 0.72f, 0.35f, 1.0f), mark);
    }

    // Название под обложкой, по центру. Имена шаблонов переводятся, и по-русски
    // они длиннее — не влезающее УКОРАЧИВАЕТСЯ С МНОГОТОЧИЕМ, а не режется по
    // краю карточки. Разница не косметическая: обрезанное «Menu and HUD»
    // выходило как «Menu and HUI» и читалось опечаткой, а не «здесь не всё».
    std::string name = T(tpl.Name);
    const float room = width - pad * 2.0f;
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize();

    // СНАЧАЛА УМЕНЬШАЕМ ШРИФТ, и только потом режем. «Menu and HUD» и «Empty
    // project» не влезают в карточку ряда из четырёх, и обрезка превращала их
    // в «Menu and H...» — читается как поломка, а не как длинное имя. Ужатое
    // на пятую часть имя читается целиком и остаётся именем.
    const float minSize = size * 0.78f;
    while (size > minSize &&
           font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str()).x > room) {
        size -= 1.0f;
    }
    // Не помогло (очень длинный перевод) — тогда многоточие: пусть будет видно,
    // что имя не целиком, а не половина слова у края.
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str()).x > room) {
        const std::string tail = "...";
        while (!name.empty() &&
               font->CalcTextSizeA(size, FLT_MAX, 0.0f, (name + tail).c_str()).x > room) {
            // По границе UTF-8: половина символа кириллицы — это не текст.
            do { name.pop_back(); }
            while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80);
        }
        name += tail;
    }
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str());
    const ImVec2 textPos(a.x + std::max(pad, (width - ts.x) * 0.5f),
                         a.y + st.FramePadding.y + coverH + 2.0f);
    dl->AddText(font, size, textPos,
                ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                name.c_str());

    return pressed;
}
