#include "sage/ui/debug/UIDebug.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/effects/UIEffect.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMask.h"
#include "sage/ui/serialization/UIPrefab.h"

namespace sage::ui {

namespace {

const char* SizeModeName(UISizeMode m) {
    switch (m) {
        case UISizeMode::Fixed: return "Fixed";
        case UISizeMode::Percent: return "Percent";
        case UISizeMode::Content: return "Content";
        default: return "Stretch";
    }
}

std::string Num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string RectText(const UIRect& r) {
    return "(" + Num(r.x) + ", " + Num(r.y) + ", " + Num(r.w) + " x " + Num(r.h) + ")";
}

void Outline(UIRenderList& list, const UIRect& r, const UIColor& color, float thickness,
             uint64_t sortKey) {
    UIRenderCommand& c = list.Add();
    c.Kind = UIPrimitive::Border;
    c.Rect = r;
    c.Color = color;
    c.Thickness = thickness;
    // Отладочные слои идут ПОВЕРХ всего: ключ сортировки максимальный.
    c.SortKey = sortKey;
}

} // namespace

std::string UIProfile::Summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "узлов %d (видно %d, отброшено %d) | команд %d, батчей %d, квадов %d, глифов %d\n"
                  "маски %d, эффекты %d, цели %d | раскладка %.2f мс, подготовка %.2f мс, "
                  "ввод %.2f мс, всего %.2f мс",
                  Layout.Nodes, Layout.Visible, Layout.Culled, Render.Commands, Render.Batches,
                  Render.Quads, Render.Glyphs, Render.MaskPasses, Render.EffectPasses,
                  Render.RenderTargets, Layout.LayoutMs, Render.PrepareMs, InputMs, TotalMs);
    return buf;
}

void UIAppendDebugOverlay(const UIDocument& doc, const UILayoutSolver& layout,
                          const UIContext& ctx, UIRenderList& list) {
    const uint64_t topKey = ~0ull;
    for (const UIResolvedNode& r : layout.Nodes()) {
        const UINode* node = doc.Find(r.Id);
        if (!node) continue;

        if (ctx.Debug & UIDebug_Bounds) {
            const UIColor c = r.Culled ? UIColor(1.0f, 0.3f, 0.3f, 0.5f)
                                       : UIColor(0.3f, 0.9f, 1.0f, 0.45f);
            Outline(list, r.Rect, c, 1.0f, topKey);
        }
        if (ctx.Debug & UIDebug_Layers) {
            // Слой показан цветом рамки: разбираться в числах поверх кадра
            // невозможно, а «этот выше того» видно сразу.
            const float hue = (float)((node->Layer % 8) + 1) / 9.0f;
            Outline(list, r.Rect, UIColor(hue, 1.0f - hue, 0.6f, 0.6f), 2.0f, topKey);
        }
        if ((ctx.Debug & UIDebug_Anchors) && r.ParentIndex >= 0) {
            const UITransform* t = node->Get<UITransform>();
            const UIResolvedNode& parent = layout.Nodes()[(size_t)r.ParentIndex];
            if (t) {
                const UIRect a{parent.Rect.x + parent.Rect.w * t->AnchorMin.x,
                               parent.Rect.y + parent.Rect.h * t->AnchorMin.y,
                               parent.Rect.w * (t->AnchorMax.x - t->AnchorMin.x),
                               parent.Rect.h * (t->AnchorMax.y - t->AnchorMin.y)};
                Outline(list, a, UIColor(1.0f, 0.85f, 0.2f, 0.6f), 1.0f, topKey);
            }
        }
        if (ctx.Debug & UIDebug_Pivots) {
            if (const UITransform* t = node->Get<UITransform>()) {
                const glm::vec2 p{r.Rect.x + r.Rect.w * t->Pivot.x,
                                  r.Rect.y + r.Rect.h * t->Pivot.y};
                UIRenderCommand& c = list.Add();
                c.Kind = UIPrimitive::Rect;
                c.Rect = {p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f};
                c.Radius = UICorners(3.0f);
                c.Color = UIColor(1.0f, 0.4f, 0.8f, 0.9f);
                c.SortKey = topKey;
            }
        }
        if ((ctx.Debug & UIDebug_Masks) && node->Has<UIMask>())
            Outline(list, r.Rect, UIColor(0.6f, 0.3f, 1.0f, 0.8f), 2.0f, topKey);
        if ((ctx.Debug & UIDebug_Clip) && r.Clipped)
            Outline(list, r.Clip, UIColor(1.0f, 0.5f, 0.1f, 0.35f), 1.0f, topKey);
        if ((ctx.Debug & UIDebug_HitAreas) && r.HitTestable) {
            const UIInteraction* ia = node->Get<UIInteraction>();
            const UIRect hit = ia ? UIInflate(r.Rect, UIEdges(ia->HitPadding.L * r.Scale,
                                                              ia->HitPadding.T * r.Scale,
                                                              ia->HitPadding.R * r.Scale,
                                                              ia->HitPadding.B * r.Scale))
                                  : r.Rect;
            Outline(list, hit, UIColor(0.2f, 1.0f, 0.4f, 0.5f), 1.0f, topKey);
        }
    }
    if (ctx.Debug & UIDebug_Batches) {
        // Границы батчей: разработчик обязан видеть, где интерфейс рвёт вызов
        // рисования, а не догадываться по счётчику (§109).
        for (const UIRenderBatch& b : list.Batches()) {
            if (b.Count <= 0) continue;
            const UIRenderCommand& first = list.Commands()[(size_t)b.First];
            Outline(list, first.Rect, UIColor(1.0f, 1.0f, 1.0f, 0.25f), 1.0f, topKey);
        }
    }
}

