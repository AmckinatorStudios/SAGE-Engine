#include "sage/ui/render/UIBackend.h"

#include <algorithm>
#include <cmath>

#include "sage/render/Texture.h"
#include "sage/ui/UIRenderer.h"

// ---------------------------------------------------------------------------
// Бэкенд поверх штатного UIRenderer движка.
//
// Он НЕ ЗНАЕТ ничего об узлах, компонентах и дереве — только о списке команд. В
// этом и смысл границы: чтобы заменить рисование (другой API, снимок в файл,
// отладочный вывод), достаточно написать второй такой класс, не трогая ни
// строки в самой системе интерфейса.
//
// Где возможности UIRenderer уступают модели команд (разные радиусы у четырёх
// углов, мягкий край произвольной ширины), это ЯВНО отмечено в коде: система не
// делает вид, что нарисовала то, чего нарисовать не смогла.
// ---------------------------------------------------------------------------
namespace sage::ui {

namespace {
glm::vec3 Rgb(const UIColor& c) { return {c.r, c.g, c.b}; }

// Точка, прогнанная через преобразование узла. Для неповёрнутых узлов матрица
// единичная, и это ноль работы.
glm::vec2 Apply(const UIRenderCommand& c, glm::vec2 p) {
    if (!c.Transformed) return p;
    const glm::vec3 t = c.Transform * glm::vec3(p, 1.0f);
    return {t.x, t.y};
}
} // namespace

void UIClassicBackend::Begin(glm::vec2 screenPixels) {
    m_ui.Begin((int)screenPixels.x, (int)screenPixels.y);
    m_clipOpen = false;
}

void UIClassicBackend::End() {
    if (m_clipOpen) {
        m_ui.PopClipRect();
        m_clipOpen = false;
    }
    m_ui.End();
}

void UIClassicBackend::DrawRect(const UIRenderCommand& c) {
    // Мягкий край (тени, свечения) — несколько расширяющихся слоёв с падающей
    // прозрачностью. Это компромисс ИМЕННО ЭТОГО бэкенда: у штатного шейдера
    // нет параметра размытия. Модель команд его несёт, и бэкенд со своим
    // шейдером нарисует то же самое одним квадом.
    if (c.Softness > 0.5f && !c.Inner) {
        const int steps = std::min(8, std::max(3, (int)(c.Softness / 3.0f)));
        for (int i = steps; i >= 1; --i) {
            const float k = (float)i / (float)steps;
            const float grow = c.Softness * k;
            const float alpha = c.Color.a * (1.0f - k) / (float)steps * 2.2f;
            const UIRect r = UIInflate(c.Rect, UIEdges::Uniform(grow));
            m_ui.RoundedRect(r.x, r.y, r.w, r.h, Rgb(c.Color), alpha,
                             c.Radius.Max() + grow);
        }
        return;
    }
    if (c.Gradient.Active()) {
        // Градиент раскладывается в вертикальный переход между крайними
        // остановками: больше штатный рисующий не умеет, и врать про это не
        // нужно — цвета берутся честно из градиента.
        const UIColor top = c.Gradient.Evaluate(0.0f) * c.Color;
        const UIColor bottom = c.Gradient.Evaluate(1.0f) * c.Color;
        m_ui.GradientRect(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, Rgb(top), Rgb(bottom), top.a,
                          bottom.a, c.Radius.Max());
        return;
    }
    // Разные радиусы у четырёх углов штатный шейдер не поддерживает — берём
    // наибольший. Данные при этом не теряются: они в команде.
    m_ui.RoundedRect(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, Rgb(c.Color), c.Color.a,
                     c.Radius.Max());
}

void UIClassicBackend::DrawBorder(const UIRenderCommand& c) {
    m_ui.RoundedRectOutline(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, c.Radius.Max(),
                            std::max(1.0f, c.Thickness), Rgb(c.Color), c.Color.a);
}

void UIClassicBackend::DrawImage(const UIRenderCommand& c) {
    if (!c.Tex) return;
    UIRenderer::Sprite sprite;
    const float w = (float)c.Tex->Width();
    const float h = (float)c.Tex->Height();
    // UV в команде нормализованы; штатный рисующий хочет пиксели исходника.
    sprite.X = c.Uv.x * w;
    sprite.Y = c.Uv.y * h;
    sprite.W = (c.Uv.z - c.Uv.x) * w;
    sprite.H = (c.Uv.w - c.Uv.y) * h;
    if (sprite.W <= 0.0f || sprite.H <= 0.0f) {
        m_ui.Image(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, c.Tex, Rgb(c.Color), c.Color.a,
                   c.Radius.Max());
        return;
    }
    m_ui.ImageSprite(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, c.Tex, sprite, Rgb(c.Color),
                     c.Color.a);
}

void UIClassicBackend::DrawNineSlice(const UIRenderCommand& c) {
    if (!c.Tex) return;
    UIRenderer::Sprite sprite;
    const float w = (float)c.Tex->Width();
    const float h = (float)c.Tex->Height();
    sprite.X = c.Uv.x * w;
    sprite.Y = c.Uv.y * h;
    sprite.W = (c.Uv.z - c.Uv.x) * w;
    sprite.H = (c.Uv.w - c.Uv.y) * h;
    m_ui.ImageNineSlice(c.Rect.x, c.Rect.y, c.Rect.w, c.Rect.h, c.Tex, sprite,
                        glm::vec4(c.Slice.L, c.Slice.T, c.Slice.R, c.Slice.B), 1.0f,
                        Rgb(c.Color), c.Color.a);
}

void UIClassicBackend::DrawGlyphs(const UIRenderCommand& c, const UIRenderList& list) {
    // Штатный рисующий работает строками, а не отдельными глифами. Собираем
    // строку обратно и рисуем одним вызовом: это тот же результат при тех же
    // метриках, потому что раскладка пользовалась ими же.
    if (c.GlyphCount <= 0) return;
    const std::vector<UIGlyphDraw>& glyphs = list.Glyphs();
    const int first = c.GlyphFirst;
    const int last = std::min((int)glyphs.size(), first + c.GlyphCount);
    if (first >= last) return;

    // Масштаб UIRenderer: его Text() принимает «масштаб», а не кегль в
    // пикселях. Переводим через известное отношение (8 единиц на кегль).
    float scale = glyphs[(size_t)first].Size / 8.0f;
    if (scale <= 0.0f) scale = 1.0f;

    std::string run;
    float runX = glyphs[(size_t)first].Pos.x;
    float runY = glyphs[(size_t)first].Pos.y;
    UIColor runColor = glyphs[(size_t)first].Color;

    auto flush = [&]() {
        if (run.empty()) return;
        // Позиция глифа — базовая линия; Text() хочет верхний край.
        m_ui.Text(runX, runY - m_ui.LineHeight(scale) * 0.78f, scale, Rgb(runColor), run,
                  runColor.a);
        run.clear();
    };

    float expectedX = runX;
    for (int i = first; i < last; ++i) {
        const UIGlyphDraw& g = glyphs[(size_t)i];
        const bool sameLine = std::fabs(g.Pos.y - runY) < 0.5f;
        const bool contiguous = std::fabs(g.Pos.x - expectedX) < 1.5f;
        const bool sameColor = g.Color == runColor;
        if (!run.empty() && (!sameLine || !contiguous || !sameColor)) {
            flush();
            runX = g.Pos.x;
            runY = g.Pos.y;
            runColor = g.Color;
        } else if (run.empty()) {
            runX = g.Pos.x;
            runY = g.Pos.y;
            runColor = g.Color;
        }
        // Кодовая точка обратно в UTF-8.
        const uint32_t cp = g.Codepoint;
        if (cp < 0x80) run.push_back((char)cp);
        else if (cp < 0x800) {
            run.push_back((char)(0xC0 | (cp >> 6)));
            run.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            run.push_back((char)(0xE0 | (cp >> 12)));
            run.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            run.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            run.push_back((char)(0xF0 | (cp >> 18)));
            run.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            run.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            run.push_back((char)(0x80 | (cp & 0x3F)));
        }
        expectedX = g.Pos.x + m_ui.MeasureText(std::string(1, ' '), scale) * 0.0f;
        // Ожидаемая позиция следующего глифа — по фактической ширине уже
        // собранного куска: так разрыв ловится там, где раскладка его и сделала.
        expectedX = runX + m_ui.MeasureText(run, scale);
    }
    flush();
}

void UIClassicBackend::DrawPolygon(const UIRenderCommand& c) {
    if (c.Points.size() < 3) return;
    // Веер треугольников от первой вершины: для выпуклых форм (а процедурные
    // формы интерфейса именно такие) этого достаточно и не нужен триангулятор.
    const glm::vec2 a = Apply(c, c.Points[0]);
    for (size_t i = 1; i + 1 < c.Points.size(); ++i) {
        m_ui.Triangle(a, Apply(c, c.Points[i]), Apply(c, c.Points[i + 1]), Rgb(c.Color),
                      c.Color.a);
    }
}

void UIClassicBackend::DrawRing(const UIRenderCommand& c) {
    const glm::vec2 centre = UICenter(c.Rect);
    const float radius = std::min(c.Rect.w, c.Rect.h) * 0.5f;
    if (c.SweepAngle >= 359.5f) {
        m_ui.Ring(centre.x, centre.y, radius, std::max(1.0f, c.Thickness), Rgb(c.Color),
                  c.Color.a);
        return;
    }
    // Дуга — полоса четырёхугольников по сегментам. Шаг выбирается по радиусу:
    // фиксированное число сегментов либо рвёт большую дугу, либо тратит
    // геометрию на маленькую.
    const int segments = std::max(4, (int)(std::fabs(c.SweepAngle) / 6.0f));
    const float inner = std::max(0.0f, radius - std::max(1.0f, c.Thickness));
    const float start = (c.StartAngle - 90.0f) * 3.14159265358979f / 180.0f;
    const float sweep = c.SweepAngle * 3.14159265358979f / 180.0f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = start + sweep * (float)i / (float)segments;
        const float a1 = start + sweep * (float)(i + 1) / (float)segments;
        const glm::vec2 p0{centre.x + std::cos(a0) * inner, centre.y + std::sin(a0) * inner};
        const glm::vec2 p1{centre.x + std::cos(a0) * radius, centre.y + std::sin(a0) * radius};
        const glm::vec2 p2{centre.x + std::cos(a1) * radius, centre.y + std::sin(a1) * radius};
        const glm::vec2 p3{centre.x + std::cos(a1) * inner, centre.y + std::sin(a1) * inner};
        m_ui.Quad(p0, p1, p2, p3, Rgb(c.Color), c.Color.a);
    }
}

