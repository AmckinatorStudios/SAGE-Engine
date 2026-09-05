#include "sage/ui/animation/UIAnimationValue.h"

#include <cmath>

#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"

namespace sage::ui {

std::string UIPropertyPath::ToString() const {
    std::string out = NodePath.empty() ? Component : NodePath + "." + Component;
    out += "." + Property;
    if (Channel >= 0) {
        static const char* kChannels[4] = {"x", "y", "z", "w"};
        out += ".";
        out += kChannels[Channel < 4 ? Channel : 3];
    }
    return out;
}

UIPropertyPath UIPropertyPath::Parse(const std::string& s) {
    // Разбор идёт СПРАВА: имя узла может содержать точку («Panel v1.2»), а
    // ключи компонента и свойства — нет. Слева направо это неразрешимо.
    UIPropertyPath p;
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() < 2) return p;

    size_t last = parts.size() - 1;
    if (parts.size() >= 3 && parts[last].size() == 1) {
        const char c = parts[last][0];
        if (c == 'x' || c == 'r') p.Channel = 0;
        else if (c == 'y' || c == 'g') p.Channel = 1;
        else if (c == 'z' || c == 'b') p.Channel = 2;
        else if (c == 'w' || c == 'a') p.Channel = 3;
        if (p.Channel >= 0) --last;
    }
    if (last < 1) return UIPropertyPath{};
    p.Property = parts[last];
    p.Component = parts[last - 1];
    for (size_t i = 0; i + 1 < last; ++i) {
        if (!p.NodePath.empty()) p.NodePath += ".";
        p.NodePath += parts[i];
    }
    return p;
}

bool UIPropertyBinding::Bind(UIDocument& doc, const UIPropertyPath& path) {
    m_doc = &doc;
    m_component = nullptr;
    m_prop = nullptr;
    m_node = kUIInvalidNode;
    if (!path.Valid()) return false;

    UINode* node = path.NodePath.empty() ? nullptr : doc.FindByPath(path.NodePath);
    if (!node && !path.NodePath.empty()) node = doc.FindByName(path.NodePath);
    if (!node) return false;

    UIComponent* comp = node->Find(path.Component);
    if (!comp) return false;
    for (const UIProperty& p : comp->Type().Props) {
        if (path.Property == p.Key) { m_prop = &p; break; }
    }
    if (!m_prop) return false;

    m_component = comp;
    m_node = node->Id;
    m_channel = path.Channel;
    // Какой флаг «устарело» поднимать — свойство самого свойства, а не догадка
    // вызывающего: смена текста требует пересчёта раскладки, смена цвета — нет.
    if (path.Component == "text") m_dirty = UIDirty_Text | UIDirty_Layout;
    else if (path.Component == "transform" || path.Component == "layout")
        m_dirty = UIDirty_Layout;
    else m_dirty = UIDirty_Visual;
    return true;
}

bool UIPropertyBinding::Get(float& out) const {
    if (!Valid()) return false;
    return UIPropertyGetFloat(m_component->Data(), *m_prop, m_channel < 0 ? 0 : m_channel, out);
}

bool UIPropertyBinding::Set(float value) {
    if (!Valid()) return false;
    bool changed = false;
    if (m_channel >= 0) {
        float before = 0.0f;
        UIPropertyGetFloat(m_component->Data(), *m_prop, m_channel, before);
        changed = before != value;
        UIPropertySetFloat(m_component->Data(), *m_prop, m_channel, value);
    } else {
        // Без указания компоненты значение кладётся во ВСЕ: «прозрачность 0.5»
        // для vec4 без канала — почти всегда ошибка адреса, и лучше, чтобы она
        // была видна сразу.
        const int n = UIPropertyFloatCount(*m_prop);
        for (int i = 0; i < n; ++i) {
            float before = 0.0f;
            UIPropertyGetFloat(m_component->Data(), *m_prop, i, before);
            if (before != value) changed = true;
            UIPropertySetFloat(m_component->Data(), *m_prop, i, value);
        }
    }
    if (changed && m_doc) m_doc->MarkDirty(m_node, m_dirty);
    return changed;
}

bool UIPropertyBinding::GetVec4(glm::vec4& out) const {
    if (!Valid()) return false;
    const int n = UIPropertyFloatCount(*m_prop);
    for (int i = 0; i < n && i < 4; ++i) UIPropertyGetFloat(m_component->Data(), *m_prop, i, out[i]);
    return n > 0;
}

bool UIPropertyBinding::SetVec4(const glm::vec4& v) {
    if (!Valid()) return false;
    const int n = UIPropertyFloatCount(*m_prop);
    for (int i = 0; i < n && i < 4; ++i) UIPropertySetFloat(m_component->Data(), *m_prop, i, v[i]);
    if (m_doc) m_doc->MarkDirty(m_node, m_dirty);
    return n > 0;
}

bool UIPropertyBinding::SetString(const std::string& v) {
    if (!Valid() || m_prop->Type != UIProperty::Kind::String) return false;
    std::string& target = UIFieldAs<std::string>(m_component->Data(), *m_prop);
    if (target == v) return false;
    target = v;
    if (m_doc) m_doc->MarkDirty(m_node, m_dirty);
    return true;
}

bool UIPropertyBinding::GetString(std::string& out) const {
    if (!Valid() || m_prop->Type != UIProperty::Kind::String) return false;
    out = UIFieldAs<std::string>(m_component->Data(), *m_prop);
    return true;
}

void UIAnimatedValue::Step(float dt) {
    if (dt <= 0.0f) { return; }
    if (Speed <= 0.0f && Smoothing <= 0.0f) { Current = Target; return; }
    if (Speed > 0.0f) {
        const float diff = Target - Current;
        const float step = Speed * dt;
        Current += std::fabs(diff) <= step ? diff : (diff > 0.0f ? step : -step);
        return;
    }
    // Экспоненциальное приближение, устойчивое к длинному кадру: наивное
    // «Current += diff * k * dt» при просадке кадра перелетает цель и звенит.
    const float k = 1.0f - std::exp(-Smoothing * dt);
    Current += (Target - Current) * k;
}

void UIBindings::Add(const std::string& propertyPath, const std::string& sourceKey, bool asText) {
    Entry e;
    e.Path = propertyPath;
    e.Key = sourceKey;
    e.AsText = asText;
    m_entries.push_back(std::move(e));
}

void UIBindings::Clear() { m_entries.clear(); }

int UIBindings::Apply(UIDocument& doc, const IUIDataSource& source) {
    int changed = 0;
    for (Entry& e : m_entries) {
        if (!e.Bound) {
            e.Bound = e.Binding.Bind(doc, e.Path);
            // Не привязалось — не ошибка кадра: узел мог ещё не появиться.
            // Пробуем снова на следующем вызове, но не спамим в лог.
            if (!e.Bound) continue;
        }
        if (e.AsText) {
            std::string value;
            if (source.Text(e.Key, value) && e.Binding.SetString(value)) ++changed;
        } else {
            float value = 0.0f;
            if (source.Number(e.Key, value) && e.Binding.Set(value)) ++changed;
        }
    }
    return changed;
}

} // namespace sage::ui
