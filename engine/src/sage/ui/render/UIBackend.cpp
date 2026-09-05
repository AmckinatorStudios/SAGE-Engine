#include "sage/ui/render/UIBackend.h"

#include <algorithm>
#include <cmath>

#include "sage/render/Texture.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/UIIcons.h"
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
    m_screen = screenPixels;
    m_offscreenDepth = 0;
    m_ui.Begin((int)screenPixels.x, (int)screenPixels.y);
    m_clipOpen = false;
}

UIClassicBackend::Target& UIClassicBackend::Acquire(size_t slot, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (m_pool.size() <= slot) m_pool.resize(slot + 1);
    Target& t = m_pool[slot];
    if (!t.Fbo) {
        t.Fbo = std::make_unique<Framebuffer>(width, height);
        t.View = Texture::Wrap(t.Fbo->ColorTexture(), width, height);
    } else if (t.Fbo->Width() != width || t.Fbo->Height() != height) {
        t.Fbo->Resize(width, height);
        // Хендл после пересоздания хранилища другой — обёртку надо обновить,
        // иначе рисовалась бы текстура, которой больше нет.
        t.View = Texture::Wrap(t.Fbo->ColorTexture(), width, height);
    }
    return t;
}

void UIClassicBackend::BindRoot() {
    if (m_root) m_root->Bind();
    else sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
}

void UIClassicBackend::BeginOffscreen() {
    // Всё накопленное уходит в текущую цель — иначе поддерево, которое сейчас
    // начнётся, смешалось бы с тем, что было до него.
    m_ui.End();
    const int w = (int)m_screen.x, h = (int)m_screen.y;
    Target& t = Acquire((size_t)m_offscreenDepth * 8, w, h);
    t.Fbo->Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    // Прозрачный фон обязателен: в цель попадает ТОЛЬКО поддерево, всё
    // остальное должно остаться видимым при композиции.
    device.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    device.Clear();
    ++m_offscreenDepth;
    m_ui.Begin(w, h);
    m_clipOpen = false;
}

const Texture* UIClassicBackend::BlurChain(size_t firstSlot, const Texture* source, float radius,
                                           int passes) {
    if (!source || radius <= 0.5f) return source;
    // ПРОМЕЖУТОЧНЫЕ ЦЕЛИ ХРАНЯТ ЦВЕТ, УМНОЖЕННЫЙ НА ПРОЗРАЧНОСТЬ. Так его туда
    // положило обычное альфа-смешивание поверх прозрачного фона. Если рисовать
    // такую картинку дальше обычным режимом, цвет умножится на альфу ещё раз —
    // и ещё раз на каждой ступени. На восьми ступенях от полупрозрачных краёв
    // не остаётся ничего: интерфейс с размытием просто чернеет. Поэтому вся
    // цепочка идёт в режиме «уже умноженной» альфы.
    const auto premultiplied = sage::rhi::GraphicsDevice::BlendMode::Premultiplied;
    // Сколько ступеней уменьшения нужно под запрошенный радиус. Радиус 8 — одна
    // ступень, 16 — две, и так далее: каждая ступень удваивает область
    // усреднения.
    int levels = 1;
    while (levels < 4 && radius > (float)(8 << (levels - 1))) ++levels;
    passes = std::max(1, std::min(passes, 3));

    const int w = (int)m_screen.x, h = (int)m_screen.y;
    const Texture* current = source;
    for (int pass = 0; pass < passes; ++pass) {
        // Вниз.
        for (int i = 1; i <= levels; ++i) {
            const int lw = std::max(1, w >> i), lh = std::max(1, h >> i);
            Target& t = Acquire(firstSlot + (size_t)i, lw, lh);
            t.Fbo->Bind();
            sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
            device.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            device.Clear();
            m_ui.Begin(lw, lh);
            m_ui.SetBlendMode(premultiplied);
            m_ui.Image(0.0f, 0.0f, (float)lw, (float)lh, current);
            m_ui.End();
            current = t.View.get();
        }
        // Вверх.
        for (int i = levels - 1; i >= 0; --i) {
            const int lw = std::max(1, w >> i), lh = std::max(1, h >> i);
            Target& t = Acquire(firstSlot + (size_t)(i == 0 ? 5 : i), lw, lh);
            t.Fbo->Bind();
            sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
            device.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            device.Clear();
            m_ui.Begin(lw, lh);
            m_ui.SetBlendMode(premultiplied);
            m_ui.Image(0.0f, 0.0f, (float)lw, (float)lh, current);
            m_ui.End();
            current = t.View.get();
        }
    }
    return current;
}

void UIClassicBackend::EndOffscreen(const UIRenderCommand& c) {
    // Поддерево уже нарисовано в цель — доводим его до GPU.
    m_ui.End();
    if (m_offscreenDepth > 0) --m_offscreenDepth;

    const size_t slot = (size_t)m_offscreenDepth * 8;
    Target& subtree = Acquire(slot, (int)m_screen.x, (int)m_screen.y);
    subtree.Fbo->Resolve();
    const Texture* result = BlurChain(slot, subtree.View.get(), c.Softness, (int)c.Thickness);

    BindRoot();
    if (result) {
        // Композиция: результат кладётся поверх того, что было под ним. Тем же
        // режимом «уже умноженной» альфы — цель хранит именно такой цвет.
        m_ui.Begin((int)m_screen.x, (int)m_screen.y);
        m_ui.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Premultiplied);
        m_ui.Image(0.0f, 0.0f, m_screen.x, m_screen.y, result,
                   glm::vec3(c.Color.r, c.Color.g, c.Color.b), c.Color.a);
        m_ui.End();
    }
    // Дальше кадр рисуется как обычно: обычные цвета в промежуточных целях не
    // бывали и умножены на альфу не были.
    m_ui.Begin((int)m_screen.x, (int)m_screen.y);
    m_clipOpen = false;
}

