#include "UISceneSystem.h"
#include "UIRenderer.h"
#include "UIIcons.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include <algorithm>
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

void DrawElement(const UIElementComponent& e, const UIRect& r, UIRenderer& ui) {
    using Kind = UIElementComponent::Kind;
    glm::vec3 fillRgb{e.Color.r, e.Color.g, e.Color.b};

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
                ui.Image(r.x, r.y, r.w, r.h, e.Tex.get(), fillRgb, e.Color.a, e.Rounding);
            } else {
                // Текстура не задана/не загрузилась — заглушка цветом, чтобы
                // элемент был виден и настраиваем в редакторе.
                FillRect(e, r, ui);
            }
            break;
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

    if (!e.Text.empty() && e.TextColor.a > 0.0f) {
        glm::vec3 textRgb{e.TextColor.r, e.TextColor.g, e.TextColor.b};
        // Однострочный текст центрируется по высоте элемента — иначе подпись
        // рядом с иконкой висит выше неё, и строка «иконка + текст» выглядит
        // развалившейся. У многострочного отсчёт от верха: центрировать по
        // первой строке нечестно, а высоту всего блока здесь не знают.
        bool multiline = e.Text.find('\n') != std::string::npos;
        float textY = multiline ? r.y + 6.0f
                                : r.y + (r.h - ui.TextHeight(e.TextScale)) * 0.5f;
        if (e.TextCentered) {
            ui.TextCentered(r.x + r.w * 0.5f, textY, e.TextScale, textRgb, e.Text, e.TextColor.a);
        } else {
            ui.Text(textLeft, textY, e.TextScale, textRgb, e.Text, e.TextColor.a);
        }
    }
}

void DrawSubtree(Scene& scene, entt::entity ent, const UIRect& parentRect, UIRenderer& ui) {
    entt::registry& reg = scene.Registry();
    UIElementComponent& e = reg.get<UIElementComponent>(ent);
    if (!e.Visible) return; // невидимый элемент прячет и всё поддерево

    // Размер по содержимому считается здесь: ширину текста знает только шрифт,
    // а он есть только у рендерера. Результат остаётся в LayoutSize — им
    // пользуются HitTest и следующий кадр.
    e.LayoutSize = MeasuredSize(e, ui);
    UIRect r = ResolveElementRect(e, parentRect, e.LayoutSize);
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