std::string UIExplainPosition(const UIDocument& doc, const UILayoutSolver& layout, UINodeId id) {
    const UINode* node = doc.Find(id);
    const UIResolvedNode* r = layout.Get(id);
    if (!node) return "узла нет";
    if (!r) return "узел не участвует в раскладке (выключен сам или предок)";
    const UITransform* t = node->Get<UITransform>();
    if (!t) return "у узла нет прямоугольника";

    std::string out = node->Name + ": " + RectText(r->Rect) + " (пиксели экрана)\n";
    // Область, внутри которой узел расположен: у корня это холст, у остальных —
    // родитель. Дальше цепочка одна и та же — иначе «откуда взялось положение»
    // имело бы два разных ответа в зависимости от глубины.
    UIRect box = layout.Viewport();
    if (r->ParentIndex >= 0) {
        box = layout.Nodes()[(size_t)r->ParentIndex].Rect;
        out += "  родитель: " + RectText(box) + "\n";
    } else {
        out += "  корень: область холста " + RectText(box) + "\n";
    }
    const UIRect anchor{box.x + box.w * t->AnchorMin.x, box.y + box.h * t->AnchorMin.y,
                        box.w * (t->AnchorMax.x - t->AnchorMin.x),
                        box.h * (t->AnchorMax.y - t->AnchorMin.y)};
    out += "  якоря: " + Num(t->AnchorMin.x) + "," + Num(t->AnchorMin.y) + " .. " +
           Num(t->AnchorMax.x) + "," + Num(t->AnchorMax.y) + " → " + RectText(anchor) + "\n";
    out += "  ширина: " + std::string(SizeModeName(t->WidthMode));
    if (t->WidthMode == UISizeMode::Content) out += " (содержимое " + Num(r->Measured.x) + ")";
    if (t->WidthMode == UISizeMode::Percent) out += " (" + Num(t->Percent.x) + "%)";
    out += "\n  высота: " + std::string(SizeModeName(t->HeightMode));
    if (t->HeightMode == UISizeMode::Content) out += " (содержимое " + Num(r->Measured.y) + ")";
    out += "\n  смещение: " + Num(t->Offset.x) + ", " + Num(t->Offset.y) +
           "; опорная точка: " + Num(t->Pivot.x) + ", " + Num(t->Pivot.y) + "\n";
    if (const UINode* parent = doc.Find(node->Parent)) {
        if (const UILayout* pl = parent->Get<UILayout>()) {
            if (pl->Kind != UILayout::Mode::None && !t->IgnoreLayout)
                out += "  ВНИМАНИЕ: положение задаёт раскладка родителя, а не якоря\n";
        }
    }
    out += "  масштаб холста: " + Num(r->Scale);
    return out;
}

std::string UIExplainVisibility(const UIDocument& doc, const UILayoutSolver& layout, UINodeId id) {
    const UINode* node = doc.Find(id);
    if (!node) return "узла нет";
    const UIResolvedNode* r = layout.Get(id);
    std::string out = node->Name + ":\n";
    if (!node->Enabled) return out + "  ВЫКЛЮЧЕН — не участвует ни в чём";
    if (!r) return out + "  не в раскладке: выключен кто-то из предков";

    out += std::string("  собственная видимость: ") + (node->Visible ? "да" : "НЕТ") + "\n";
    out += "  прозрачность своя: " + Num(node->Opacity) + ", итоговая: " + Num(r->Opacity) + "\n";
    if (r->Opacity <= 0.001f) out += "  ПРИЧИНА: итоговая прозрачность равна нулю\n";
    if (!r->Visible) out += "  ПРИЧИНА: невидим сам или кто-то из предков\n";
    if (r->Culled) out += "  ПРИЧИНА: целиком за краем экрана или маски\n";
    if (r->Clipped) out += "  окно обрезки: " + RectText(r->Clip) + "\n";
    if (!UIRectValid(r->Rect)) out += "  ПРИЧИНА: нулевой размер " + RectText(r->Rect) + "\n";
    if (r->MaskState != 0) {
        out += "  масок над узлом: " + std::to_string(layout.Masks().State(r->MaskState).Entries.size() +
                                                      (layout.Masks().State(r->MaskState).HasScissor ? 1 : 0)) +
               "\n";
    }
    bool drawable = false;
    for (const auto& c : node->Components) {
        const std::string& cid = c->Type().Id;
        if (cid == "fill" || cid == "image" || cid == "text" || cid == "shape" ||
            cid == "progress" || cid == "border" || cid == "range")
            drawable = true;
    }
    if (!drawable) out += "  ПРИЧИНА: у узла нет ни одного рисующего компонента\n";
    return out;
}

