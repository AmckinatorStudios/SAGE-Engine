#include "sage/ui/UI.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {

// --- Раскладка -------------------------------------------------------------

glm::vec2 ResolveSize(const Transform& t, const UIRect& parent) {
    glm::vec2 size = t.Size;
    const bool stretchX =
        t.Mode == Transform::Stretch::Horizontal || t.Mode == Transform::Stretch::Both;
    const bool stretchY =
        t.Mode == Transform::Stretch::Vertical || t.Mode == Transform::Stretch::Both;
    // Растянутый элемент не может быть уже нуля: поля больше родителя дают
    // отрицательную ширину, а отрицательный прямоугольник рисуется вывернутым
    // наизнанку и ловит мышь там, где его не видно.
    if (stretchX) size.x = std::max(0.0f, parent.w - t.Margin.x - t.Margin.z);
    if (stretchY) size.y = std::max(0.0f, parent.h - t.Margin.y - t.Margin.w);
    return size;
}

UIRect Resolve(const Transform& t, const UIRect& parent, glm::vec2 size) {
    const bool stretchX =
        t.Mode == Transform::Stretch::Horizontal || t.Mode == Transform::Stretch::Both;
    const bool stretchY =
        t.Mode == Transform::Stretch::Vertical || t.Mode == Transform::Stretch::Both;

    glm::vec2 pos = ResolveAnchored(t.Anchor, t.Offset, size, parent);
    // Растянутая ось не якорится: её положение задают поля от краёв родителя.
    if (stretchX) pos.x = parent.x + t.Margin.x;
    if (stretchY) pos.y = parent.y + t.Margin.y;

    // Точка привязки внутри самого элемента. Pivot (0,0) — прежнее поведение
    // (якорь держит левый верхний угол), (0.5,0.5) — центр: подпись, растущая
    // от середины, больше не требует пересчёта Offset при каждой смене текста.
    if (!stretchX) pos.x -= size.x * t.Pivot.x;
    if (!stretchY) pos.y -= size.y * t.Pivot.y;

    return UIRect{pos.x, pos.y, size.x, size.y};
}

UIRect Resolve(const Transform& t, const UIRect& parent) {
    // LayoutSize — то, что посчитали раскладка или авто-ширина; пока его нет,
    // работает заданный размер.
    const glm::vec2 measured = (t.LayoutSize.x > 0.0f && t.LayoutSize.y > 0.0f)
                                   ? t.LayoutSize
                                   : ResolveSize(t, parent);
    return Resolve(t, parent, measured);
}

float CanvasScale(const Canvas& canvas, glm::vec2 screen) {
    if (canvas.Mode != Canvas::Scale::ScaleWithSize) return 1.0f;
    if (canvas.Reference.x <= 0.0f || canvas.Reference.y <= 0.0f) return 1.0f;
    if (screen.x <= 0.0f || screen.y <= 0.0f) return 1.0f;

    // Логарифмическое смешивание, а не линейное: интерфейс должен уменьшаться и
    // увеличиваться симметрично. При линейном среднем окно вдвое уже опорного
    // даёт масштаб 0.75, а вдвое шире — 1.5, то есть «сузили» и «расширили» на
    // одну и ту же долю дают разный по величине эффект.
    const float byWidth = std::log2(std::max(screen.x / canvas.Reference.x, 1e-4f));
    const float byHeight = std::log2(std::max(screen.y / canvas.Reference.y, 1e-4f));
    const float t = std::clamp(canvas.MatchWidthOrHeight, 0.0f, 1.0f);
    return std::exp2(byWidth * (1.0f - t) + byHeight * t);
}

UIRect MaskWindow(const Mask& mask, const UIRect& rect) {
    UIRect out;
    out.x = rect.x + mask.Padding.x;
    out.y = rect.y + mask.Padding.y;
    out.w = std::max(0.0f, rect.w - mask.Padding.x - mask.Padding.z);
    out.h = std::max(0.0f, rect.h - mask.Padding.y - mask.Padding.w);
    return out;
}

