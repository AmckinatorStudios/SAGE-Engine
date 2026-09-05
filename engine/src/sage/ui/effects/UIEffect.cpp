#include "sage/ui/effects/UIEffect.h"

#include <algorithm>

namespace sage::ui {

// --- Встроенные эффекты -----------------------------------------------------

const UIEffectType& UIDropShadow::StaticType() {
    static UIEffectType t = [] {
        UIEffectType d;
        d.Id = "shadow";
        d.Title = SAGE_UI_TEXT("Drop shadow");
        d.Hint = SAGE_UI_TEXT("Soft shadow under the node");
        d.Icon = "shadow";
        d.Stage = UIEffectStage::Behind;
        d.Create = [] { return std::unique_ptr<UIEffect>(new UIDropShadow()); };
        d.Props = {
            {"Offset", SAGE_UI_TEXT("Offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIDropShadow, Offset), -64.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Blur", SAGE_UI_TEXT("Blur"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIDropShadow, Blur), 0.0f, 96.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Spread", SAGE_UI_TEXT("Spread"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIDropShadow, Spread), -32.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIDropShadow, Color), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIEffectType& UIInnerShadow::StaticType() {
    static UIEffectType t = [] {
        UIEffectType d;
        d.Id = "inner_shadow";
        d.Title = SAGE_UI_TEXT("Inner shadow");
        d.Hint = SAGE_UI_TEXT("Shadow cast inside the shape");
        d.Icon = "shadow";
        d.Stage = UIEffectStage::Front;
        d.Create = [] { return std::unique_ptr<UIEffect>(new UIInnerShadow()); };
        d.Props = {
            {"Offset", SAGE_UI_TEXT("Offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIInnerShadow, Offset), -32.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Blur", SAGE_UI_TEXT("Blur"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIInnerShadow, Blur), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Spread", SAGE_UI_TEXT("Spread"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIInnerShadow, Spread), -32.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIInnerShadow, Color), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIEffectType& UIGlow::StaticType() {
    static UIEffectType t = [] {
        UIEffectType d;
        d.Id = "glow";
        d.Title = SAGE_UI_TEXT("Glow");
        d.Hint = SAGE_UI_TEXT("Light halo around the visible shape");
        d.Icon = "glow";
        d.Stage = UIEffectStage::Behind;
        d.Create = [] { return std::unique_ptr<UIEffect>(new UIGlow()); };
        d.Props = {
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIGlow, Color), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Radius", SAGE_UI_TEXT("Radius"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIGlow, Radius), 0.0f, 128.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Intensity", SAGE_UI_TEXT("Intensity"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIGlow, Intensity), 0.0f, 4.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Inner", SAGE_UI_TEXT("Inner"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIGlow, Inner), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
        };
        return d;
    }();
    return t;
}

const UIEffectType& UIBlur::StaticType() {
    static UIEffectType t = [] {
        UIEffectType d;
        d.Id = "blur";
        d.Title = SAGE_UI_TEXT("Blur");
        d.Hint = SAGE_UI_TEXT("Needs an offscreen target — the cost is visible in the profiler");
        d.Icon = "blur";
        d.Stage = UIEffectStage::Offscreen;
        d.Create = [] { return std::unique_ptr<UIEffect>(new UIBlur()); };
        d.Props = {
            {"Radius", SAGE_UI_TEXT("Radius"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIBlur, Radius), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Passes", SAGE_UI_TEXT("Passes"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UIBlur, Passes), 1.0f, 6.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Backdrop", SAGE_UI_TEXT("Backdrop"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIBlur, Backdrop), 0, 0,
             SAGE_UI_TEXT("Blur what is behind the node instead of the node itself"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

const UIEffectType& UIColorEffect::StaticType() {
    static UIEffectType t = [] {
        UIEffectType d;
        d.Id = "color";
        d.Title = SAGE_UI_TEXT("Color adjust");
        d.Hint = SAGE_UI_TEXT("Tint, brightness, contrast, saturation");
        d.Icon = "palette";
        d.Stage = UIEffectStage::Modulate;
        d.Create = [] { return std::unique_ptr<UIEffect>(new UIColorEffect()); };
        d.Props = {
            {"Tint", SAGE_UI_TEXT("Tint"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIColorEffect, Tint), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Brightness", SAGE_UI_TEXT("Brightness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIColorEffect, Brightness), 0.0f, 3.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Contrast", SAGE_UI_TEXT("Contrast"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIColorEffect, Contrast), 0.0f, 3.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Saturation", SAGE_UI_TEXT("Saturation"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIColorEffect, Saturation), 0.0f, 3.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Opacity", SAGE_UI_TEXT("Opacity"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIColorEffect, Opacity), 0.0f, 1.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
        };
        return d;
    }();
    return t;
}

// --- Стек -------------------------------------------------------------------

const UIComponentType& UIEffects::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "effects";
        d.Title = SAGE_UI_TEXT("Effects");
        d.Hint = SAGE_UI_TEXT("Ordered stack of effects applied to the node");
        d.Icon = "sparkle";
        d.Category = UIComponentCategory::Effects;
        d.Order = 90;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIEffects()); };
        // Свойств у самого стека нет: они у эффектов внутри, и редактор берёт
        // их из UIEffectType. Отдельного списка полей здесь быть не должно —
        // иначе он немедленно разойдётся с набором эффектов.
        return d;
    }();
    return t;
}

UIEffects& UIEffects::operator=(const UIEffects& o) {
    if (this == &o) return *this;
    Items.clear();
    Items.reserve(o.Items.size());
    for (const auto& e : o.Items) Items.push_back(e->Clone());
    return *this;
}

UIEffect* UIEffects::Add(std::string_view typeId) {
    std::unique_ptr<UIEffect> made = UIEffectRegistry::Instance().Create(typeId);
    if (!made) return nullptr;
    Items.push_back(std::move(made));
    return Items.back().get();
}

void UIEffects::Remove(int index) {
    if (index < 0 || index >= (int)Items.size()) return;
    Items.erase(Items.begin() + index);
}

void UIEffects::MoveItem(int from, int to) {
    // Порядок эффектов значим (§37), поэтому перестановка — обычная операция, а
    // не «редкий случай»: она обязана быть в самой модели, а не в редакторе.
    if (from < 0 || from >= (int)Items.size()) return;
    if (to < 0) to = 0;
    if (to >= (int)Items.size()) to = (int)Items.size() - 1;
    if (from == to) return;
    std::unique_ptr<UIEffect> moved = std::move(Items[(size_t)from]);
    Items.erase(Items.begin() + from);
    Items.insert(Items.begin() + to, std::move(moved));
}

bool UIEffects::NeedsOffscreen() const {
    for (const auto& e : Items)
        if (e->Enabled && e->Type().Stage == UIEffectStage::Offscreen) return true;
    return false;
}

// --- Реестр -----------------------------------------------------------------

UIEffectRegistry& UIEffectRegistry::Instance() {
    static UIEffectRegistry r;
    return r;
}

void UIEffectRegistry::Register(const UIEffectType& type) {
    for (auto& t : m_types) {
        if (t->Id == type.Id) { t = &type; m_sorted.clear(); return; }
    }
    m_types.push_back(&type);
    m_sorted.clear();
}

const UIEffectType* UIEffectRegistry::Find(std::string_view id) const {
    EnsureBuiltins();
    for (const UIEffectType* t : m_types)
        if (t->Id == id) return t;
    return nullptr;
}

const std::vector<const UIEffectType*>& UIEffectRegistry::All() const {
    EnsureBuiltins();
    if (m_sorted.empty()) m_sorted = m_types;
    return m_sorted;
}

std::unique_ptr<UIEffect> UIEffectRegistry::Create(std::string_view id) const {
    const UIEffectType* t = Find(id);
    if (!t || !t->Create) return nullptr;
    return t->Create();
}

void UIEffectRegistry::EnsureBuiltins() const {
    if (m_builtinsDone) return;
    m_builtinsDone = true;
    RegisterBuiltinUIEffects();
}

void RegisterBuiltinUIEffects() {
    static bool done = false;
    if (done) return;
    done = true;
    UIEffectRegistry& r = UIEffectRegistry::Instance();
    r.Register(UIDropShadow::StaticType());
    r.Register(UIInnerShadow::StaticType());
    r.Register(UIGlow::StaticType());
    r.Register(UIBlur::StaticType());
    r.Register(UIColorEffect::StaticType());
}

} // namespace sage::ui
