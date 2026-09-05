#include "sage/ui/layout/UILayoutSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "sage/ui/core/UINode.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMask.h"
#include "sage/ui/widgets/UIWidgets.h"

namespace sage::ui {

namespace {

// Ограничения размера. Ноль означает «не ограничено» — отдельного флага не
// нужно, а «максимальная ширина ноль» не значит ничего осмысленного.
float ClampAxis(float value, float minV, float maxV) {
    if (minV > 0.0f && value < minV) value = minV;
    if (maxV > 0.0f && value > maxV) value = maxV;
    return value < 0.0f ? 0.0f : value;
}

// Соотношение сторон (§11). Применяется ПОСЛЕ всех остальных правил: иначе
// «ширина 50% и соотношение 16:9» дают два разных ответа в зависимости от
// порядка, и предсказать результат нельзя.
void ApplyAspect(const UITransform& t, glm::vec2& size) {
    if (t.Aspect == UIAspectMode::None || t.AspectRatio <= 0.0f) return;
    switch (t.Aspect) {
        case UIAspectMode::WidthFromHeight: size.x = size.y * t.AspectRatio; break;
        case UIAspectMode::HeightFromWidth: size.y = size.x / t.AspectRatio; break;
        case UIAspectMode::FitInside: {
            const float byW = size.x / t.AspectRatio;
            if (byW <= size.y) size.y = byW; else size.x = size.y * t.AspectRatio;
            break;
        }
        case UIAspectMode::FillOutside: {
            const float byW = size.x / t.AspectRatio;
            if (byW >= size.y) size.y = byW; else size.x = size.y * t.AspectRatio;
            break;
        }
        default: break;
    }
}

bool IsMainHorizontal(UILayout::Mode m) {
    return m == UILayout::Mode::Horizontal || m == UILayout::Mode::Wrap ||
           m == UILayout::Mode::Grid;
}

} // namespace

// ---------------------------------------------------------------------------
// Прямоугольник узла внутри родителя. Одна функция, которой пользуются и
// решатель, и редактор, и тесты (§114: на «откуда взялось положение» обязан
// быть один ответ).
// ---------------------------------------------------------------------------
UIRect UIResolveTransform(const UITransform& t, const UIRect& parent, glm::vec2 content) {
    const UIRect anchor = UIAnchorRect(t, parent);

    glm::vec2 size = t.Size;
    // Ширина.
    switch (t.WidthMode) {
        case UISizeMode::Fixed: size.x = t.Size.x; break;
        case UISizeMode::Percent: size.x = parent.w * t.Percent.x * 0.01f; break;
        case UISizeMode::Content: size.x = content.x; break;
        case UISizeMode::Stretch: size.x = anchor.w - t.Margin.Horizontal(); break;
    }
    // Высота.
    switch (t.HeightMode) {
        case UISizeMode::Fixed: size.y = t.Size.y; break;
        case UISizeMode::Percent: size.y = parent.h * t.Percent.y * 0.01f; break;
        case UISizeMode::Content: size.y = content.y; break;
        case UISizeMode::Stretch: size.y = anchor.h - t.Margin.Vertical(); break;
    }

    size.x = ClampAxis(size.x, t.MinSize.x, t.MaxSize.x);
    size.y = ClampAxis(size.y, t.MinSize.y, t.MaxSize.y);
    ApplyAspect(t, size);
    size.x = ClampAxis(size.x, t.MinSize.x, t.MaxSize.x);
    size.y = ClampAxis(size.y, t.MinSize.y, t.MaxSize.y);

    // Положение. Растянутая ось прижимается полями к области якорей; не
    // растянутая ставится опорной точкой на середину области якорей плюс
    // смещение. Именно на середину: у точечного якоря область вырождена в
    // точку, и это ровно она.
    float x, y;
    if (t.WidthMode == UISizeMode::Stretch) {
        x = anchor.x + t.Margin.L;
    } else {
        x = anchor.x + anchor.w * 0.5f + t.Offset.x - size.x * t.Pivot.x;
    }
    if (t.HeightMode == UISizeMode::Stretch) {
        y = anchor.y + t.Margin.T;
    } else {
        y = anchor.y + anchor.h * 0.5f + t.Offset.y - size.y * t.Pivot.y;
    }
    return UIRect{x + t.Translate.x, y + t.Translate.y, size.x, size.y};
}

glm::vec2 UIOffsetForTopLeft(const UITransform& t, glm::vec2 topLeft, glm::vec2 size,
                             const UIRect& parent) {
    const UIRect anchor = UIAnchorRect(t, parent);
    // Точный обратный ход к UIResolveTransform. Складывать дельту с Offset
    // напрямую нельзя: у якорей справа и снизу область якорей смещена, и
    // «подвинуть на 10 пикселей» перестаёт быть «прибавить 10».
    const float x = topLeft.x - t.Translate.x - (anchor.x + anchor.w * 0.5f) + size.x * t.Pivot.x;
    const float y = topLeft.y - t.Translate.y - (anchor.y + anchor.h * 0.5f) + size.y * t.Pivot.y;
    return {x, y};
}

// ---------------------------------------------------------------------------
// Раскладка детей внутри контейнера.
// ---------------------------------------------------------------------------
glm::vec2 UIApplyLayout(const UILayout& layout, const UIRect& container,
                        std::vector<UILayoutSlot>& slots) {
    const UIRect box = UIDeflate(container, layout.Padding);
    if (slots.empty()) return glm::vec2(0.0f);

    const int n = (int)slots.size();

    auto alignMain = [&](float total, float free, int count, float& start, float& gap) {
        (void)total;
        start = 0.0f;
        gap = 0.0f;
        switch (layout.Main) {
            case UIAlign::Center: start = free * 0.5f; break;
            case UIAlign::End: start = free; break;
            case UIAlign::SpaceBetween:
                if (count > 1) gap = free / (float)(count - 1);
                else start = free * 0.5f;
                break;
            case UIAlign::SpaceAround:
                gap = count > 0 ? free / (float)count : 0.0f;
                start = gap * 0.5f;
                break;
            case UIAlign::SpaceEvenly:
                gap = free / (float)(count + 1);
                start = gap;
                break;
            default: break; // Start, Stretch
        }
    };

    auto crossPos = [&](float boxStart, float boxSize, float itemSize) {
        switch (layout.Cross) {
            case UIAlign::Center: return boxStart + (boxSize - itemSize) * 0.5f;
            case UIAlign::End: return boxStart + boxSize - itemSize;
            default: return boxStart;
        }
    };

    switch (layout.Kind) {
        case UILayout::Mode::None:
            return glm::vec2(0.0f);

        case UILayout::Mode::Overlay: {
            // Все дети занимают весь контейнер: слои поверх друг друга.
            for (auto& s : slots) {
                s.Pos = {box.x, box.y};
                s.Size = {box.w, box.h};
                s.StretchMain = s.StretchCross = true;
            }
            return {box.w, box.h};
        }

        case UILayout::Mode::Center: {
            for (auto& s : slots) {
                s.Pos = {box.x + (box.w - s.Size.x) * 0.5f, box.y + (box.h - s.Size.y) * 0.5f};
            }
            glm::vec2 used(0.0f);
            for (const auto& s : slots) used = glm::max(used, s.Size);
            return used;
        }

        case UILayout::Mode::Aspect: {
            // Один ребёнок, вписанный по соотношению сторон. Остальные — тоже
            // вписываются: «Aspect с двумя детьми» не должен вести себя
            // непредсказуемо.
            for (auto& s : slots) {
                const float ratio = s.Size.y > 0.0f ? s.Size.x / s.Size.y : 1.0f;
                float w = box.w, h = box.w / std::max(0.0001f, ratio);
                if (h > box.h) { h = box.h; w = h * ratio; }
                s.Size = {w, h};
                s.Pos = {box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f};
            }
            return {box.w, box.h};
        }

        case UILayout::Mode::Stack: {
            glm::vec2 used(0.0f);
            for (int i = 0; i < n; ++i) {
                slots[(size_t)i].Pos = {box.x + layout.StackOffset.x * (float)i,
                                        box.y + layout.StackOffset.y * (float)i};
                used = glm::max(used, slots[(size_t)i].Pos + slots[(size_t)i].Size -
                                          glm::vec2(box.x, box.y));
            }
            return used;
        }

        case UILayout::Mode::Grid: {
            const int cols = std::max(1, layout.Columns);
            const int rows = (n + cols - 1) / cols;
            glm::vec2 cell = layout.CellSize;
            if (cell.x <= 0.0f)
                cell.x = (box.w - layout.Gap.x * (float)(cols - 1)) / (float)cols;
            if (cell.y <= 0.0f) {
                // Высота ячейки — по самому высокому ребёнку, а не по высоте
                // контейнера: сетка обязана уметь расти вниз (инвентарь).
                float maxH = 0.0f;
                for (const auto& s : slots) maxH = std::max(maxH, s.Size.y);
                cell.y = maxH > 0.0f ? maxH : (box.h - layout.Gap.y * (float)(rows - 1)) /
                                                  std::max(1.0f, (float)rows);
            }
            for (int i = 0; i < n; ++i) {
                const int c = i % cols, r = i / cols;
                UILayoutSlot& s = slots[(size_t)i];
                s.Pos = {box.x + (cell.x + layout.Gap.x) * (float)c,
                         box.y + (cell.y + layout.Gap.y) * (float)r};
                if (layout.Cross == UIAlign::Stretch) {
                    s.Size = cell;
                    s.StretchMain = s.StretchCross = true;
                } else {
                    s.Pos.x += (cell.x - s.Size.x) * 0.5f;
                    s.Pos.y += (cell.y - s.Size.y) * 0.5f;
                }
            }
            return {cell.x * (float)cols + layout.Gap.x * (float)(cols - 1),
                    cell.y * (float)rows + layout.Gap.y * (float)(rows - 1)};
        }

        case UILayout::Mode::Wrap: {
            // Ряд с переносом. Строка закрывается, когда следующий ребёнок не
            // влезает; ребёнок шире контейнера занимает свою строку целиком —
            // иначе он ушёл бы за край и никогда не был бы виден.
            float x = 0.0f, y = 0.0f, lineH = 0.0f, usedW = 0.0f;
            for (auto& s : slots) {
                if (x > 0.0f && x + s.Size.x > box.w) {
                    x = 0.0f;
                    y += lineH + layout.Gap.y;
                    lineH = 0.0f;
                }
                s.Pos = {box.x + x, box.y + y};
                x += s.Size.x + layout.Gap.x;
                lineH = std::max(lineH, s.Size.y);
                usedW = std::max(usedW, x - layout.Gap.x);
            }
            return {usedW, y + lineH};
        }

        default: break; // Horizontal / Vertical — ниже
    }

    // --- Ряд и столбец -------------------------------------------------------
    const bool horizontal = IsMainHorizontal(layout.Kind);
    const float boxMain = horizontal ? box.w : box.h;
    const float boxCross = horizontal ? box.h : box.w;
    const float gap = horizontal ? layout.Gap.x : layout.Gap.y;

    // Дети, тянущиеся вдоль основной оси, делят между собой остаток. Именно
    // остаток, а не «поровну весь контейнер»: иначе фиксированные соседи
    // выдавливаются за край.
    float fixedTotal = 0.0f;
    int stretchCount = 0;
    for (const auto& s : slots) {
        if (s.StretchMain) ++stretchCount;
        else fixedTotal += horizontal ? s.Size.x : s.Size.y;
    }
    const float gapsTotal = gap * (float)(n - 1);
    float free = boxMain - fixedTotal - gapsTotal;
    if (stretchCount > 0) {
        const float share = std::max(0.0f, free) / (float)stretchCount;
        for (auto& s : slots)
            if (s.StretchMain) (horizontal ? s.Size.x : s.Size.y) = share;
        free = 0.0f;
    }

    float start = 0.0f, extraGap = 0.0f;
    alignMain(fixedTotal + gapsTotal, std::max(0.0f, free), n, start, extraGap);

    float cursor = start;
    float usedMain = 0.0f, usedCross = 0.0f;
    for (auto& s : slots) {
        const float mainSize = horizontal ? s.Size.x : s.Size.y;
        float crossSize = horizontal ? s.Size.y : s.Size.x;
        if (layout.Cross == UIAlign::Stretch || s.StretchCross) {
            crossSize = boxCross;
            (horizontal ? s.Size.y : s.Size.x) = crossSize;
            s.StretchCross = true;
        }
        const float mainPos = (horizontal ? box.x : box.y) + cursor;
        const float crossStart = horizontal ? box.y : box.x;
        const float crossP = crossPos(crossStart, boxCross, crossSize);
        s.Pos = horizontal ? glm::vec2(mainPos, crossP) : glm::vec2(crossP, mainPos);
        cursor += mainSize + gap + extraGap;
        usedMain = cursor - gap - extraGap - start;
        usedCross = std::max(usedCross, crossSize);
    }
    return horizontal ? glm::vec2(usedMain, usedCross) : glm::vec2(usedCross, usedMain);
}

// ---------------------------------------------------------------------------
// Решатель
// ---------------------------------------------------------------------------

void UILayoutSolver::Solve(UIDocument& doc, const UIContext& ctx) {
    const auto t0 = std::chrono::steady_clock::now();

    m_ctx = &ctx;
    m_nodes.clear();
    m_index.clear();
    m_measure.clear();
    m_masks.Clear();
    m_stats = UILayoutStats{};
    m_sortCounter = 0;

    m_scale = doc.ScaleFor(ctx.ScreenPixels);
    m_viewport = doc.ViewportFor(ctx.ScreenPixels);

    const int rootMask = m_masks.Root();
    m_nodes.reserve(doc.NodeCount());

    // Корни сортируются по слою и порядку — как и любые соседи. Отдельного
    // правила для корней нет намеренно (§26: никаких скрытых правил).
    std::vector<UINodeId> roots = doc.Roots();
    std::stable_sort(roots.begin(), roots.end(), [&](UINodeId a, UINodeId b) {
        const UINode* na = doc.Find(a);
        const UINode* nb = doc.Find(b);
        if (!na || !nb) return false;
        if (na->Layer != nb->Layer) return na->Layer < nb->Layer;
        return na->Order < nb->Order;
    });

    for (UINodeId id : roots) {
        UINode* n = doc.Find(id);
        if (!n) continue;
        ArrangeNode(doc, *n, m_viewport, -1, rootMask, 1.0f, true, true, glm::mat3(1.0f), 0, 0);
    }

    m_stats.Nodes = (int)m_nodes.size();
    m_stats.MaskStates = (int)m_masks.StateCount();
    for (const auto& r : m_nodes) {
        if (r.Visible && !r.Culled) ++m_stats.Visible;
        if (r.Culled) ++m_stats.Culled;
    }
    const auto t1 = std::chrono::steady_clock::now();
    m_stats.LayoutMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    doc.ClearDirty();
}

glm::vec2 UILayoutSolver::MeasureNode(UIDocument& doc, UINode& node, glm::vec2 available) {
    auto it = m_measure.find(node.Id);
    if (it != m_measure.end() && it->second.Valid) return it->second.Size;

    ++m_stats.MeasurePasses;
    glm::vec2 content(0.0f);

    // Вклад компонентов: текст просит место под строки, картинка — под свой
    // размер. Компонент, которому размер безразличен, возвращает ноль и ничего
    // не стоит (§130).
    for (const auto& c : node.Components) {
        const glm::vec2 want = c->Measure(*m_ctx, node, available);
        content = glm::max(content, want);
    }

    if (const UILayout* layout = node.Get<UILayout>()) {
        if (layout->Kind != UILayout::Mode::None && (layout->FitWidth || layout->FitHeight)) {
            const glm::vec2 inner = MeasureContainer(doc, node, *layout, available);
            if (layout->FitWidth) content.x = std::max(content.x, inner.x);
            if (layout->FitHeight) content.y = std::max(content.y, inner.y);
        }
    }

    const UITransform* t = node.Get<UITransform>();
    if (t) {
        content.x = ClampAxis(content.x, t->MinSize.x, t->MaxSize.x);
        content.y = ClampAxis(content.y, t->MinSize.y, t->MaxSize.y);
    }

    MeasureCache& cache = m_measure[node.Id];
    cache.Size = content;
    cache.Valid = true;
    return content;
}

glm::vec2 UILayoutSolver::MeasureContainer(UIDocument& doc, UINode& node, const UILayout& layout,
                                           glm::vec2 available) {
    // Размер контейнера по содержимому считается ТОЙ ЖЕ раскладкой, что и
    // расстановка. Второй, «упрощённой» формулы здесь нет намеренно: она
    // разошлась бы с настоящей на первом же выравнивании.
    std::vector<UILayoutSlot> slots;
    slots.reserve(node.Children.size());
    for (UINodeId cid : node.Children) {
        UINode* child = doc.Find(cid);
        if (!child || !child->Enabled) continue;
        const UITransform* ct = child->Get<UITransform>();
        if (ct && ct->IgnoreLayout) continue;

        const glm::vec2 childContent = MeasureNode(doc, *child, available);
        UILayoutSlot s;
        if (ct) {
            const UIRect fake{0.0f, 0.0f, available.x, available.y};
            const UIRect r = UIResolveTransform(*ct, fake, childContent);
            s.Size = {r.w, r.h};
            s.StretchMain = IsMainHorizontal(layout.Kind)
                                ? ct->WidthMode == UISizeMode::Stretch
                                : ct->HeightMode == UISizeMode::Stretch;
            s.StretchCross = IsMainHorizontal(layout.Kind)
                                 ? ct->HeightMode == UISizeMode::Stretch
                                 : ct->WidthMode == UISizeMode::Stretch;
        } else {
            s.Size = childContent;
        }
        slots.push_back(s);
    }
    // Растягивающиеся дети не задают размер контейнера: иначе «панель по
    // содержимому со строкой во всю ширину» уходит в бесконечность.
    for (auto& s : slots) {
        if (s.StretchMain) s.Size = glm::vec2(0.0f);
    }
    const UIRect container{0.0f, 0.0f, available.x, available.y};
    const glm::vec2 used = UIApplyLayout(layout, container, slots);
    return used + glm::vec2(layout.Padding.Horizontal(), layout.Padding.Vertical());
}

void UILayoutSolver::ArrangeNode(UIDocument& doc, UINode& node, const UIRect& parentRect,
                                 int parentIndex, int maskState, float opacity, bool visible,
                                 bool enabled, const glm::mat3& parentWorld, int depth,
                                 uint32_t layerBase) {
    if (!node.Enabled) {
        // Выключенный узел не участвует ни в чём — вместе с поддеревом. Именно
        // «не участвует», а не «невидим»: невидимый может ловить мышь, а
        // выключенного нет вовсе.
        return;
    }

    const UITransform* t = node.Get<UITransform>();
    static const UITransform kDefault; // узел без прямоугольника — сломанные
                                       // данные; безопасный откат вместо падения
    if (!t) t = &kDefault;

    const glm::vec2 content = MeasureNode(doc, node, {parentRect.w, parentRect.h});
    UIRect rect = UIResolveTransform(*t, parentRect, content);

    // Раскладка родителя уже положила прямоугольник в LayoutOverride — тогда
    // якоря не спрашиваем (кроме узлов с IgnoreLayout, которых родитель не
    // трогал вовсе).
    if (m_pendingRect.Valid) {
        rect = m_pendingRect.Rect;
        m_pendingRect.Valid = false;
    }

    // Экранные пиксели: раскладка идёт в ЛОГИЧЕСКИХ единицах холста, а всё
    // наружу отдаётся в пикселях. Перевод в одном месте — здесь.
    UIRect screen{rect.x * m_scale, rect.y * m_scale, rect.w * m_scale, rect.h * m_scale};

    const int index = (int)m_nodes.size();
    m_nodes.emplace_back();
    UIResolvedNode& out = m_nodes.back();
    out.Id = node.Id;
    out.Parent = node.Parent;
    out.ParentIndex = parentIndex;
    out.Depth = depth;
    out.Rect = screen;
    out.Measured = content;
    out.Scale = m_scale;
    out.Opacity = opacity * UIClamp01(node.Opacity);
    out.Blend = node.Blend;
    out.Visible = visible && node.Visible && !node.EditorHidden;
    out.Enabled = enabled;
    out.Layer = node.Layer;

    glm::mat3 world = parentWorld;
    const glm::mat3 local = UILocalMatrix(*t, screen);
    const bool hasLocal = local != glm::mat3(1.0f);
    if (hasLocal) world = parentWorld * local;
    out.World = world;
    out.Transformed = world != glm::mat3(1.0f);

    // Ключ сортировки (§26): слой → порядок → место в дереве. Ровно три ступени
    // и ни одной скрытой. Порядковый счётчик обхода даёт «место в дереве»
    // бесплатно и делает порядок устойчивым.
    const uint64_t layerKey = (uint64_t)(uint32_t)(node.Layer + 0x40000000);
    const uint64_t orderKey = (uint64_t)(uint32_t)(node.Order + 0x40000000);
    out.SortKey = (layerKey << 40) | ((orderKey & 0xFFFFF) << 20) | (m_sortCounter++ & 0xFFFFF);
    (void)layerBase;

    // Маска узла добавляется к состоянию предков и действует на ПОТОМКОВ, а не
    // на сам узел: панель с маской рисует свою подложку целиком, а режет
    // содержимое (§31).
    int childMask = maskState;
    if (const UIMask* mask = node.Get<UIMask>()) {
        UIMaskEntry e;
        e.Form = mask->Form;
        e.Mode = mask->Mode;
        e.Rect = UIDeflate(screen, UIEdges(mask->Padding.L * m_scale, mask->Padding.T * m_scale,
                                           mask->Padding.R * m_scale, mask->Padding.B * m_scale));
        e.Radius = UICorners(mask->Radius.TL * m_scale, mask->Radius.TR * m_scale,
                             mask->Radius.BR * m_scale, mask->Radius.BL * m_scale);
        e.Softness = mask->Softness * m_scale;
        e.Invert = mask->Invert;
        e.Source = mask->Source;
        e.GradientAngle = mask->GradientAngle;
        e.GradientStart = mask->GradientStart;
        e.GradientEnd = mask->GradientEnd;
        e.AffectsHitTest = mask->AffectsHitTest;
        e.Owner = node.Id;
        if (mask->Form == UIMask::Shape::Texture && m_ctx->Textures && !mask->TexturePath.empty())
            e.Tex = m_ctx->Textures->Get(mask->TexturePath);
        if (!mask->ShowOutside) childMask = m_masks.Push(maskState, e);
    }

    const UIMaskState& ms = m_masks.State(maskState);
    out.MaskState = maskState;
    out.Clipped = ms.HasScissor;
    out.Clip = ms.HasScissor ? ms.Scissor
                             : UIRect{0.0f, 0.0f, m_ctx->ScreenPixels.x, m_ctx->ScreenPixels.y};

    // Отсечение (§89): целиком за краем экрана или за маской — не готовим
    // вовсе. Повёрнутые узлы проверяются по охватывающему прямоугольнику.
    if (!out.Transformed) {
        const UIRect screenBox{0.0f, 0.0f, m_ctx->ScreenPixels.x, m_ctx->ScreenPixels.y};
        UIRect visibleArea = ms.HasScissor ? UIIntersectRect(ms.Scissor, screenBox) : screenBox;
        if (!UIRectValid(UIIntersectRect(screen, visibleArea))) out.Culled = true;
    }

    if (const UIInteraction* ia = node.Get<UIInteraction>())
        out.HitTestable = ia->Enabled && enabled && ia->Hit != UIHitShape::None;

    m_index[node.Id] = index;

    // Дети. Прокрутка сдвигает область, в которой они раскладываются, — это и
    // есть вся прокрутка: содержимое не «перерисовывается со смещением», оно
    // РАСКЛАДЫВАЕТСЯ в сдвинутой области, поэтому маска, попадание курсором и
    // отсечение работают сами собой.
    UIRect childArea{rect.x, rect.y, rect.w, rect.h};
    UIScrollView* scroll = node.Get<UIScrollView>();
    if (scroll) {
        childArea.x -= scroll->Horizontal ? scroll->Offset.x : 0.0f;
        childArea.y -= scroll->Vertical ? scroll->Offset.y : 0.0f;
    }
    const size_t firstChildIndex = m_nodes.size();
    ArrangeChildren(doc, node, childArea, index, childMask, out.Opacity, out.Visible,
                    enabled && node.Enabled, world, depth + 1);

    if (scroll) {
        // Насколько содержимое БОЛЬШЕ окна — то, до чего можно докрутить.
        // Считается по фактическим прямоугольникам детей: «сколько там строк»
        // прокрутка знать не должна и не знает (§90).
        UIRect content{};
        for (size_t i = firstChildIndex; i < m_nodes.size(); ++i)
            content = UIUnionRect(content, m_nodes[i].Rect);
        const UIRect self = m_nodes[(size_t)index].Rect;
        const float extraX = std::max(0.0f, UIRight(content) - UIRight(self));
        const float extraY = std::max(0.0f, UIBottom(content) - UIBottom(self));
        const float s = m_scale > 0.0001f ? m_scale : 1.0f;
        scroll->ContentSize = {extraX / s + scroll->Offset.x, extraY / s + scroll->Offset.y};
    }
}

void UILayoutSolver::ArrangeChildren(UIDocument& doc, UINode& node, const UIRect& contentRect,
                                     int selfIndex, int maskState, float opacity, bool visible,
                                     bool enabled, const glm::mat3& world, int depth) {
    if (node.Children.empty()) return;

    // Порядок среди соседей: слой → порядок → место в дереве.
    std::vector<UINodeId> kids = node.Children;
    std::stable_sort(kids.begin(), kids.end(), [&](UINodeId a, UINodeId b) {
        const UINode* na = doc.Find(a);
        const UINode* nb = doc.Find(b);
        if (!na || !nb) return false;
        if (na->Layer != nb->Layer) return na->Layer < nb->Layer;
        return na->Order < nb->Order;
    });

    const UILayout* layout = node.Get<UILayout>();
    if (!layout || layout->Kind == UILayout::Mode::None) {
        for (UINodeId cid : kids) {
            UINode* child = doc.Find(cid);
            if (!child) continue;
            ArrangeNode(doc, *child, contentRect, selfIndex, maskState, opacity, visible, enabled,
                        world, depth, 0);
        }
        return;
    }

    // Контейнер: сначала считаем места, потом раскладываем детей по ним.
    std::vector<UINodeId> managed;
    std::vector<UILayoutSlot> slots;
    for (UINodeId cid : kids) {
        UINode* child = doc.Find(cid);
        if (!child || !child->Enabled) continue;
        const UITransform* ct = child->Get<UITransform>();
        if (ct && ct->IgnoreLayout) continue;

        const glm::vec2 childContent = MeasureNode(doc, *child, {contentRect.w, contentRect.h});
        UILayoutSlot s;
        if (ct) {
            const UIRect r = UIResolveTransform(*ct, contentRect, childContent);
            s.Size = {r.w, r.h};
            s.StretchMain = IsMainHorizontal(layout->Kind)
                                ? ct->WidthMode == UISizeMode::Stretch
                                : ct->HeightMode == UISizeMode::Stretch;
            s.StretchCross = IsMainHorizontal(layout->Kind)
                                 ? ct->HeightMode == UISizeMode::Stretch
                                 : ct->WidthMode == UISizeMode::Stretch;
        } else {
            s.Size = childContent;
        }
        slots.push_back(s);
        managed.push_back(cid);
    }
    if (layout->Reverse) {
        std::reverse(managed.begin(), managed.end());
        std::reverse(slots.begin(), slots.end());
    }

    UIApplyLayout(*layout, contentRect, slots);

    if (layout->Reverse) {
        std::reverse(managed.begin(), managed.end());
        std::reverse(slots.begin(), slots.end());
    }

    size_t slotIndex = 0;
    for (UINodeId cid : kids) {
        UINode* child = doc.Find(cid);
        if (!child) continue;
        const UITransform* ct = child->Get<UITransform>();
        const bool managedChild = child->Enabled && (!ct || !ct->IgnoreLayout);
        if (managedChild && slotIndex < managed.size() && managed[slotIndex] == cid) {
            const UILayoutSlot& s = slots[slotIndex++];
            // Прямоугольник, назначенный раскладкой, передаётся узлу напрямую:
            // спрашивать его якоря второй раз означало бы, что раскладка
            // «предлагает», а якорь «решает» — то есть непредсказуемо.
            m_pendingRect.Valid = true;
            m_pendingRect.Rect = UIRect{s.Pos.x, s.Pos.y, s.Size.x, s.Size.y};
        }
        ArrangeNode(doc, *child, contentRect, selfIndex, maskState, opacity, visible, enabled,
                    world, depth, 0);
        m_pendingRect.Valid = false;
    }
}

int UILayoutSolver::IndexOf(UINodeId id) const {
    auto it = m_index.find(id);
    return it == m_index.end() ? -1 : it->second;
}

const UIResolvedNode* UILayoutSolver::Get(UINodeId id) const {
    const int i = IndexOf(id);
    return i < 0 ? nullptr : &m_nodes[(size_t)i];
}

bool UILayoutSolver::RectOf(UINodeId id, UIRect& out) const {
    const UIResolvedNode* r = Get(id);
    if (!r) return false;
    out = r->Rect;
    return true;
}

} // namespace sage::ui
