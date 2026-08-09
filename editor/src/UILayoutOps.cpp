#include "UILayoutOps.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace {

using sage::ui::UIRect;

// Один выделенный элемент со всем, что нужно, чтобы его подвинуть.
// Прямоугольники — В ПИКСЕЛЯХ ЭКРАНА, как их считает сама система интерфейса.
struct Target {
    entt::entity Entity = entt::null;
    UIRect Rect{};
    UIRect Parent{};
    float Scale = 1.0f;   // опорные единицы -> пиксели (см. Place)
    bool InLayout = false;
};

// Выделенные элементы интерфейса — в порядке выделения. Элементы без
// ui::Transform молча пропускаются: выделение общее на всю сцену, и куб в нём
// не ошибка, просто не наше дело.
//
// Прямоугольники берутся у sage::ui::SolveSceneRects — тем же расчётом, каким
// интерфейс рисуют и каким его показывает холст редактора. Своя копия формул
// здесь означала бы, что кнопка «по левому краю» равняет не по тому краю,
// который человек видит.
std::vector<Target> Collect(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    const glm::vec2 frame = host.UITools().FrameSize;
    const std::vector<sage::ui::ElementRect> solved =
        sage::ui::SolveSceneRects(scene, (int)frame.x, (int)frame.y, /*includeHidden=*/true);

    std::vector<Target> out;
    for (int id : host.Selection()) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
        if (!reg.all_of<sage::ui::Transform>(obj.Entity())) continue;
        for (const sage::ui::ElementRect& e : solved) {
            if (e.Entity != obj.Entity()) continue;
            out.push_back(Target{e.Entity, e.Rect, e.Parent, e.Scale, e.InLayout});
            break;
        }
    }
    return out;
}

// Пиксели экрана -> опорные единицы холста, в которых и хранятся Offset/Size.
void Place(Scene& scene, const Target& t, glm::vec2 topLeft, glm::vec2 size) {
    sage::ui::Transform* u = scene.Registry().try_get<sage::ui::Transform>(t.Entity);
    if (!u || t.InLayout) return;   // раскладка родителя всё равно переставит
    const float k = t.Scale > 0.0f ? t.Scale : 1.0f;
    const UIRect parent{t.Parent.x / k, t.Parent.y / k, t.Parent.w / k, t.Parent.h / k};
    u->Size = size / k;
    u->Offset = sage::ui::OffsetForTopLeft(u->Anchor, topLeft / k, size / k, parent);
}

void Shift(Scene& scene, const Target& t, glm::vec2 delta) {
    Place(scene, t, glm::vec2(t.Rect.x + delta.x, t.Rect.y + delta.y),
          glm::vec2(t.Rect.w, t.Rect.h));
}

} // namespace