UIRect Intersect(const UIRect& a, const UIRect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    // Непересекающиеся окна дают ПУСТОЙ прямоугольник, а не отрицательный:
    // отрицательная ширина ниже по коду превращается в «обрезки нет».
    return UIRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

glm::vec2 ApplyLayout(const Layout& layout, const UIRect& container,
                      std::vector<LayoutSlot>& slots) {
    if (slots.empty()) return glm::vec2(0.0f);

    const float left = container.x + layout.Padding.x;
    const float top = container.y + layout.Padding.y;
    const float innerW = std::max(0.0f, container.w - layout.Padding.x - layout.Padding.z);
    const float innerH = std::max(0.0f, container.h - layout.Padding.y - layout.Padding.w);

    if (layout.Direction == Layout::Flow::Grid) {
        const int columns = std::max(1, layout.Columns);
        const int rows = ((int)slots.size() + columns - 1) / columns;
        // Ячейка сетки одна на всех: инвентарь с ячейками разной ширины — это
        // не сетка, а список, и притворяться сеткой ему незачем.
        const float cellW =
            (innerW - layout.Spacing * (float)(columns - 1)) / (float)columns;
        float cellH = 0.0f;
        for (const LayoutSlot& s : slots) cellH = std::max(cellH, s.Size.y);
        for (size_t i = 0; i < slots.size(); ++i) {
            const int col = (int)i % columns;
            const int row = (int)i / columns;
            slots[i].Pos = {left + (float)col * (cellW + layout.Spacing),
                            top + (float)row * (cellH + layout.Spacing)};
            if (layout.StretchCross) slots[i].Size.x = std::max(0.0f, cellW);
        }
        return glm::vec2(innerW, (float)rows * cellH + layout.Spacing * (float)(rows - 1));
    }

    const bool horizontal = layout.Direction == Layout::Flow::Horizontal;
    const float axisSpace = horizontal ? innerW : innerH;

    float content = layout.Spacing * (float)(slots.size() - 1);
    for (const LayoutSlot& s : slots) content += horizontal ? s.Size.x : s.Size.y;

    float cursor = 0.0f;
    float gap = layout.Spacing;
    switch (layout.Justify) {
        case Layout::Align::Start: break;
        case Layout::Align::Center: cursor = (axisSpace - content) * 0.5f; break;
        case Layout::Align::End: cursor = axisSpace - content; break;
        case Layout::Align::SpaceBetween:
            // Свободное место раздаётся МЕЖДУ детьми, а не по краям: именно так
            // выглядит строка «Назад ... Далее» внизу окна.
            if (slots.size() > 1) {
                float sum = 0.0f;
                for (const LayoutSlot& s : slots) sum += horizontal ? s.Size.x : s.Size.y;
                gap = std::max(0.0f, (axisSpace - sum) / (float)(slots.size() - 1));
            }
            break;
    }

    float extent = 0.0f;
    for (LayoutSlot& s : slots) {
        if (horizontal) {
            if (layout.StretchCross) s.Size.y = innerH;
            s.Pos = {left + cursor, top};
            cursor += s.Size.x + gap;
            extent = std::max(extent, s.Size.y);
        } else {
            if (layout.StretchCross) s.Size.x = innerW;
            s.Pos = {left, top + cursor};
            cursor += s.Size.y + gap;
            extent = std::max(extent, s.Size.x);
        }
    }
    const float used = cursor - gap; // последний зазор лишний
    return horizontal ? glm::vec2(used, extent) : glm::vec2(extent, used);
}

// --- Заготовки --------------------------------------------------------------

namespace {

std::vector<Preset> BuildPresets() {
    std::vector<Preset> out;

    // Ребёнок-надпись: тем, кому нужен текст на подложке, он достаётся ОБЪЕКТОМ,
    // а не полем внутри подложки. Помощником, потому что это самый частый
    // ребёнок и повторять пять присваиваний в каждой заготовке незачем.
    auto TextChild = [](const char* name, const char* text) {
        Preset child;
        child.Name = name;
        child.Xf.Anchor = UIAnchor::TopLeft;
        child.Xf.Mode = Transform::Stretch::Both;   // на всю подложку
        child.Xf.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        child.HasLabel = true;
        child.LabelStyle.Text = text;
        return child;
    };

    auto add = [&out](const char* name) -> Preset& {
        out.push_back(Preset{});
        out.back().Name = name;
        out.back().Xf.Anchor = UIAnchor::Center;
        out.back().Xf.Offset = {0.0f, 0.0f};
        return out.back();
    };

    {
        Preset& p = add("Panel");
        p.Xf.Size = {260.0f, 140.0f};
        p.HasFill = true;
    }
    {
        // Кнопка = подложка, реагирующая на мышь, ПЛЮС отдельный объект-надпись
        // внутри. Не «подложка со встроенным текстом»: надпись видно в дереве,
        // её можно подвинуть, покрасить, заменить значком или убрать.
        Preset& p = add("Button");
        p.Xf.Size = {200.0f, 52.0f};
        p.HasFill = true;
        p.FillStyle.Color = {0.16f, 0.22f, 0.34f, 0.95f};
        p.FillStyle.BorderThickness = 1.0f;
        p.HasInteractable = true; // без этого «кнопка» — просто прямоугольник
        p.Children.push_back(TextChild("Текст", "Button"));
    }
    {
        Preset& p = add("Label");
        p.Xf.Size = {220.0f, 40.0f};
        p.HasLabel = true;
        p.LabelStyle.Text = "Text";
        p.LabelStyle.AutoWidth = true;
    }
    {
        Preset& p = add("Image");
        p.Xf.Size = {160.0f, 160.0f};
        p.HasImage = true;
    }
    {
        Preset& p = add("Bar");
        p.Xf.Size = {240.0f, 26.0f};
        p.HasFill = true;
        p.FillStyle.Rounding = 6.0f;
        p.HasBar = true;
        p.BarStyle.Value = 0.6f; // пустая полоса неотличима от панели
    }
    {
        // Галка = квадратик слева ПЛЮС подпись рядом отдельным объектом.
        // Раньше подпись жила внутри и начиналась «за квадратиком» по правилу,
        // зашитому в отрисовку; теперь её можно поставить и слева, и под галкой.
        Preset& p = add("Checkbox");
        p.Xf.Size = {200.0f, 36.0f};
        p.HasInteractable = true;
        p.HasRange = true;
        p.RangeValue.Toggle = true;
        p.RangeValue.Step = 1.0f;
        p.RangeValue.Value = 0.0f;
        // Подложки (Fill) у галки НЕТ: она закрасила бы весь элемент вместе с
        // местом под подпись. Квадратик рисует сама галка своим цветом.
        p.RangeValue.BorderThickness = 1.0f;
        p.RangeValue.BorderColor = {0.55f, 0.60f, 0.72f, 0.9f};
        Preset text = TextChild("Текст", "Checkbox");
        text.Xf.Anchor = UIAnchor::CenterLeft;
        text.Xf.Offset = {44.0f, 0.0f};      // правее квадратика
        text.Xf.Size = {150.0f, 28.0f};
        text.LabelStyle.Horizontal = Label::Align::Start;
        p.Children.push_back(text);
    }
    {
        Preset& p = add("Slider");
        p.Xf.Size = {240.0f, 30.0f};
        p.HasInteractable = true;
        p.HasRange = true;
        // Дорожка и ручка — цвета САМОГО ползунка. Подложка здесь закрасила бы
        // прямоугольник во всю высоту, а дорожка тонкая и по центру.
    }
    {
        Preset& p = add("Input");
        p.Xf.Size = {260.0f, 40.0f};
        p.HasFill = true;
        p.HasLabel = true;
        p.LabelStyle.Horizontal = Label::Align::Start; // по центру набирать непривычно
        p.HasInteractable = true;
        p.HasInput = true;
        p.Input.Placeholder = "Enter text";
    }
    {
        // Новые заготовки — то, что раньше собиралось руками из пяти сущностей.
        Preset& p = add("Vertical List");
        p.Xf.Size = {280.0f, 320.0f};
        p.HasFill = true;
        p.HasMask = true; // содержимое не вылезает за края списка
        p.HasLayout = true;
        p.LayoutRule.Direction = Layout::Flow::Vertical;
    }
    {
        Preset& p = add("Toolbar");
        p.Xf.Anchor = UIAnchor::TopCenter;
        p.Xf.Size = {600.0f, 56.0f};
        p.Xf.Mode = Transform::Stretch::Horizontal;
        p.Xf.Margin = {24.0f, 16.0f, 24.0f, 0.0f};
        p.HasFill = true;
        p.HasLayout = true;
        p.LayoutRule.Direction = Layout::Flow::Horizontal;
        p.LayoutRule.Justify = Layout::Align::Center;
    }
    {
        Preset& p = add("Grid");
        p.Xf.Size = {320.0f, 320.0f};
        p.HasFill = true;
        p.HasMask = true;
        p.HasLayout = true;
        p.LayoutRule.Direction = Layout::Flow::Grid;
        p.LayoutRule.Columns = 4;
    }
    {
        Preset& p = add("Screen");
        p.Xf.Anchor = UIAnchor::TopLeft;
        p.Xf.Mode = Transform::Stretch::Both;
        p.Xf.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        p.HasFill = true;
        p.FillStyle.Color = {0.0f, 0.0f, 0.0f, 0.55f};
        p.FillStyle.Rounding = 0.0f;
    }
    return out;
}

const std::vector<Preset>& Presets() {
    static const std::vector<Preset> presets = BuildPresets();
    return presets;
}

} // namespace

const std::vector<std::string>& PresetNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Preset& p : Presets()) out.push_back(p.Name);
        return out;
    }();
    return names;
}

const Preset* FindPreset(const std::string& name) {
    for (const Preset& p : Presets()) {
        if (p.Name == name) return &p;
    }
    return nullptr;
}

} // namespace sage::ui
