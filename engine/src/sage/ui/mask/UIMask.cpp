#include "sage/ui/mask/UIMask.h"

namespace sage::ui {

const char* const* UIMaskShapeNames() {
    static const char* names[] = {"Rect", "RoundedRect", "Ellipse", "Texture", "Gradient"};
    return names;
}
int UIMaskShapeCount() { return 5; }

const char* const* UIMaskComposeNames() {
    static const char* names[] = {"Intersect", "Replace", "Subtract", "Add", "Multiply"};
    return names;
}
int UIMaskComposeCount() { return 5; }

namespace {
const char* const kChannelNames[] = {"Alpha", "Red", "Green", "Blue", "Luminance"};
}

const UIComponentType& UIMask::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "mask";
        d.Title = SAGE_UI_TEXT("Mask");
        d.Hint = SAGE_UI_TEXT("Limits the whole subtree to a shape");
        d.Icon = "mask";
        d.Category = UIComponentCategory::Mask;
        d.Order = 4;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIMask()); };
        d.Props = {
            {"Form", SAGE_UI_TEXT("Shape"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIMask, Form),
             0, 0, nullptr, UIMaskShapeNames(), 5, UIProperty::Widget::Auto, nullptr},
            {"Mode", SAGE_UI_TEXT("Compose"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIMask, Mode),
             0, 0, SAGE_UI_TEXT("How this mask combines with the parent masks"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIMask, Radius), 0.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Padding", SAGE_UI_TEXT("Padding"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIMask, Padding), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Softness", SAGE_UI_TEXT("Softness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIMask, Softness), 0.0f, 64.0f,
             SAGE_UI_TEXT("Soft edge width in pixels"), nullptr, 0, UIProperty::Widget::Slider,
             nullptr},
            {"Invert", SAGE_UI_TEXT("Invert"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIMask, Invert), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"TexturePath", SAGE_UI_TEXT("Mask texture"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIMask, TexturePath), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Texture, SAGE_UI_TEXT("Texture")},
            {"Source", SAGE_UI_TEXT("Channel"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIMask, Source), 0, 0, nullptr, kChannelNames, 5,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Texture")},
            {"GradientAngle", SAGE_UI_TEXT("Gradient angle"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIMask, GradientAngle), -360.0f, 360.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Angle, SAGE_UI_TEXT("Gradient")},
            {"GradientStart", SAGE_UI_TEXT("Gradient start"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIMask, GradientStart), 0.0f, 1.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Gradient")},
            {"GradientEnd", SAGE_UI_TEXT("Gradient end"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIMask, GradientEnd), 0.0f, 1.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Gradient")},
            {"ShowOutside", SAGE_UI_TEXT("Show outside"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIMask, ShowOutside), 0, 0,
             SAGE_UI_TEXT("Mark the area without actually clipping"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"AffectsHitTest", SAGE_UI_TEXT("Affects hit test"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIMask, AffectsHitTest), 0, 0,
             SAGE_UI_TEXT("Clicks outside the mask do not reach the subtree"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

} // namespace sage::ui