std::string UIExplainOrder(const UIDocument& doc, const UILayoutSolver& layout, UINodeId id) {
    const UINode* node = doc.Find(id);
    const UIResolvedNode* r = layout.Get(id);
    if (!node || !r) return "узла нет в раскладке";
    std::string out = node->Name + ":\n";
    out += "  слой: " + std::to_string(node->Layer) + "\n";
    out += "  порядок среди соседей: " + std::to_string(node->Order) + "\n";
    out += "  глубина в дереве: " + std::to_string(r->Depth) + "\n";
    out += "  итоговый ключ: " + std::to_string(r->SortKey) + "\n";
    out += "  правило: слой → порядок → место в дереве. Больше ключ — рисуется поверх.";
    return out;
}

std::string UIExplainEffects(const UIDocument& doc, UINodeId id) {
    const UINode* node = doc.Find(id);
    if (!node) return "узла нет";
    const UIEffects* fx = node->Get<UIEffects>();
    if (!fx || fx->Items.empty()) return node->Name + ": эффектов нет — ни одного лишнего прохода";
    std::string out = node->Name + ": стек эффектов (порядок значим)\n";
    int index = 0;
    for (const auto& e : fx->Items) {
        const char* stage = "";
        switch (e->Type().Stage) {
            case UIEffectStage::Modulate: stage = "множитель цвета, без прохода"; break;
            case UIEffectStage::Behind: stage = "геометрия под узлом"; break;
            case UIEffectStage::Front: stage = "геометрия поверх узла"; break;
            case UIEffectStage::Offscreen: stage = "ПРОМЕЖУТОЧНАЯ ЦЕЛЬ"; break;
        }
        out += "  " + std::to_string(index++) + ". " + e->Type().Title +
               (e->Enabled ? "" : " (выключен)") + " — " + stage + "\n";
    }
    if (fx->NeedsOffscreen()) out += "  итого: узлу нужна промежуточная цель рисования";
    return out;
}

std::string UIDumpTree(const UIDocument& doc, const UILayoutSolver* layout) {
    std::string out;
    for (UINodeId id : doc.Ordered()) {
        const UINode* n = doc.Find(id);
        if (!n) continue;
        int depth = 0;
        for (const UINode* p = doc.Find(n->Parent); p; p = doc.Find(p->Parent)) ++depth;
        out.append((size_t)depth * 2, ' ');
        out += n->Name;
        out += " [";
        for (size_t i = 0; i < n->Components.size(); ++i) {
            if (i) out += ", ";
            out += n->Components[i]->Type().Id;
        }
        out += "]";
        if (!n->Visible) out += " (скрыт)";
        if (!n->Enabled) out += " (выключен)";
        if (layout) {
            if (const UIResolvedNode* r = layout->Get(id)) out += " " + RectText(r->Rect);
        }
        out += "\n";
    }
    return out;
}

std::vector<UIValidationIssue> UIValidate(const UIDocument& doc) {
    std::vector<UIValidationIssue> issues;
    std::unordered_set<std::string> guids;

    for (UINodeId id : doc.Ordered()) {
        const UINode* n = doc.Find(id);
        if (!n) continue;

        if (n->Parent != kUIInvalidNode && !doc.Find(n->Parent))
            issues.push_back({id, "родитель не существует", true});
        // Цикл в иерархии — не «странная вёрстка», а бесконечный обход при
        // первой же отрисовке.
        int guard = 0;
        for (const UINode* p = doc.Find(n->Parent); p && guard < 512;
             p = doc.Find(p->Parent), ++guard) {
            if (p->Id == id) {
                issues.push_back({id, "цикл в иерархии", true});
                break;
            }
        }
        if (!n->Find("transform")) issues.push_back({id, "узел без прямоугольника", true});
        if (!n->Guid.empty() && !guids.insert(n->Guid).second)
            issues.push_back({id, "повторяющийся guid: " + n->Guid, false});

        for (UINodeId cid : n->Children) {
            const UINode* c = doc.Find(cid);
            if (!c) { issues.push_back({id, "ссылка на несуществующего ребёнка", true}); continue; }
            if (c->Parent != id) issues.push_back({cid, "ребёнок не признаёт родителя", true});
        }
        if (const UIMask* m = n->Get<UIMask>()) {
            if (m->Form == UIMask::Shape::Texture && m->TexturePath.empty())
                issues.push_back({id, "маска по картинке без картинки", false});
        }
        if (const UIPrefabInstance* p = n->Get<UIPrefabInstance>()) {
            if (p->Source.empty()) issues.push_back({id, "экземпляр префаба без источника", false});
        }
    }
    return issues;
}

} // namespace sage::ui
