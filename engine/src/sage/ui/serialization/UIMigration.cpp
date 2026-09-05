#include "sage/ui/serialization/UIMigration.h"

#include <algorithm>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"
#include "sage/ui/visual/UIIcon.h"

namespace sage::ui {

namespace {

// Старый якорь → доли. Девять точек — частный случай долей, поэтому перевод
// однозначен и не теряет ничего.
glm::vec2 AnchorFraction(UIAnchor a) {
    float x = 0.0f, y = 0.0f;
    switch (a) {
        case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft: x = 0.0f; break;
        case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter: x = 0.5f; break;
        default: x = 1.0f; break;
    }
    switch (a) {
        case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight: y = 0.0f; break;
        case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight: y = 0.5f; break;
        default: y = 1.0f; break;
    }
    return {x, y};
}

UITextAlign AlignH(Label::Align a) {
    switch (a) {
        case Label::Align::Start: return UITextAlign::Left;
        case Label::Align::End: return UITextAlign::Right;
        default: return UITextAlign::Center;
    }
}
UITextVAlign AlignV(Label::Align a) {
    switch (a) {
        case Label::Align::Start: return UITextVAlign::Top;
        case Label::Align::End: return UITextVAlign::Bottom;
        default: return UITextVAlign::Center;
    }
}

UIProgress::Direction BarDir(Bar::Direction d) {
    switch (d) {
        case Bar::Direction::RightToLeft: return UIProgress::Direction::RightToLeft;
        case Bar::Direction::BottomToTop: return UIProgress::Direction::BottomToTop;
        case Bar::Direction::TopToBottom: return UIProgress::Direction::TopToBottom;
        default: return UIProgress::Direction::LeftToRight;
    }
}

void ConvertNode(Scene& scene, entt::registry& reg, entt::entity src, UIDocument& doc,
                 UINodeId parent, UIMigrationReport& report);

void ConvertChildren(Scene& scene, entt::registry& reg, entt::entity src, UIDocument& doc,
                     UINodeId parent, UIMigrationReport& report) {
    // Дети берутся из иерархии сцены. Порядок — тот же, что был: старая система
    // сортировала соседей по Layer, и новый Order его сохраняет.
    reg.view<HierarchyComponent>().each([&](entt::entity e, const HierarchyComponent& h) {
        if (h.Parent != src) return;
        if (!reg.all_of<sage::ui::Transform>(e)) return;
        ConvertNode(scene, reg, e, doc, parent, report);
    });
}

void ConvertNode(Scene& scene, entt::registry& reg, entt::entity src, UIDocument& doc,
                 UINodeId parent, UIMigrationReport& report) {
    const sage::ui::Transform* old = reg.try_get<sage::ui::Transform>(src);
    if (!old) return;

    std::string name = "Element";
    if (const NameComponent* n = reg.try_get<NameComponent>(src)) name = n->Name;

    UINode& node = *doc.Create(name, parent);
    ++report.Nodes;
    node.Visible = old->Visible;
    node.Layer = 0;
    node.Order = old->Layer; // старый Layer был «порядком среди соседей»

    UITransform& t = node.Ensure<UITransform>();
    const glm::vec2 anchor = AnchorFraction(old->Anchor);
    t.AnchorMin = t.AnchorMax = anchor;
    // Опорная точка = точка якоря плюс старый «сдвиг на долю размера»: старая
    // формула складывала их ровно так, и без этого элементы уехали бы.
    t.Pivot = anchor + old->Pivot;
    t.Offset = old->Offset;
    t.Size = old->Size;
    t.Margin = UIEdges(old->Margin.x, old->Margin.y, old->Margin.z, old->Margin.w);
    switch (old->Mode) {
        case sage::ui::Transform::Stretch::Horizontal: t.SetStretch(true, false); break;
        case sage::ui::Transform::Stretch::Vertical: t.SetStretch(false, true); break;
        case sage::ui::Transform::Stretch::Both: t.SetStretch(true, true); break;
        default: break;
    }
    ++report.Components;

    if (const Group* g = reg.try_get<Group>(src)) {
        node.Opacity = g->Alpha;
        if (!g->BlockRaycasts) {
            UIInteraction& ia = node.Ensure<UIInteraction>();
            ia.BlockRaycast = false;
        }
    }

    if (const Fill* f = reg.try_get<Fill>(src)) {
        UIFill& fill = node.Ensure<UIFill>();
        fill.Color = f->Color;
        fill.Radius = UICorners(f->Rounding);
        if (f->Gradient.a > 0.0f) {
            fill.Type = UIFill::Kind::Gradient;
            fill.Gradient = UIGradient::TwoColor(f->Color, f->Gradient, 0.0f);
        }
        if (f->BorderThickness > 0.0f) {
            // Рамка была двумя полями внутри заливки — теперь это свой
            // компонент, и у неё появляются собственные скругления и стороны.
            UIBorder& b = node.Ensure<UIBorder>();
            b.Thickness = UIEdges::Uniform(f->BorderThickness);
            b.Color = f->BorderColor;
        }
        if (f->ShadowSize > 0.0f) {
            // Тень была парой полей — теперь это эффект в стеке, и рядом с ней
            // можно поставить вторую, свечение или размытие.
            UIDropShadow& s = node.Ensure<UIEffects>().Ensure<UIDropShadow>();
            s.Blur = f->ShadowSize;
            s.Offset = {0.0f, f->ShadowSize * 0.35f};
            s.Color = f->ShadowColor;
        }
        ++report.Components;
    }

    if (const Label* l = reg.try_get<Label>(src)) {
        UIText& text = node.Ensure<UIText>();
        text.Text = l->Text;
        // Старый «масштаб» был множителем к номинальным восьми пикселям.
        text.Size = l->Scale * 8.0f;
        text.Color = l->Color;
        text.Align = AlignH(l->Horizontal);
        text.VAlign = AlignV(l->Vertical);
        text.Wrap = l->Wrap ? UITextWrap::Word : UITextWrap::None;
        text.Padding = UIEdges(l->PadX, 0.0f, l->PadX, 0.0f);
        if (l->AutoWidth) {
            t.WidthMode = UISizeMode::Content;
            report.Warnings.push_back(name + ": авто-ширина стала размером по содержимому");
        }
        ++report.Components;
    }

    if (const Image* im = reg.try_get<Image>(src)) {
        UIImage& img = node.Ensure<UIImage>();
        img.Path = im->Path;
        img.Tint = im->Tint;
        img.SourceRect = im->Sprite;
        img.Slice = UIEdges(im->SliceBorder.x, im->SliceBorder.y, im->SliceBorder.z,
                            im->SliceBorder.w);
        img.SliceScale = im->PixelScale;
        img.PixelArt = im->PixelArt;
        if (im->SpriteHover.z > 0.0f || im->SpritePressed.z > 0.0f) {
            // Спрайты состояний в новой системе — дело СТИЛЯ состояния, а не
            // поля картинки: так их можно задать любому свойству, а не только
            // спрайту.
            report.Warnings.push_back(
                name + ": спрайты состояний не перенесены — задайте их стилем состояния");
        }
        ++report.Components;
    }

    if (const Bar* b = reg.try_get<Bar>(src)) {
        UIProgress& p = node.Ensure<UIProgress>();
        p.Value = b->Value;
        p.FillColor = b->FillColor;
        p.TrackColor = UIColor(0.0f, 0.0f, 0.0f, 0.0f); // дорожкой была подложка
        p.Grow = BarDir(b->Grow);
        p.Smoothing = b->Smoothing;
        ++report.Components;
    }

    if (const Icon* ic = reg.try_get<Icon>(src)) {
        UIIcon& icon = node.Ensure<UIIcon>();
        icon.Name = ic->Name;
        icon.Color = ic->Color;
        icon.Size = ic->Size;
        ++report.Components;
    }

    if (const sage::ui::Mask* m = reg.try_get<sage::ui::Mask>(src)) {
        UIMask& mask = node.Ensure<UIMask>();
        mask.Form = m->Form == sage::ui::Mask::Shape::RoundedRect ? UIMask::Shape::RoundedRect
                                                                  : UIMask::Shape::Rect;
        float rounding = m->Rounding;
        if (rounding < 0.0f) {
            if (const Fill* f = reg.try_get<Fill>(src)) rounding = f->Rounding;
            else rounding = 0.0f;
        }
        mask.Radius = UICorners(rounding);
        mask.Padding = UIEdges(m->Padding.x, m->Padding.y, m->Padding.z, m->Padding.w);
        mask.ShowOutside = m->ShowOutside;
        ++report.Components;
    }

    if (const sage::ui::Layout* l = reg.try_get<sage::ui::Layout>(src)) {
        UILayout& layout = node.Ensure<UILayout>();
        switch (l->Direction) {
            case sage::ui::Layout::Flow::Horizontal: layout.Kind = UILayout::Mode::Horizontal; break;
            case sage::ui::Layout::Flow::Grid: layout.Kind = UILayout::Mode::Grid; break;
            default: layout.Kind = UILayout::Mode::Vertical; break;
        }
        switch (l->Justify) {
            case sage::ui::Layout::Align::Center: layout.Main = UIAlign::Center; break;
            case sage::ui::Layout::Align::End: layout.Main = UIAlign::End; break;
            case sage::ui::Layout::Align::SpaceBetween: layout.Main = UIAlign::SpaceBetween; break;
            default: layout.Main = UIAlign::Start; break;
        }
        layout.Cross = l->StretchCross ? UIAlign::Stretch : UIAlign::Start;
        layout.Gap = {l->Spacing, l->Spacing};
        layout.Padding = UIEdges(l->Padding.x, l->Padding.y, l->Padding.z, l->Padding.w);
        layout.Columns = l->Columns;
        layout.FitWidth = layout.FitHeight = l->FitContent;
        if (l->FitContent) {
            t.WidthMode = UISizeMode::Content;
            t.HeightMode = UISizeMode::Content;
        }
        ++report.Components;
    }

    if (const Interactable* it = reg.try_get<Interactable>(src)) {
        UIInteraction& ia = node.Ensure<UIInteraction>();
        ia.Enabled = it->Enabled;
        ia.Cursor = it->Cursor;
        ia.Command = it->Action;
        ia.Focusable = true;
        ia.Hit = UIHitShape::RoundedRect;
        if (!it->Events.empty()) {
            // Связи «событие → метод объекта» — часть игровой сцены, а не
            // документа интерфейса: новый UI сообщает КОМАНДУ, а кто её слушает,
            // решает игра (§102, §103).
            report.Warnings.push_back(
                name + ": связи событий не перенесены — подпишитесь на команду \"" +
                it->Action + "\" в игре");
        }
        ++report.Components;
    }

    if (const sage::ui::Range* r = reg.try_get<sage::ui::Range>(src)) {
        UIRangeValue& range = node.Ensure<UIRangeValue>();
        range.Min = r->Min;
        range.Max = r->Max;
        range.Value = r->Value;
        range.Step = r->Step;
        range.Toggle = r->Toggle;
        range.TrackColor = r->TrackColor;
        range.AccentColor = r->AccentColor;
        range.Radius = UICorners(r->Rounding);
        ++report.Components;
    }

    if (const sage::ui::TextInput* ti = reg.try_get<sage::ui::TextInput>(src)) {
        UITextField& field = node.Ensure<UITextField>();
        field.PlaceholderKey = ti->Placeholder;
        field.MaxLength = ti->MaxLength;
        field.Password = ti->Password;
        field.ReadOnly = ti->ReadOnly;
        if (const Label* l = reg.try_get<Label>(src)) field.Value = l->Text;
        ++report.Components;
    }

    ConvertChildren(scene, reg, src, doc, node.Id, report);
}

} // namespace

UIMigrationReport UIMigrateSceneUI(Scene& scene, UIDocument& doc) {
    UIInitialize();
    UIMigrationReport report;
    doc.Clear();

    entt::registry& reg = scene.Registry();

    // Настройки холста берутся у ПЕРВОГО найденного корня-холста: в старой
    // системе их мог нести любой элемент, и «главным» был тот, что рисовался
    // первым.
    bool canvasTaken = false;
    reg.view<sage::ui::Canvas>().each([&](entt::entity e, const sage::ui::Canvas& c) {
        (void)e;
        if (canvasTaken) return;
        canvasTaken = true;
        UICanvasSettings& canvas = doc.Canvas();
        canvas.Scale = c.Mode == sage::ui::Canvas::Scale::ScaleWithSize
                           ? UICanvasSettings::ScaleMode::ScaleWithSize
                           : UICanvasSettings::ScaleMode::Pixels;
        canvas.Reference = c.Reference;
        canvas.MatchWidthOrHeight = c.MatchWidthOrHeight;
        canvas.SortOrder = c.SortOrder;
    });

    // Корни: элементы без родителя-элемента.
    reg.view<sage::ui::Transform>().each([&](entt::entity e, const sage::ui::Transform&) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        const bool rooted = !h || h->Parent == entt::null ||
                            !reg.all_of<sage::ui::Transform>(h->Parent);
        if (rooted) ConvertNode(scene, reg, e, doc, kUIInvalidNode, report);
    });

    doc.MarkDirty(UIDirty_All);
    return report;
}

} // namespace sage::ui