void UIClassicBackend::End() {
    m_ui.SetMask(nullptr);
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

namespace {
// Режим наложения интерфейса → режим смешивания GPU. Не все семь режимов
// выражаются функцией смешивания (Multiply, Screen, Overlay нужны либо шейдеру,
// либо промежуточной цели), поэтому здесь честно поддержаны те, что
// выражаются; остальные рисуются обычным наложением, а не «примерно похоже».
sage::rhi::GraphicsDevice::BlendMode ToDeviceBlend(UIBlendMode mode) {
    switch (mode) {
        case UIBlendMode::Add:
        case UIBlendMode::Screen:
        case UIBlendMode::Lighten: return sage::rhi::GraphicsDevice::BlendMode::Additive;
        default: return sage::rhi::GraphicsDevice::BlendMode::Alpha;
    }
}
} // namespace

void UIClassicBackend::Submit(const UIRenderList& list) {
    UIBlendMode currentBlend = UIBlendMode::Normal;
    for (const UIRenderBatch& batch : list.Batches()) {
        // Смена режима наложения рвёт вызов рисования — как и любая другая
        // смена состояния GPU. Свечение поверх панели обязано складываться, а
        // не закрывать её, и это стоит ровно одного лишнего вызова.
        if (batch.Blend != currentBlend && batch.Count > 0 &&
            list.Commands()[(size_t)batch.First].Op == UIPassOp::Draw) {
            if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
            m_ui.End();
            m_ui.Begin((int)m_screen.x, (int)m_screen.y);
            m_ui.SetBlendMode(ToDeviceBlend(batch.Blend));
            currentBlend = batch.Blend;
        }
        for (int i = 0; i < batch.Count; ++i) {
            const UIRenderCommand& c = list.Commands()[(size_t)(batch.First + i)];
            if (c.Op == UIPassOp::BeginOffscreen) {
                if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
                BeginOffscreen();
                continue;
            }
            if (c.Op == UIPassOp::EndOffscreen) {
                if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
                EndOffscreen(c);
                continue;
            }
            if (c.Op != UIPassOp::Draw) continue;
            // Ножницы и фигурная маска ставятся ровно на границе состояния —
            // по одному вызову на батч, а не на команду.
            if (i == 0) {
                if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
                if (c.Clip.HasScissor && UIRectValid(c.Clip.Scissor)) {
                    m_ui.PushClipRect(c.Clip.Scissor.x, c.Clip.Scissor.y, c.Clip.Scissor.w,
                                      c.Clip.Scissor.h);
                    m_clipOpen = true;
                }
                if (!c.Clip.Shape) {
                    m_ui.SetMask(nullptr);
                } else {
                    const UIMaskShape& s = *c.Clip.Shape;
                    UIRenderer::Mask mask;
                    switch (s.Form) {
                        case UIMaskShape::Kind::Ellipse:
                            mask.Form = UIRenderer::Mask::Shape::Ellipse;
                            break;
                        case UIMaskShape::Kind::Texture:
                            mask.Form = UIRenderer::Mask::Shape::Texture;
                            break;
                        case UIMaskShape::Kind::Gradient:
                            mask.Form = UIRenderer::Mask::Shape::Gradient;
                            break;
                        default: mask.Form = UIRenderer::Mask::Shape::RoundedRect; break;
                    }
                    mask.X = s.Rect.x; mask.Y = s.Rect.y;
                    mask.W = s.Rect.w; mask.H = s.Rect.h;
                    mask.Radius = glm::vec4(s.Radius.TL, s.Radius.TR, s.Radius.BR, s.Radius.BL);
                    mask.Softness = s.Softness;
                    mask.Invert = s.Invert;
                    mask.Tex = s.Tex;
                    mask.Channel = s.Channel;
                    mask.GradientAngle = s.GradientAngle;
                    mask.GradientStart = s.GradientStart;
                    mask.GradientEnd = s.GradientEnd;
                    m_ui.SetMask(&mask);
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
                case UIPrimitive::Custom:
                    // Единственный материал, который знает ЭТОТ бэкенд, —
                    // векторный значок движка. Чужой материал он честно
                    // пропускает: рисовать неизвестное нечем, а притворяться
                    // хуже, чем не рисовать (§134).
                    if (c.Material && c.Material->Shader == "icon") {
                        sage::ui::DrawIcon(m_ui, c.Material->Name, c.Rect.x, c.Rect.y,
                                           std::min(c.Rect.w, c.Rect.h), Rgb(c.Color),
                                           c.Color.a);
                    }
                    break;
                default: break;
            }
        }
        if (m_clipOpen) { m_ui.PopClipRect(); m_clipOpen = false; }
    }
}

} // namespace sage::ui
