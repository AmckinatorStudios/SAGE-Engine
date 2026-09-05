#include "sage/ui/input/UIInteraction.h"

namespace sage::ui {

const char* const* UIStateNames() {
    static const char* n[] = {"Normal", "Hovered", "Pressed", "Focused",
                              "Disabled", "Selected", "Checked"};
    return n;
}
int UIStateCount() { return 7; }

const char* const* UIHitShapeNames() {
    static const char* n[] = {"Rect", "RoundedRect", "Ellipse", "ImageAlpha", "Polygon", "None"};
    return n;
}
int UIHitShapeCount() { return 6; }

const UIComponentType& UIInteraction::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "interaction";
        d.Title = SAGE_UI_TEXT("Interaction");
        d.Hint = SAGE_UI_TEXT("Node reacts to pointer, keyboard and navigation");
        d.Icon = "cursor";
        d.Category = UIComponentCategory::Interaction;
        d.Order = 95;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIInteraction()); };
        d.Props = {
            {"Enabled", SAGE_UI_TEXT("Enabled"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIInteraction, Enabled), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"BlockRaycast", SAGE_UI_TEXT("Block raycast"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIInteraction, BlockRaycast), 0, 0,
             SAGE_UI_TEXT("Off lets clicks pass through to whatever is below"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Focusable", SAGE_UI_TEXT("Focusable"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIInteraction, Focusable), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Focus")},
            {"Draggable", SAGE_UI_TEXT("Draggable"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIInteraction, Draggable), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"ScrollTarget", SAGE_UI_TEXT("Scroll target"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIInteraction, ScrollTarget), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"AlphaThreshold", SAGE_UI_TEXT("Alpha threshold"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIInteraction, AlphaThreshold), 0.0f, 1.0f,
             SAGE_UI_TEXT("Below this opacity the node stops catching the pointer"), nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"Hit", SAGE_UI_TEXT("Hit shape"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UIInteraction, Hit), 0, 0, nullptr, UIHitShapeNames(), 6,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Hit test")},
            {"HitPadding", SAGE_UI_TEXT("Hit padding"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIInteraction, HitPadding), -64.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Hit test")},
            {"Command", SAGE_UI_TEXT("Command"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, Command), 0, 0,
             SAGE_UI_TEXT("Reported to the game; the UI never interprets it"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"NavUp", SAGE_UI_TEXT("Nav up"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, NavUp), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"NavDown", SAGE_UI_TEXT("Nav down"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, NavDown), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"NavLeft", SAGE_UI_TEXT("Nav left"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, NavLeft), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"NavRight", SAGE_UI_TEXT("Nav right"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, NavRight), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"TabIndex", SAGE_UI_TEXT("Tab index"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UIInteraction, TabIndex), 0.0f, 999.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"FocusGroup", SAGE_UI_TEXT("Focus group"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, FocusGroup), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Navigation")},
            {"Cursor", SAGE_UI_TEXT("Cursor"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, Cursor), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"TooltipKey", SAGE_UI_TEXT("Tooltip key"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIInteraction, TooltipKey), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

} // namespace sage::ui
