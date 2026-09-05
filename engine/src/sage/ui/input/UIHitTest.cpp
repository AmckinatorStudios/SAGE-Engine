#include "sage/ui/input/UIHitTest.h"

#include <algorithm>
#include <cmath>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIShape.h"

namespace sage::ui {

namespace {

// Точка в координатах узла: для повёрнутого узла нужен обратный ход через его
// матрицу. Отдельной функцией, потому что этим же занимается и доставка
// события (LocalPointer), и обе обязаны отвечать одинаково.
glm::vec2 ToLocal(const UIResolvedNode& r, glm::vec2 point) {
    if (!r.Transformed) return point;
    const glm::mat3 inv = glm::inverse(r.World);
    const glm::vec3 p = inv * glm::vec3(point, 1.0f);
    return {p.x, p.y};
}

bool InsideRoundedRect(const UIRect& rect, const UICorners& radius, glm::vec2 p) {
    if (!UIContains(rect, p)) return false;
    const UICorners c = radius.Clamped(rect.w, rect.h);
    if (c.Empty()) return true;
    // Проверяем только четыре угловых четверти: в остальном прямоугольнике
    // ответ уже дан.
    struct Corner { glm::vec2 centre; float r; };
    const Corner corners[4] = {
        {{rect.x + c.TL, rect.y + c.TL}, c.TL},
        {{UIRight(rect) - c.TR, rect.y + c.TR}, c.TR},
        {{UIRight(rect) - c.BR, UIBottom(rect) - c.BR}, c.BR},
        {{rect.x + c.BL, UIBottom(rect) - c.BL}, c.BL},
    };
    const bool left = p.x < corners[0].centre.x;
    const bool right = p.x > corners[1].centre.x;
    const bool top = p.y < corners[0].centre.y;
    const bool bottom = p.y > corners[3].centre.y;
    int idx = -1;
    if (left && top) idx = 0;
    else if (right && top) idx = 1;
    else if (right && bottom) idx = 2;
    else if (left && bottom) idx = 3;
    if (idx < 0) return true;
    const Corner& k = corners[idx];
    if (k.r <= 0.0f) return true;
    return glm::length(p - k.centre) <= k.r;
}

bool InsidePolygon(const std::vector<glm::vec2>& pts, glm::vec2 p) {
    // Классический алгоритм чётности пересечений. Работает для любых
    // многоугольников, включая невыпуклые: карта, шестиугольник, стрелка.
    bool inside = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const glm::vec2& a = pts[i];
        const glm::vec2& b = pts[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x))
            inside = !inside;
    }
    return inside;
}

} // namespace