namespace uiops {

void Align(EditorHost& host, sage::ui::AlignEdge edge) {
    std::vector<Target> targets = Collect(host);
    if (targets.empty()) return;
    Scene& scene = host.CurrentScene();

    // Один элемент — равняем по родителю (см. заголовок: «надпись по центру
    // панели»). Несколько — по первичному, то есть по последнему кликнутому:
    // это единственный элемент, про который человек точно знает, что он
    // остался на месте.
    GameObject primaryObj = scene.Get(host.SelectedId());
    const entt::entity primary = primaryObj.Valid() ? primaryObj.Entity() : entt::null;

    UIRect target = targets.front().Parent;
    if (targets.size() > 1) {
        for (const Target& t : targets)
            if (t.Entity == primary) target = t.Rect;
    }

    host.PushUndoSnapshot();
    for (const Target& t : targets) {
        // Первичный — это ЦЕЛЬ, его не двигают. Сравнение по сущности, а не по
        // совпадению прямоугольников: две одинаковые кнопки друг под другом —
        // обычное дело, и по числам они неразличимы.
        if (targets.size() > 1 && t.Entity == primary) continue;
        Shift(scene, t, sage::ui::AlignDelta(t.Rect, target, edge));
    }
}

void Distribute(EditorHost& host, bool horizontal, bool byCenters) {
    std::vector<Target> targets = Collect(host);
    if (targets.size() < 3) return;
    Scene& scene = host.CurrentScene();

    std::vector<UIRect> rects;
    rects.reserve(targets.size());
    for (const Target& t : targets) rects.push_back(t.Rect);

    const std::vector<float> deltas = byCenters
                                          ? sage::ui::DistributeCenterDeltas(rects, horizontal)
                                          : sage::ui::DistributeGapDeltas(rects, horizontal);
    host.PushUndoSnapshot();
    for (size_t i = 0; i < targets.size(); ++i) {
        const glm::vec2 d = horizontal ? glm::vec2(deltas[i], 0.0f) : glm::vec2(0.0f, deltas[i]);
        Shift(scene, targets[i], d);
    }
}

void StretchToParent(EditorHost& host, float margin) {
    std::vector<Target> targets = Collect(host);
    if (targets.empty()) return;
    Scene& scene = host.CurrentScene();

    host.PushUndoSnapshot();
    for (const Target& t : targets) {
        Place(scene, t, glm::vec2(t.Parent.x + margin, t.Parent.y + margin),
              glm::vec2(std::max(1.0f, t.Parent.w - margin * 2.0f),
                        std::max(1.0f, t.Parent.h - margin * 2.0f)));
    }
}

void SetAnchorKeepingPlace(EditorHost& host, UIAnchor anchor) {
    std::vector<Target> targets = Collect(host);
    if (targets.empty()) return;
    Scene& scene = host.CurrentScene();

    host.PushUndoSnapshot();
    for (const Target& t : targets) {
        sage::ui::Transform* u = scene.Registry().try_get<sage::ui::Transform>(t.Entity);
        if (!u) continue;
        u->Anchor = anchor;
        // Прямоугольник взят ДО смены якоря — именно поэтому элемент и остаётся
        // на месте: новый Offset считается под новый якорь из старого места.
        u->Offset = sage::ui::OffsetForTopLeft(anchor, glm::vec2(t.Rect.x, t.Rect.y),
                                               glm::vec2(t.Rect.w, t.Rect.h), t.Parent);
    }
}

void BringIntoView(EditorHost& host) {
    std::vector<Target> targets = Collect(host);
    if (targets.empty()) return;
    const glm::vec2 frame = host.UITools().FrameSize;
    Scene& scene = host.CurrentScene();

    host.PushUndoSnapshot();
    for (const Target& t : targets) {
        // Прижимаем к экрану по каждой оси отдельно: элемент, вышедший только
        // вправо, не должен ещё и прыгнуть по вертикали.
        float x = std::min(std::max(t.Rect.x, 0.0f), std::max(0.0f, frame.x - t.Rect.w));
        float y = std::min(std::max(t.Rect.y, 0.0f), std::max(0.0f, frame.y - t.Rect.h));
        if (x == t.Rect.x && y == t.Rect.y) continue;
        Place(scene, t, glm::vec2(x, y), glm::vec2(t.Rect.w, t.Rect.h));
    }
}

void SnapSelectionToGrid(EditorHost& host) {
    std::vector<Target> targets = Collect(host);
    if (targets.empty()) return;
    const float step = host.UITools().Snap.GridStep;
    if (step <= 0.0f) return;
    Scene& scene = host.CurrentScene();

    host.PushUndoSnapshot();
    for (const Target& t : targets) {
        const glm::vec2 tl(std::round(t.Rect.x / step) * step, std::round(t.Rect.y / step) * step);
        // Размер округляется вверх до целой клетки, а не до ближайшей: кнопка
        // шириной 61 при шаге 8 должна стать 64, а не 56 — уменьшать размер
        // «наведением порядка» значит обрезать содержимое.
        const glm::vec2 size(std::max(step, std::ceil(t.Rect.w / step) * step),
                             std::max(step, std::ceil(t.Rect.h / step) * step));
        Place(scene, t, tl, size);
    }
}

} // namespace uiops
