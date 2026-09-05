// ---------------------------------------------------------------------------
// Описания визуальных компонентов: заливка, рамка, форма, изображение.
//
// Каждый описывает СЕБЯ САМ — таблицей свойств. По ней потом работают запись в
// файл, чтение, инспектор редактора и адресация свойств для анимации. Ни один
// из этих четверых не содержит списка полей: список ровно один, и он здесь.
// ---------------------------------------------------------------------------
#include "sage/ui/core/UIContext.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIShape.h"

#include "sage/render/Texture.h"

namespace sage::ui {

namespace {
const char* const kFillKinds[] = {"Solid", "Gradient", "Texture"};
const char* const kFitNames[] = {"Stretch", "Contain", "Cover", "None", "Tile"};
} // namespace

const UIComponentType& UIFill::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "fill";
        d.Title = SAGE_UI_TEXT("Fill");
        d.Hint = SAGE_UI_TEXT("Background of the node");
        d.Icon = "square";
        d.Category = UIComponentCategory::Appearance;
        d.Order = 10;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIFill()); };
        d.Props = {
            {"Type", SAGE_UI_TEXT("Type"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIFill, Type), 0,
             0, nullptr, kFillKinds, 3, UIProperty::Widget::Auto, nullptr},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIFill, Color), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Gradient", SAGE_UI_TEXT("Gradient"), UIProperty::Kind::Gradient,
             SAGE_UI_OFFSET(UIFill, Gradient), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIFill, Radius), 0.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Softness", SAGE_UI_TEXT("Softness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIFill, Softness), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"TexturePath", SAGE_UI_TEXT("Texture"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIFill, TexturePath), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Texture, SAGE_UI_TEXT("Texture")},
            {"TextureScale", SAGE_UI_TEXT("Texture scale"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIFill, TextureScale), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Texture")},
            {"TextureOffset", SAGE_UI_TEXT("Texture offset"), UIProperty::Kind::Vec2,
             SAGE_UI_OFFSET(UIFill, TextureOffset), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Texture")},
            {"Repeat", SAGE_UI_TEXT("Repeat"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIFill, Repeat), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Texture")},
        };
        return d;
    }();
    return t;
}

const UIComponentType& UIBorder::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "border";
        d.Title = SAGE_UI_TEXT("Border");
        d.Hint = SAGE_UI_TEXT("Outline around the node");
        d.Icon = "frame";
        d.Category = UIComponentCategory::Appearance;
        d.Order = 80;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIBorder()); };
        d.Props = {
            {"Thickness", SAGE_UI_TEXT("Thickness"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIBorder, Thickness), 0.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIBorder, Color), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Gradient", SAGE_UI_TEXT("Gradient"), UIProperty::Kind::Gradient,
             SAGE_UI_OFFSET(UIBorder, Gradient), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIBorder, Radius), -1.0f, 200.0f,
             SAGE_UI_TEXT("Negative means take the radius from the fill"), nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Inset", SAGE_UI_TEXT("Inset"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIBorder, Inset), -32.0f, 32.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
        };
        return d;
    }();
    return t;
}

const char* const* UIShapeKindNames() {
    static const char* names[] = {"Rectangle", "RoundedRect", "Circle", "Ellipse", "Line",
                                  "Triangle",  "Polygon",     "Arc",    "Ring"};
    return names;
}
int UIShapeKindCount() { return 9; }

const UIComponentType& UIShape::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "shape";
        d.Title = SAGE_UI_TEXT("Shape");
        d.Hint = SAGE_UI_TEXT("Procedural vector shape");
        d.Icon = "circle";
        d.Category = UIComponentCategory::Appearance;
        d.Order = 25;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIShape()); };
        d.Props = {
            {"Type", SAGE_UI_TEXT("Shape"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIShape, Type),
             0, 0, nullptr, UIShapeKindNames(), 9, UIProperty::Widget::Auto, nullptr},
            {"Color", SAGE_UI_TEXT("Color"), UIProperty::Kind::Color,
             SAGE_UI_OFFSET(UIShape, Color), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Gradient", SAGE_UI_TEXT("Gradient"), UIProperty::Kind::Gradient,
             SAGE_UI_OFFSET(UIShape, Gradient), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIShape, Radius), 0.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Thickness", SAGE_UI_TEXT("Thickness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIShape, Thickness), 0.0f, 64.0f,
             SAGE_UI_TEXT("Zero fills the shape; above zero draws an outline"), nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
            {"StartAngle", SAGE_UI_TEXT("Start angle"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIShape, StartAngle), -360.0f, 360.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Angle, SAGE_UI_TEXT("Arc")},
            {"SweepAngle", SAGE_UI_TEXT("Sweep angle"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIShape, SweepAngle), -360.0f, 360.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Angle, SAGE_UI_TEXT("Arc")},
            {"Sides", SAGE_UI_TEXT("Sides"), UIProperty::Kind::Int,
             SAGE_UI_OFFSET(UIShape, Sides), 3.0f, 24.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Polygon")},
            {"Softness", SAGE_UI_TEXT("Softness"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIShape, Softness), 0.0f, 64.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, nullptr},
        };
        return d;
    }();
    return t;
}

