#include "sage/ui/style/UIStyle.h"

#include <algorithm>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/input/UIInteraction.h"

namespace sage::ui {

const UIComponentType& UIStyled::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "styled";
        d.Title = SAGE_UI_TEXT("Style");
        d.Hint = SAGE_UI_TEXT("Named style from the theme");
        d.Icon = "palette";
        d.Category = UIComponentCategory::Appearance;
        d.Order = 1;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIStyled()); };
        d.Props = {
            {"Style", SAGE_UI_TEXT("Style name"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIStyled, Style), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
        };
        return d;
    }();
    return t;
}

bool UIStyled::IsOverridden(const std::string& path) const {
    return std::find(Overrides.begin(), Overrides.end(), path) != Overrides.end();
}

void UIStyled::SetOverride(const std::string& path, bool on) {
    const auto it = std::find(Overrides.begin(), Overrides.end(), path);
    if (on && it == Overrides.end()) Overrides.push_back(path);
    else if (!on && it != Overrides.end()) Overrides.erase(it);
}

const UIStyle* UITheme::Find(const std::string& name) const {
    auto it = Styles.find(name);
    return it == Styles.end() ? nullptr : &it->second;
}

UIStyle& UITheme::Ensure(const std::string& name) {
    UIStyle& s = Styles[name];
    if (s.Name.empty()) s.Name = name;
    return s;
}

bool UITheme::ResolveColor(const std::string& ref, UIColor& out) const {
    if (ref.empty()) return false;
    if (ref[0] == '@') {
        const std::string key = ref.substr(1);
        if (!Tokens.Has(key)) return false;
        out = Tokens.Color(key, out);
        return true;
    }
    if (ref[0] == '#') {
        out = UIColorFromHex(ref, out);
        return true;
    }
    return false;
}

bool UITheme::ResolveNumber(const std::string& ref, float& out) const {
    if (ref.empty() || ref[0] != '@') return false;
    const std::string key = ref.substr(1);
    if (!Tokens.Has(key)) return false;
    out = Tokens.Number(key, out);
    return true;
}

namespace {

// Применить одно значение стиля к узлу. Через таблицу свойств — то есть тем же
// путём, что запись в файл и инспектор: третьего способа положить значение в
// компонент в системе нет (§58).
void ApplyValue(const UITheme& theme, UINode& node, const UIStyleValue& v,
                const UIStyled* styled) {
    // Локальная правка сильнее темы (§59, пункт 5). Проверка здесь, а не у
    // вызывающего: иначе про неё забудут ровно один раз, и тема начнёт
    // затирать ручную работу.
    const std::string path = v.Component + "." + v.Property;
    if (styled && styled->IsOverridden(path)) return;

    UIComponent* comp = node.Find(v.Component);
    if (!comp) {
        comp = node.Add(v.Component);
        if (!comp) return;
    }
    const UIComponentType& type = comp->Type();
    const UIProperty* prop = nullptr;
    for (const UIProperty& p : type.Props)
        if (v.Property == p.Key) { prop = &p; break; }
    if (!prop) return;

    void* data = comp->Data();
    if (prop->Type == UIProperty::Kind::Color) {
        UIColor c = UIFieldAs<UIColor>(data, *prop);
        if (v.IsText) { if (theme.ResolveColor(v.Text, c)) UIFieldAs<UIColor>(data, *prop) = c; }
        else UIFieldAs<UIColor>(data, *prop) = v.Numbers;
        return;
    }
    if (prop->Type == UIProperty::Kind::String) {
        UIFieldAs<std::string>(data, *prop) = v.Text;
        return;
    }
    if (v.IsText) {
        float number = 0.0f;
        if (theme.ResolveNumber(v.Text, number)) {
            const int n = UIPropertyFloatCount(*prop);
            for (int i = 0; i < n; ++i) UIPropertySetFloat(data, *prop, i, number);
        }
        return;
    }
    const int n = UIPropertyFloatCount(*prop);
    for (int i = 0; i < n; ++i) UIPropertySetFloat(data, *prop, i, v.Numbers[i]);
}

const char* StateKey(uint32_t flag) {
    switch (flag) {
        case UIState_Hovered: return "hover";
        case UIState_Pressed: return "pressed";
        case UIState_Focused: return "focused";
        case UIState_Disabled: return "disabled";
        case UIState_Selected: return "selected";
        case UIState_Checked: return "checked";
        default: return nullptr;
    }
}

} // namespace