bool UIHitNode(const UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
               UINodeId id, glm::vec2 point) {
    const UIResolvedNode* r = layout.Get(id);
    const UINode* node = doc.Find(id);
    if (!r || !node) return false;
    const UIInteraction* ia = node->Get<UIInteraction>();
    if (!ia || !ia->Enabled) return false;
    if (ia->Hit == UIHitShape::None) return false;
    if (!r->Enabled) return false;

    // §52: невидимый или почти прозрачный узел мышь не ловит. Иначе панель с
    // альфой 0 продолжает перехватывать клики — классическая причина «кнопка
    // не нажимается, а почему — непонятно».
    if (!r->Visible) return false;
    if (r->Opacity < ia->AlphaThreshold) return false;

    // Маска предков режет и попадание тоже (§52): клик по невидимому куску
    // списка — самая частая жалоба на маски, которых нет.
    if (layout.Masks().Sample(r->MaskState, point, true) < 0.5f) return false;

    const glm::vec2 local = ToLocal(*r, point);
    const UIRect rect = UIInflate(r->Rect, UIEdges(ia->HitPadding.L * r->Scale,
                                                   ia->HitPadding.T * r->Scale,
                                                   ia->HitPadding.R * r->Scale,
                                                   ia->HitPadding.B * r->Scale));

    switch (ia->Hit) {
        case UIHitShape::Rect: return UIContains(rect, local);
        case UIHitShape::RoundedRect: {
            // Радиус берётся у того, что нарисовано: иначе скруглённая кнопка
            // ловит мышь в срезанных углах, и подсказка выскакивает «рядом».
            UICorners radius(0.0f);
            if (const UIFill* f = node->Get<UIFill>()) radius = f->Radius;
            else if (const UIImage* im = node->Get<UIImage>()) radius = im->Radius;
            else if (const UIShape* sh = node->Get<UIShape>()) radius = sh->Radius;
            const UICorners scaled(radius.TL * r->Scale, radius.TR * r->Scale,
                                   radius.BR * r->Scale, radius.BL * r->Scale);
            return InsideRoundedRect(rect, scaled, local);
        }
        case UIHitShape::Ellipse: {
            const glm::vec2 c = UICenter(rect);
            const glm::vec2 rad{rect.w * 0.5f, rect.h * 0.5f};
            if (rad.x <= 0.0f || rad.y <= 0.0f) return false;
            const glm::vec2 d = (local - c) / rad;
            return d.x * d.x + d.y * d.y <= 1.0f;
        }
        case UIHitShape::Polygon: {
            const UIShape* sh = node->Get<UIShape>();
            if (!sh || sh->Points.size() < 3) return UIContains(rect, local);
            std::vector<glm::vec2> pts;
            pts.reserve(sh->Points.size());
            for (const glm::vec2& p : sh->Points)
                pts.push_back({rect.x + p.x * rect.w, rect.y + p.y * rect.h});
            return InsidePolygon(pts, local);
        }
        case UIHitShape::ImageAlpha:
            // Альфа пикселей живёт на GPU; читать её обратно на каждый кадр
            // ради попадания — дороже, чем всё остальное попадание вместе
            // взятое. Честный ответ: прямоугольник картинки, и это записано
            // здесь, а не «догадайся сам» (§134).
            return UIContains(rect, local);
        default: return false;
    }
    (void)ctx;
}

UIHitResult UIHitTest(const UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
                      glm::vec2 point) {
    UIHitResult best;
    uint64_t bestKey = 0;
    bool found = false;

    // Верхний по ТОМУ ЖЕ ключу, по которому идёт рисование (§54). Отдельного
    // порядка для ввода нет намеренно: иначе «кликается не то, что видно»
    // становится неизбежностью.
    for (int i = 0; i < (int)layout.Nodes().size(); ++i) {
        const UIResolvedNode& r = layout.Nodes()[(size_t)i];
        if (!r.HitTestable) continue;
        const UINode* node = doc.Find(r.Id);
        if (!node) continue;
        const UIInteraction* ia = node->Get<UIInteraction>();
        if (!ia || !ia->BlockRaycast) continue;
        if (!UIHitNode(doc, layout, ctx, r.Id, point)) continue;
        if (!found || r.SortKey >= bestKey) {
            found = true;
            bestKey = r.SortKey;
            best.Node = r.Id;
            best.Index = i;
            best.Local = ToLocal(r, point) - UIPos(r.Rect);
        }
    }
    return best;
}

void UIHitPath(const UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
               glm::vec2 point, std::vector<UINodeId>& outPath) {
    outPath.clear();
    const UIHitResult hit = UIHitTest(doc, layout, ctx, point);
    if (hit.Node == kUIInvalidNode) return;
    // Путь строится ОТ КОРНЯ к цели: в этом порядке идёт фаза перехвата, а
    // обратный проход даёт всплытие (§51).
    const UINode* n = doc.Find(hit.Node);
    while (n) {
        outPath.push_back(n->Id);
        n = n->Parent == kUIInvalidNode ? nullptr : doc.Find(n->Parent);
    }
    std::reverse(outPath.begin(), outPath.end());
}

} // namespace sage::ui
