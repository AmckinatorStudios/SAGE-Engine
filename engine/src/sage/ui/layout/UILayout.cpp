#include "sage/ui/layout/UILayout.h"

namespace sage::ui {

namespace {
const char* const kModeNames[] = {"None", "Horizontal", "Vertical", "Grid",  "Wrap",
                                  "Overlay", "Center",   "Stack",    "Aspect"};
} // namespace

const char* const* UIAlignNames() {
    static const char* names[] = {"Start",        "Center",      "End",        "Stretch",
                                  "SpaceBetween", "SpaceAround", "SpaceEvenly"};
    return names;
}
int UIAlignCount() { return 7; }

const UIComponentType& UILayout::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "layout";
        d.Title = SAGE_UI_TEXT("Layout");
        d.Hint = SAGE_UI_TEXT("Arranges children automatically");
        d.Icon = "layout";
        d.Category = UIComponentCategory::Layout;
        d.Order = 5;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UILayout()); };
        d.Props = {
            {"Kind", SAGE_UI_TEXT("Mode"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UILayout, Kind),
             0, 0, nullptr, kModeNames, 9, UIProperty::Widget::Auto, nullptr},
            {"Padding", SAGE_UI_TEXT("Padding"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UILayout, Padding), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Gap", SAGE_UI_TEXT("Gap"), UIProperty::Kind::Vec2, SAGE_UI_OFFSET(UILayout, Gap),
             0.0f, 200.0f, SAGE_UI_TEXT("Spacing along the main and cross axes"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Main", SAGE_UI_TEXT("Main axis"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UILayout, Main), 0, 0, nullptr, UIAlignNames(), 7,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Alignment")},
            {"Cross", SAGE_UI_TEXT("Cross axis"), UIProperty::Kind::Enum,
             SAGE_UI_OFFSET(UILayout, Cross), 0, 0, nullptr, UIAlignNames(), 7,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Alignment")},
            {"Columns", SAGE_UI_TEXT("Columns"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UILayout, Columns), 1.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Grid")},
            {"CellSize", SAGE_UI_TEXT("Cell size"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UILayout, CellSize), 0, 0,
             SAGE_UI_TEXT("Zero derives the cell from the container"), nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Grid")},
            {"StackOffset", SAGE_UI_TEXT("Stack offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UILayout, StackOffset), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Stack")},
            {"FitWidth", SAGE_UI_TEXT("Fit width"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UILayout, FitWidth), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Fit")},
            {"FitHeight", SAGE_UI_TEXT("Fit height"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UILayout, FitHeight), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Fit")},
            {"Reverse", SAGE_UI_TEXT("Reverse"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UILayout, Reverse), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

} // namespace sage::ui
