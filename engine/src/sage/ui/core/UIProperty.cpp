#include "sage/ui/core/UIProperty.h"

namespace sage::ui {

int UIPropertyFloatCount(const UIProperty& p) {
    switch (p.Type) {
        case UIProperty::Kind::Float:
        case UIProperty::Kind::Int:
        case UIProperty::Kind::Enum:
        case UIProperty::Kind::Bool: return 1;
        case UIProperty::Kind::Vec2: return 2;
        case UIProperty::Kind::Vec4:
        case UIProperty::Kind::Color:
        case UIProperty::Kind::Edges:
        case UIProperty::Kind::Corners: return 4;
        default: return 0;
    }
}

bool UIPropertyGetFloat(const void* data, const UIProperty& p, int component, float& out) {
    const int n = UIPropertyFloatCount(p);
    if (n == 0) return false;
    if (component < 0) component = 0;
    if (component >= n) return false;

    switch (p.Type) {
        case UIProperty::Kind::Bool: out = UIFieldAs<bool>(data, p) ? 1.0f : 0.0f; return true;
        case UIProperty::Kind::Int:
        case UIProperty::Kind::Enum: out = (float)UIFieldAs<int>(data, p); return true;
        case UIProperty::Kind::Float: out = UIFieldAs<float>(data, p); return true;
        case UIProperty::Kind::Vec2: out = UIFieldAs<glm::vec2>(data, p)[component]; return true;
        case UIProperty::Kind::Vec4:
        case UIProperty::Kind::Color: out = UIFieldAs<glm::vec4>(data, p)[component]; return true;
        case UIProperty::Kind::Edges: {
            const UIEdges& e = UIFieldAs<UIEdges>(data, p);
            out = component == 0 ? e.L : component == 1 ? e.T : component == 2 ? e.R : e.B;
            return true;
        }
        case UIProperty::Kind::Corners: {
            const UICorners& c = UIFieldAs<UICorners>(data, p);
            out = component == 0 ? c.TL : component == 1 ? c.TR : component == 2 ? c.BR : c.BL;
            return true;
        }
        default: return false;
    }
}

bool UIPropertySetFloat(void* data, const UIProperty& p, int component, float value) {
    const int n = UIPropertyFloatCount(p);
    if (n == 0) return false;
    if (component < 0) component = 0;
    if (component >= n) return false;

    switch (p.Type) {
        case UIProperty::Kind::Bool: UIFieldAs<bool>(data, p) = value >= 0.5f; return true;
        case UIProperty::Kind::Int:
        case UIProperty::Kind::Enum: UIFieldAs<int>(data, p) = (int)(value + (value < 0 ? -0.5f : 0.5f)); return true;
        case UIProperty::Kind::Float: UIFieldAs<float>(data, p) = value; return true;
        case UIProperty::Kind::Vec2: UIFieldAs<glm::vec2>(data, p)[component] = value; return true;
        case UIProperty::Kind::Vec4:
        case UIProperty::Kind::Color: UIFieldAs<glm::vec4>(data, p)[component] = value; return true;
        case UIProperty::Kind::Edges: {
            UIEdges& e = UIFieldAs<UIEdges>(data, p);
            (component == 0 ? e.L : component == 1 ? e.T : component == 2 ? e.R : e.B) = value;
            return true;
        }
        case UIProperty::Kind::Corners: {
            UICorners& c = UIFieldAs<UICorners>(data, p);
            (component == 0 ? c.TL : component == 1 ? c.TR : component == 2 ? c.BR : c.BL) = value;
            return true;
        }
        default: return false;
    }
}

} // namespace sage::ui