void UIClassicBackend::Submit(const UIRenderList& list) {
    for (const UIRenderBatch& batch : list.Batches()) {
        for (int i = 0; i < batch.Count; ++i) {
            const UIRenderCommand& c = list.Commands()[(size_t)(batch.First + i)];
            if (c.Op != UIPassOp::Draw) {
                // Промежуточные цели этот бэкенд не поддерживает: поддерево
                // рисуется как есть, без размытия. Это честная деградация —
                // интерфейс остаётся рабочим, просто без одного эффекта (§134).
                continue;
            }
            // Ножницы ставятся ровно на границе состояния — по одному вызову на
            // батч, а не на команду.
            if (i == 0) {
                if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
                if (c.Clip.HasScissor && UIRectValid(c.Clip.Scissor)) {
                    m_ui.PushClipRect(c.Clip.Scissor.x, c.Clip.Scissor.y, c.Clip.Scissor.w,
                                      c.Clip.Scissor.h);
                    m_clipOpen = true;
                }
            }
            switch (c.Kind) {
                case UIPrimitive::Rect: DrawRect(c); break;
                case UIPrimitive::Border: DrawBorder(c); break;
                case UIPrimitive::Image: DrawImage(c); break;
                case UIPrimitive::NineSlice: DrawNineSlice(c); break;
                case UIPrimitive::Glyphs: DrawGlyphs(c, list); break;
                case UIPrimitive::Polygon:
                case UIPrimitive::Line: DrawPolygon(c); break;
                case UIPrimitive::Ring: DrawRing(c); break;
                default: break;
            }
        }
        if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
    }
}

} // namespace sage::ui
