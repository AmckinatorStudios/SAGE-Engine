#include "sage/ui/UIButton.h"

#include <algorithm>

#include "sage/scene/Scene.h"
#include "sage/scene/SceneComponents.h"
#include "sage/ui/UI.h"

namespace sage::ui {
namespace {

// Дети кнопки: первая надпись — подпись, первый значок — значок.
void FindParts(Scene& scene, entt::entity button, entt::entity& label, entt::entity& icon) {
    entt::registry& reg = scene.Registry();
    label = icon = entt::null;
    for (entt::entity c : scene.ChildrenOf(button)) {
        if (label == entt::null && reg.all_of<Label>(c)) label = c;
        if (icon == entt::null && reg.all_of<Icon>(c)) icon = c;
    }
}

entt::entity MakeChild(Scene& scene, entt::entity button, const char* name, int order) {
    GameObject obj = scene.CreateEmptyObject(name);
    scene.SetParent(obj.Entity(), button);
    Element el;
    el.Anchor = UIAnchor::Center;
    el.Order = order;
    el.Skin = Element::SkinMode::Engine;
    scene.Registry().emplace<Element>(obj.Entity(), el);
    return obj.Entity();
}

void Remove(Scene& scene, entt::entity e) {
    if (const IdComponent* id = scene.Registry().try_get<IdComponent>(e)) scene.RemoveObject(id->Id);
}

} // namespace

bool ReadButtonContent(Scene& scene, entt::entity button, ButtonContent& out) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(button) || !reg.all_of<Element>(button)) return false;
    entt::entity label, icon;
    FindParts(scene, button, label, icon);
    out = ButtonContent{};
    if (label != entt::null) out.Label = reg.get<Label>(label).Text;
    if (icon != entt::null) out.Icon = reg.get<Icon>(icon).Name;
    if (const Arrangement* a = reg.try_get<Arrangement>(button); a && a->Arranges()) {
        out.Alignment = a->Distribution;
        out.Spacing = a->Spacing;
    } else if (label != entt::null) {
        // Без ряда выравнивает сама подпись во всю кнопку.
        const Label::Align h = reg.get<Label>(label).Horizontal;
        out.Alignment = h == Label::Align::Start ? Arrangement::Spread::Start
                        : h == Label::Align::End ? Arrangement::Spread::End
                                                 : Arrangement::Spread::Center;
    }
    return true;
}

void ApplyButtonContent(Scene& scene, entt::entity button, const ButtonContent& content) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(button) || !reg.all_of<Element>(button)) return;
    entt::entity label, icon;
    FindParts(scene, button, label, icon);

    if (content.Icon.empty() && icon != entt::null) Remove(scene, icon), icon = entt::null;
    if (content.Label.empty() && label != entt::null) Remove(scene, label), label = entt::null;
    const float h = reg.get<Element>(button).Size.y;
    if (!content.Icon.empty()) {
        if (icon == entt::null) {
            icon = MakeChild(scene, button, "Значок", 0);
            reg.emplace<Icon>(icon);
        }
        reg.get<Icon>(icon).Name = content.Icon;
        Element& el = reg.get<Element>(icon);
        el.Size = glm::vec2(std::max(12.0f, h * 0.45f));
        el.Order = 0;
    }
    if (!content.Label.empty()) {
        if (label == entt::null) {
            label = MakeChild(scene, button, "Текст", 1);
            reg.emplace<Label>(label).StateColors = true;
        }
        reg.get<Label>(label).Text = content.Label;
    }

    // Только подпись — она сама во всю кнопку, выравнивание — её. Со значком
    // — ряд кнопки: значок и подпись рядом, вместе по выравниванию.
    const Label::Align align = content.Alignment == Arrangement::Spread::Start ? Label::Align::Start
                               : content.Alignment == Arrangement::Spread::End ? Label::Align::End
                                                                                : Label::Align::Center;
    if (icon == entt::null) {
        reg.remove<Arrangement>(button);
        if (label != entt::null) {
            Element& el = reg.get<Element>(label);
            el.Mode = Element::Stretch::Both;
            el.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
            reg.get<Label>(label).Horizontal = align;
            reg.get<Label>(label).AutoWidth = false;
        }
        return;
    }
    Arrangement& a = reg.get_or_emplace<Arrangement>(button);
    a.Mode = Arrangement::Kind::Horizontal;
    a.Alignment = Arrangement::Align::Center;
    a.Distribution = content.Alignment;
    a.Spacing = content.Spacing;
    a.Padding = {12.0f, 0.0f, 12.0f, 0.0f};
    a.ChildWidth = a.ChildHeight = Arrangement::ChildSize::Fixed;
    if (label != entt::null) {
        Element& el = reg.get<Element>(label);
        el.Mode = Element::Stretch::Vertical;   // по высоте кнопки, по ширине — по тексту
        el.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        el.Order = 1;
        Label& l = reg.get<Label>(label);
        l.AutoWidth = true;
        l.Horizontal = Label::Align::Center;
    }
}

void PlaceToggleCaption(Scene& scene, entt::entity toggle) {
    entt::registry& reg = scene.Registry();
    const Range* range = reg.try_get<Range>(toggle);
    const Element* box = reg.try_get<Element>(toggle);
    if (!range || !range->Toggle || !box) return;
    // Ширина значка — та же, что рисует DrawRangePart.
    const float h = box->Size.y;
    const float icon = range->ToggleVariant == Range::Variant::Switch ? std::min(h, box->Size.x / 1.8f) * 1.8f
                                                                      : std::min(h, box->Size.x);
    for (entt::entity c : scene.ChildrenOf(toggle)) {
        if (!reg.all_of<Label>(c)) continue;
        Element& el = reg.get<Element>(c);
        el.Anchor = UIAnchor::CenterLeft;
        el.Mode = Element::Stretch::None;
        el.Position.x = icon + 8.0f;
        el.Size.x = std::max(10.0f, box->Size.x - el.Position.x);
        break;
    }
}

} // namespace sage::ui