void UITheme::ApplyTo(UINode& node, uint32_t stateFlags) const {
    const UIStyled* styled = node.Get<UIStyled>();
    if (!styled || styled->Style.empty()) return;

    // Цепочка наследования стилей разворачивается от базового к производному:
    // производный обязан перекрывать базовый, а не наоборот.
    std::vector<const UIStyle*> chain;
    const UIStyle* s = Find(styled->Style);
    int guard = 0;
    while (s && guard++ < 16) {
        chain.push_back(s);
        s = s->Parent.empty() ? nullptr : Find(s->Parent);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const UIStyleValue& v : (*it)->Values) ApplyValue(*this, node, v, styled);
    }
    // Состояния — поверх базовых значений, в фиксированном порядке. Порядок
    // важен: «нажата и под курсором» обязано выглядеть как нажата.
    static const uint32_t order[] = {UIState_Selected, UIState_Checked, UIState_Hovered,
                                     UIState_Focused,  UIState_Pressed, UIState_Disabled};
    for (uint32_t flag : order) {
        if ((stateFlags & flag) == 0) continue;
        const char* key = StateKey(flag);
        if (!key) continue;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto found = (*it)->States.find(key);
            if (found == (*it)->States.end()) continue;
            for (const UIStyleValue& v : found->second) ApplyValue(*this, node, v, styled);
        }
    }
}

int UITheme::Apply(UIDocument& doc) const {
    int changed = 0;
    for (UINodeId id : doc.Ordered()) {
        UINode* node = doc.Find(id);
        if (!node) continue;
        const UIStyled* styled = node->Get<UIStyled>();
        if (!styled || styled->Style.empty()) continue;
        uint32_t flags = UIState_Normal;
        if (const UIInteraction* ia = node->Get<UIInteraction>()) flags = ia->Runtime.Flags;
        ApplyTo(*node, flags);
        ++changed;
    }
    if (changed > 0) doc.MarkDirty(UIDirty_Style | UIDirty_Visual | UIDirty_Layout);
    return changed;
}

UITheme UITheme::Default() {
    UITheme t;
    t.Name = "SAGE Dark";
    t.Tokens = UIDesignTokens::Default();

    auto colorValue = [](const char* comp, const char* prop, const char* token) {
        UIStyleValue v;
        v.Component = comp;
        v.Property = prop;
        v.Text = token;
        v.IsText = true;
        return v;
    };
    auto numberValue = [](const char* comp, const char* prop, float a, float b = 0.0f,
                          float c = 0.0f, float d = 0.0f) {
        UIStyleValue v;
        v.Component = comp;
        v.Property = prop;
        v.Numbers = {a, b, c, d};
        return v;
    };

    UIStyle& panel = t.Ensure("Panel");
    panel.Values = {colorValue("fill", "Color", "@Color.Surface"),
                    numberValue("fill", "Radius", 10.0f, 10.0f, 10.0f, 10.0f)};

    UIStyle& button = t.Ensure("Button");
    button.Values = {colorValue("fill", "Color", "@Color.SurfaceRaised"),
                     numberValue("fill", "Radius", 10.0f, 10.0f, 10.0f, 10.0f),
                     colorValue("border", "Color", "@Color.Border"),
                     numberValue("border", "Thickness", 1.0f, 1.0f, 1.0f, 1.0f)};
    button.States["hover"] = {colorValue("fill", "Color", "@Color.Border")};
    button.States["pressed"] = {colorValue("fill", "Color", "@Color.Surface")};
    button.States["focused"] = {colorValue("border", "Color", "@Color.Accent")};
    button.States["disabled"] = {colorValue("fill", "Color", "@Color.Background")};

    UIStyle& primary = t.Ensure("ButtonPrimary");
    primary.Parent = "Button";
    primary.Values = {colorValue("fill", "Color", "@Color.Accent"),
                      colorValue("text", "Color", "@Color.AccentText")};

    UIStyle& label = t.Ensure("Label");
    label.Values = {colorValue("text", "Color", "@Color.Text")};

    UIStyle& caption = t.Ensure("Caption");
    caption.Parent = "Label";
    caption.Values = {colorValue("text", "Color", "@Color.TextMuted"),
                      numberValue("text", "Size", 13.0f)};

    UIStyle& field = t.Ensure("InputField");
    field.Values = {colorValue("fill", "Color", "@Color.Background"),
                    numberValue("fill", "Radius", 6.0f, 6.0f, 6.0f, 6.0f),
                    colorValue("border", "Color", "@Color.Border"),
                    numberValue("border", "Thickness", 1.0f, 1.0f, 1.0f, 1.0f)};
    field.States["focused"] = {colorValue("border", "Color", "@Color.Accent")};
    return t;
}

} // namespace sage::ui