const char* const* UIImageFitNames() { return kFitNames; }
int UIImageFitCount() { return 5; }

const UIComponentType& UIImage::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "image";
        d.Title = SAGE_UI_TEXT("Image");
        d.Hint = SAGE_UI_TEXT("Texture, sprite or nine-slice");
        d.Icon = "image";
        d.Category = UIComponentCategory::Image;
        d.Order = 20;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIImage()); };
        d.Props = {
            {"Path", SAGE_UI_TEXT("Texture"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIImage, Path), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Texture,
             nullptr},
            {"Atlas", SAGE_UI_TEXT("Atlas"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIImage, Atlas), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Sprite")},
            {"Sprite", SAGE_UI_TEXT("Sprite"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIImage, Sprite), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Sprite")},
            {"SourceRect", SAGE_UI_TEXT("Source rect"), UIProperty::Kind::Vec4,
             SAGE_UI_OFFSET(UIImage, SourceRect), 0, 0,
             SAGE_UI_TEXT("Region in source pixels; zero size means the whole file"), nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Sprite")},
            {"Tint", SAGE_UI_TEXT("Tint"), UIProperty::Kind::Color, SAGE_UI_OFFSET(UIImage, Tint),
             0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto, nullptr},
            {"Fit", SAGE_UI_TEXT("Fit"), UIProperty::Kind::Enum, SAGE_UI_OFFSET(UIImage, Fit), 0,
             0, nullptr, kFitNames, 5, UIProperty::Widget::Auto, nullptr},
            {"Radius", SAGE_UI_TEXT("Corner radius"), UIProperty::Kind::Corners,
             SAGE_UI_OFFSET(UIImage, Radius), 0.0f, 200.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"Slice", SAGE_UI_TEXT("Nine-slice"), UIProperty::Kind::Edges,
             SAGE_UI_OFFSET(UIImage, Slice), 0.0f, 256.0f,
             SAGE_UI_TEXT("Fixed corners in source pixels"), nullptr, 0, UIProperty::Widget::Auto,
             SAGE_UI_TEXT("Nine-slice")},
            {"SliceScale", SAGE_UI_TEXT("Slice scale"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIImage, SliceScale), 0.0f, 8.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Slider, SAGE_UI_TEXT("Nine-slice")},
            {"SliceShrink", SAGE_UI_TEXT("Shrink corners"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIImage, SliceShrink), 0, 0,
             SAGE_UI_TEXT("Scale corners down when the node is smaller than they are"), nullptr,
             0, UIProperty::Widget::Auto, SAGE_UI_TEXT("Nine-slice")},
            {"FlipX", SAGE_UI_TEXT("Flip X"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIImage, FlipX), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"FlipY", SAGE_UI_TEXT("Flip Y"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIImage, FlipY), 0, 0, nullptr, nullptr, 0, UIProperty::Widget::Auto,
             nullptr},
            {"Rotation", SAGE_UI_TEXT("Rotation"), UIProperty::Kind::Float,
             SAGE_UI_OFFSET(UIImage, Rotation), -360.0f, 360.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Angle, nullptr},
            {"PixelArt", SAGE_UI_TEXT("Pixel art"), UIProperty::Kind::Bool,
             SAGE_UI_OFFSET(UIImage, PixelArt), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
            {"UvRect", SAGE_UI_TEXT("UV rect"), UIProperty::Kind::Vec4,
             SAGE_UI_OFFSET(UIImage, UvRect), 0.0f, 1.0f, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, SAGE_UI_TEXT("Advanced")},
        };
        return d;
    }();
    return t;
}

glm::vec2 UIImage::Measure(const UIContext& ctx, const UINode& node, glm::vec2 available) const {
    (void)node;
    (void)available;
    // Размер картинки по содержимому — её собственный размер в пикселях
    // исходника (или размер спрайта). Иначе «панель по содержимому с иконкой
    // внутри» схлопывается в ноль, и иконки не видно вовсе.
    if (SourceRect.z > 0.0f && SourceRect.w > 0.0f) return {SourceRect.z, SourceRect.w};
    if (!ctx.Textures || Path.empty()) return glm::vec2(0.0f);
    const glm::ivec2 size = ctx.Textures->Size(Path);
    return {(float)size.x, (float)size.y};
}

} // namespace sage::ui
