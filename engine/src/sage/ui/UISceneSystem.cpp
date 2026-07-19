#include "UISceneSystem.h"
#include "UIRenderer.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include <algorithm>
#include <vector>

namespace sage::ui {

UIRect ResolveElementRect(const UIElementComponent& e, const UIRect& parent) {
    glm::vec2 pos = ResolveAnchored(e.Anchor, e.Offset, e.Size, parent);
    return {pos.x, pos.y, e.Size.x, e.Size.y};
}

namespace {

// Дети сущности, несущие UIElementComponent, отсортированные по Layer
// (стабильно; при равенстве — по Id, чтобы порядок был детерминирован).
std::vector<entt::entity> SortedUIChildren(Scene& scene, entt::entity parent) {
    std::vector<entt::entity> kids;
    entt::registry& reg = scene.Registry();
    if (const auto* h = reg.try_get<HierarchyComponent>(parent)) {
        for (auto c : h->Children)
            if (reg.valid(c) && reg.all_of<UIElementComponent>(c)) kids.push_back(c);
    }
    std::stable_sort(kids.begin(), kids.end(), [&reg](entt::entity a, entt::entity b) {
        const auto& ua = reg.get<UIElementComponent>(a);
        const auto& ub = reg.get<UIElementComponent>(b);
        if (ua.Layer != ub.Layer) return ua.Layer < ub.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return kids;
}

// Корневые UI-сущности: без родителя ЛИБО родитель не UI-элемент (3D-сущность
// может «держать» интерфейс — он всё равно якорится к экрану).
std::vector<entt::entity> SortedUIRoots(Scene& scene) {
    std::vector<entt::entity> roots;
    entt::registry& reg = scene.Registry();
    for (auto e : reg.view<UIElementComponent>()) {
        entt::entity parent = scene.ParentOf(e);
        if (parent == entt::null || !reg.all_of<UIElementComponent>(parent)) roots.push_back(e);
    }
    std::stable_sort(roots.begin(), roots.end(), [&reg](entt::entity a, entt::entity b) {
        const auto& ua = reg.get<UIElementComponent>(a);
        const auto& ub = reg.get<UIElementComponent>(b);
        if (ua.Layer != ub.Layer) return ua.Layer < ub.Layer;
        return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
    });
    return roots;
}

void DrawElement(const UIElementComponent& e, const UIRect& r, UIRenderer& ui) {
    using Kind = UIElementComponent::Kind;
    glm::vec3 fillRgb{e.Color.r, e.Color.g, e.Color.b};

    switch (e.Type) {
        case Kind::Panel:
            if (e.Color.a > 0.0f) ui.RoundedRect(r.x, r.y, r.w, r.h, fillRgb, e.Color.a, e.Rounding);
            break;
        case Kind::Label:
            break; // только текст (ниже)
        case Kind::Image:
            if (e.Tex) {
                ui.Image(r.x, r.y, r.w, r.h, e.Tex.get(), fillRgb, e.Color.a, e.Rounding);
            } else if (e.Color.a > 0.0f) {
                // Текстура не задана/не загрузилась — заглушка цветом, чтобы
                // элемент был виден и настраиваем в редакторе.
                ui.RoundedRect(r.x, r.y, r.w, r.h, fillRgb, e.Color.a, e.Rounding);
            }
            break;
        case Kind::Bar: {
            if (e.Color.a > 0.0f) ui.RoundedRect(r.x, r.y, r.w, r.h, fillRgb, e.Color.a, e.Rounding);
            float v = glm::clamp(e.Value, 0.0f, 1.0f);
            if (v > 0.0f && e.BarFillColor.a > 0.0f) {
                // Заполнение с небольшим внутренним отступом, радиус — согласованный.
                float pad = glm::min(2.0f, glm::min(r.w, r.h) * 0.15f);
                float innerR = glm::max(e.Rounding - pad, 0.0f);
                ui.RoundedRect(r.x + pad, r.y + pad, (r.w - 2 * pad) * v, r.h - 2 * pad,
                               {e.BarFillColor.r, e.BarFillColor.g, e.BarFillColor.b},
                               e.BarFillColor.a, innerR);
            }
            break;
        }
    }

    if (e.BorderThickness > 0.0f && e.BorderColor.a > 0.0f) {
        ui.RoundedRectOutline(r.x, r.y, r.w, r.h, e.Rounding, e.BorderThickness,
                              {e.BorderColor.r, e.BorderColor.g, e.BorderColor.b}, e.BorderColor.a);
    }

    if (!e.Text.empty() && e.TextColor.a > 0.0f) {
        glm::vec3 textRgb{e.TextColor.r, e.TextColor.g, e.TextColor.b};
        if (e.TextCentered) {
            float textY = r.y + (r.h - ui.TextHeight(e.TextScale)) * 0.5f;
            ui.TextCentered(r.x + r.w * 0.5f, textY, e.TextScale, textRgb, e.Text, e.TextColor.a);
        } else {
            ui.Text(r.x + 8.0f, r.y + 6.0f, e.TextScale, textRgb, e.Text, e.TextColor.a);
        }
    }
}

void DrawSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer& ui) {
    entt::registry& reg = scene.Registry();
    const UIElementComponent& e = reg.get<UIElementComponent>(ent);
    if (!e.Visible) return; // невидимый элемент прячет и всё поддерево

    UIRect r = ResolveElementRect(e, parentRect);
    DrawElement(e, r, ui);

    std::vector<entt::entity> kids = SortedUIChildren(scene, ent);
    if (kids.empty()) return;

    if (e.ClipChildren) ui.PushClipRect(r.x, r.y, r.w, r.h);
    for (auto k : kids) DrawSubtree(scene, k, r, ui);
    if (e.ClipChildren) ui.PopClipRect();
}

// Обход для HitTest — тот же порядок, что у отрисовки; последний попавший
// в точку и нарисованный ПОВЕРХ выигрывает.
void HitSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect,
                float x, float y, int& bestId) {
    entt::registry& reg = scene.Registry();
    const UIElementComponent& e = reg.get<UIElementComponent>(ent);
    if (!e.Visible) return;

    UIRect r = ResolveElementRect(e, parentRect);
    bool inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    if (inside) bestId = reg.get<IdComponent>(ent).Id;

    // Маска: точка вне прямоугольника не может попасть в обрезанных детей.
    if (e.ClipChildren && !inside) return;
    for (auto k : SortedUIChildren(scene, ent)) HitSubtree(scene, k, r, x, y, bestId);
}

} // namespace

void DrawSceneUI(Scene& scene, UIRenderer& ui, int screenW, int screenH) {
    UIRect screen{0.0f, 0.0f, (float)screenW, (float)screenH};
    for (auto root : SortedUIRoots(scene)) DrawSubtree(scene, root, screen, ui);
}

int HitTest(Scene& scene, float x, float y, int screenW, int screenH) {
    UIRect screen{0.0f, 0.0f, (float)screenW, (float)screenH};
    int bestId = -1;
    for (auto root : SortedUIRoots(scene)) HitSubtree(scene, root, screen, x, y, bestId);
    return bestId;
}

} // namespace sage::ui
