#include "sage/ui/layout/UITransform.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {

namespace {
const char* const kSizeModeNames[] = {"Fixed", "Percent", "Content", "Stretch"};
const char* const kAspectNames[] = {"None", "WidthFromHeight", "HeightFromWidth", "FitInside",
                                    "FillOutside"};
} // namespace

const UIComponentType& UITransform::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "transform";
        d.Title = SAGE_UI_TEXT("Transform");
        d.Hint = SAGE_UI_TEXT("Where the node sits inside its parent");
        d.Icon = "move";
        d.Category = UIComponentCategory::Transform;
        d.Order = 0;
        d.Essential = true; // §6: узел без прямоугольника негде рисовать
        d.Create = [] { return std::unique_ptr<UIComponent>(new UITransform()); };
        d.Props = {
            {"AnchorMin", SAGE_UI_TEXT("Anchor min"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, AnchorMin), 0.0f, 1.0f,
             SAGE_UI_TEXT("Anchor corner in parent fractions"), nullptr, 0,
             UIProperty::Widget::Anchor, SAGE_UI_TEXT("Anchors")},
            {"AnchorMax", SAGE_UI_TEXT("Anchor max"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, AnchorMax), 0.0f, 1.0f,
             SAGE_UI_TEXT("Equal to min means a point anchor; different means stretch"),
             nullptr, 0, UIProperty::Widget::Anchor, SAGE_UI_TEXT("Anchors")},
            {"Offset", SAGE_UI_TEXT("Offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Offset), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Size", SAGE_UI_TEXT("Size"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Size), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"WidthMode", SAGE_UI_TEXT("Width mode"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UITransform, WidthMode), 0, 0, nullptr, kSizeModeNames, 4,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Sizing")},
            {"HeightMode", SAGE_UI_TEXT("Height mode"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UITransform, HeightMode), 0, 0, nullptr, kSizeModeNames, 4,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Sizing")},
            {"Percent", SAGE_UI_TEXT("Percent"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Percent), 0.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Sizing")},
            {"Margin", SAGE_UI_TEXT("Margin"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UITransform, Margin), 0, 0,
             SAGE_UI_TEXT("Insets used when the axis is stretched"), nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Sizing")},
            {"MinSize", SAGE_UI_TEXT("Min size"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, MinSize), 0, 0, SAGE_UI_TEXT("Zero means unlimited"),
             nullptr, 0, UIProperty::Widget::Auto, SAGE_UI_TEXT("Constraints")},
            {"MaxSize", SAGE_UI_TEXT("Max size"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, MaxSize), 0, 0, SAGE_UI_TEXT("Zero means unlimited"),
             nullptr, 0, UIProperty::Widget::Auto, SAGE_UI_TEXT("Constraints")},
            {"AspectRatio", SAGE_UI_TEXT("Aspect ratio"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UITransform, AspectRatio), 0.0f, 8.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Constraints")},
            {"AspectMode", SAGE_UI_TEXT("Aspect mode"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UITransform, Aspect), 0, 0, nullptr, kAspectNames, 5,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Constraints")},
            {"Pivot", SAGE_UI_TEXT("Pivot"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Pivot), 0.0f, 1.0f,
             SAGE_UI_TEXT("Node point that lands on the anchor and rotates in place"),
             nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Scale", SAGE_UI_TEXT("Scale"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Scale), 0.0f, 4.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Transform")},
            {"Rotation", SAGE_UI_TEXT("Rotation"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UITransform, Rotation), -360.0f, 360.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Angle, SAGE_UI_TEXT("Transform")},
            {"Skew", SAGE_UI_TEXT("Skew"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Skew), -80.0f, 80.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Transform")},
            {"Translate", SAGE_UI_TEXT("Translate"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UITransform, Translate), 0, 0,
             SAGE_UI_TEXT("Shift applied after layout — for animation"), nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Transform")},
            {"IgnoreLayout", SAGE_UI_TEXT("Ignore layout"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UITransform, IgnoreLayout), 0, 0,
             SAGE_UI_TEXT("Positioned by anchors even inside a layout container"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

void UITransform::SetAnchorPoint(UIAnchor anchor) {
    float x = 0.0f, y = 0.0f;
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft: x = 0.0f; break;
        case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter: x = 0.5f; break;
        default: x = 1.0f; break;
    }
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight: y = 0.0f; break;
        case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight: y = 0.5f; break;
        default: y = 1.0f; break;
    }
    AnchorMin = AnchorMax = glm::vec2(x, y);
    // Опорная точка едет вместе с якорем: «прижать к правому краю» без этого
    // означало бы «поставить левый край узла на правый край родителя», то есть
    // выкинуть узел за экран.
    Pivot = glm::vec2(x, y);
    if (WidthMode == UISizeMode::Stretch) WidthMode = UISizeMode::Fixed;
    if (HeightMode == UISizeMode::Stretch) HeightMode = UISizeMode::Fixed;
}

void UITransform::SetStretch(bool horizontal, bool vertical) {
    if (horizontal) {
        AnchorMin.x = 0.0f;
        AnchorMax.x = 1.0f;
        WidthMode = UISizeMode::Stretch;
    }
    if (vertical) {
        AnchorMin.y = 0.0f;
        AnchorMax.y = 1.0f;
        HeightMode = UISizeMode::Stretch;
    }
}

UIRect UIAnchorRect(const UITransform& t, const UIRect& parent) {
    const float x0 = parent.x + parent.w * t.AnchorMin.x;
    const float x1 = parent.x + parent.w * t.AnchorMax.x;
    const float y0 = parent.y + parent.h * t.AnchorMin.y;
    const float y1 = parent.y + parent.h * t.AnchorMax.y;
    return UIRect{std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0)};
}

glm::mat3 UILocalMatrix(const UITransform& t, const UIRect& rect) {
    // Единичная матрица — самый частый случай, и он обязан быть бесплатным:
    // подавляющее большинство узлов не повёрнуты и не масштабированы.
    const bool identity = t.Rotation == 0.0f && t.Skew.x == 0.0f && t.Skew.y == 0.0f &&
                          t.Scale.x == 1.0f && t.Scale.y == 1.0f;
    if (identity) return glm::mat3(1.0f);

    const glm::vec2 pivot{rect.x + rect.w * t.Pivot.x, rect.y + rect.h * t.Pivot.y};
    const float rad = t.Rotation * 3.14159265358979f / 180.0f;
    const float c = std::cos(rad), s = std::sin(rad);
    const float kx = std::tan(t.Skew.x * 3.14159265358979f / 180.0f);
    const float ky = std::tan(t.Skew.y * 3.14159265358979f / 180.0f);

    // Порядок: перенос в опорную точку → поворот → наклон → масштаб → обратно.
    // Именно такой, потому что человек ждёт, что поворот идёт ВОКРУГ опорной
    // точки, а масштаб — вдоль повёрнутых осей.
    glm::mat3 m(1.0f);
    m[0][0] = (c - s * ky) * t.Scale.x;
    m[1][0] = (c * kx - s) * t.Scale.y;
    m[0][1] = (s + c * ky) * t.Scale.x;
    m[1][1] = (s * kx + c) * t.Scale.y;
    m[2][0] = pivot.x - (m[0][0] * pivot.x + m[1][0] * pivot.y);
    m[2][1] = pivot.y - (m[0][1] * pivot.x + m[1][1] * pivot.y);
    return m;
}

} // namespace sage::ui
