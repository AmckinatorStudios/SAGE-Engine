#include <utility>
#include "sage/ui/UIPart.h"   // SAGE_UI_TEXT: строки для человека помечены и для сборщика переводов
#include "sage/ui/UI.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {

// --- Раскладка -------------------------------------------------------------

glm::vec2 ResolveSize(const Element& t, const UIRect& parent) {
    glm::vec2 size = t.Size;
    const bool stretchX =
        t.Mode == Element::Stretch::Horizontal || t.Mode == Element::Stretch::Both;
    const bool stretchY =
        t.Mode == Element::Stretch::Vertical || t.Mode == Element::Stretch::Both;
    // Растянутый элемент не может быть уже нуля: поля больше родителя дают
    // отрицательную ширину, а отрицательный прямоугольник рисуется вывернутым
    // наизнанку и ловит мышь там, где его не видно.
    if (stretchX) size.x = std::max(0.0f, parent.w - t.Margin.x - t.Margin.z);
    if (stretchY) size.y = std::max(0.0f, parent.h - t.Margin.y - t.Margin.w);
    return size;
}

UIRect Resolve(const Element& t, const UIRect& parent, glm::vec2 size) {
    const bool stretchX =
        t.Mode == Element::Stretch::Horizontal || t.Mode == Element::Stretch::Both;
    const bool stretchY =
        t.Mode == Element::Stretch::Vertical || t.Mode == Element::Stretch::Both;

    glm::vec2 pos = ResolveAnchored(t.Anchor, t.Position, size, parent);
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

UIRect Resolve(const Element& t, const UIRect& parent) {
    // LayoutSize — то, что посчитали раскладка или авто-ширина; пока его нет,
    // работает заданный размер.
    const glm::vec2 measured = (t.Resolved.x > 0.0f && t.Resolved.y > 0.0f)
                                   ? t.Resolved
                                   : ResolveSize(t, parent);
    return Resolve(t, parent, measured);
}

float CanvasScale(const Canvas& canvas, glm::vec2 screen) {
    if (canvas.Mode == Canvas::Scale::Pixels) return 1.0f;
    if (canvas.Reference.x <= 0.0f || canvas.Reference.y <= 0.0f) return 1.0f;
    if (screen.x <= 0.0f || screen.y <= 0.0f) return 1.0f;

    if (canvas.Mode == Canvas::Scale::IntegerFit) {
        // Наибольшее целое, при котором опорный экран помещается целиком, но
        // не меньше единицы: окно меньше опорного не должно прятать интерфейс.
        const float fit = std::min(screen.x / canvas.Reference.x, screen.y / canvas.Reference.y);
        float k = std::max(1.0f, std::floor(fit + 1e-4f));
        if (canvas.MaxScale > 0) k = std::min(k, (float)canvas.MaxScale);
        return k;
    }

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

glm::vec2 ApplyLayout(const Stack& stack, const UIRect& container,
                      std::vector<LayoutSlot>& slots) {
    if (slots.empty()) return glm::vec2(0.0f);

    const float left = container.x + stack.Padding.x;
    const float top = container.y + stack.Padding.y;
    const float innerW = std::max(0.0f, container.w - stack.Padding.x - stack.Padding.z);
    const float innerH = std::max(0.0f, container.h - stack.Padding.y - stack.Padding.w);

    if (stack.Direction == Stack::Flow::Grid) {
        const int columns = std::max(1, stack.Columns);
        const int rows = ((int)slots.size() + columns - 1) / columns;
        // Ячейка сетки одна на всех: инвентарь с ячейками разной ширины — это
        // не сетка, а список, и притворяться сеткой ему незачем.
        const float cellW =
            (innerW - stack.Spacing * (float)(columns - 1)) / (float)columns;
        float cellH = 0.0f;
        for (const LayoutSlot& s : slots) cellH = std::max(cellH, s.Size.y);
        for (size_t i = 0; i < slots.size(); ++i) {
            const int col = (int)i % columns;
            const int row = (int)i / columns;
            slots[i].Pos = {left + (float)col * (cellW + stack.Spacing),
                            top + (float)row * (cellH + stack.Spacing)};
            if (stack.StretchCross) slots[i].Size.x = std::max(0.0f, cellW);
        }
        return glm::vec2(innerW, (float)rows * cellH + stack.Spacing * (float)(rows - 1));
    }

    const bool horizontal = stack.Direction == Stack::Flow::Horizontal;
    const float axisSpace = horizontal ? innerW : innerH;

    float content = stack.Spacing * (float)(slots.size() - 1);
    for (const LayoutSlot& s : slots) content += horizontal ? s.Size.x : s.Size.y;

    float cursor = 0.0f;
    float gap = stack.Spacing;
    switch (stack.Justify) {
        case Stack::Align::Start: break;
        case Stack::Align::Center: cursor = (axisSpace - content) * 0.5f; break;
        case Stack::Align::End: cursor = axisSpace - content; break;
        case Stack::Align::SpaceBetween:
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
            if (stack.StretchCross) s.Size.y = innerH;
            s.Pos = {left + cursor, top};
            cursor += s.Size.x + gap;
            extent = std::max(extent, s.Size.y);
        } else {
            if (stack.StretchCross) s.Size.x = innerW;
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
        child.Box.Anchor = UIAnchor::TopLeft;
        child.Box.Mode = Element::Stretch::Both;   // на всю подложку
        child.Box.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        child.HasLabel = true;
        child.LabelStyle.Text = text;
        return child;
    };

    auto add = [&out](const char* name) -> Preset& {
        out.push_back(Preset{});
        out.back().Name = name;
        out.back().Box.Anchor = UIAnchor::Center;
        out.back().Box.Position = {0.0f, 0.0f};
        return out.back();
    };

    {
        // ПУСТОЙ — тоже заготовка, и самая нужная. Элемент без единого
        // компонента ничего не рисует, но им собирают всё остальное: якорь под
        // группу, контейнер под раскладку, узел, который скрипт наполнит сам.
        // Без него «создать просто элемент» приходилось делать, создав панель и
        // сняв с неё заливку.
        Preset& p = add(SAGE_UI_TEXT("Empty"));
        p.Category = SAGE_UI_TEXT("Basic");
        p.Icon = "ui-empty";
        p.Hint = SAGE_UI_TEXT("An empty node: an anchor for a group, a container, something a script will fill");
        p.Box.Size = {120.0f, 60.0f};
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Panel"));
        p.Category = SAGE_UI_TEXT("Basic");
        p.Icon = "ui-panel";
        p.Hint = SAGE_UI_TEXT("A background block: it groups other elements");
        p.Box.Size = {260.0f, 140.0f};
        p.HasFill = true;
    }
    {
        // Кнопка = подложка, реагирующая на мышь, ПЛЮС отдельный объект-надпись
        // внутри. Не «подложка со встроенным текстом»: надпись видно в дереве,
        // её можно подвинуть, покрасить, заменить значком или убрать.
        Preset& p = add(SAGE_UI_TEXT("Button"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-button";
        p.Hint = SAGE_UI_TEXT("A backing that reacts to the mouse, with a caption inside it");
        p.Box.Size = {200.0f, 52.0f};
        p.HasFill = true;
        p.FillStyle.Color = {0.16f, 0.22f, 0.34f, 0.95f};
        p.FillStyle.BorderThickness = 1.0f;
        p.HasInteractable = true; // без этого «кнопка» — просто прямоугольник
        p.Children.push_back(TextChild("Текст", "Button"));
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Text"));
        p.Category = SAGE_UI_TEXT("Basic");
        p.Icon = "ui-text";
        p.Hint = SAGE_UI_TEXT("A line or a paragraph of text");
        p.Box.Size = {220.0f, 40.0f};
        p.HasLabel = true;
        p.LabelStyle.Text = "Text";
        p.LabelStyle.AutoWidth = true;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Image"));
        p.Category = SAGE_UI_TEXT("Basic");
        p.Icon = "ui-image";
        p.Hint = SAGE_UI_TEXT("A picture from the project");
        p.Box.Size = {160.0f, 160.0f};
        p.HasImage = true;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Bar"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-bar";
        p.Hint = SAGE_UI_TEXT("A fill level: health, loading, progress");
        p.Box.Size = {240.0f, 26.0f};
        p.HasFill = true;
        p.FillStyle.Rounding = 6.0f;
        p.HasBar = true;
        p.BarStyle.Value = 0.6f; // пустая полоса неотличима от панели
    }
    {
        // Галка = квадратик слева ПЛЮС подпись рядом отдельным объектом.
        // Раньше подпись жила внутри и начиналась «за квадратиком» по правилу,
        // зашитому в отрисовку; теперь её можно поставить и слева, и под галкой.
        Preset& p = add(SAGE_UI_TEXT("Checkbox"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-check";
        p.Hint = SAGE_UI_TEXT("An on/off switch with a caption next to it");
        p.Box.Size = {200.0f, 36.0f};
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
        text.Box.Anchor = UIAnchor::CenterLeft;
        text.Box.Position = {44.0f, 0.0f};      // правее квадратика
        text.Box.Size = {150.0f, 28.0f};
        text.LabelStyle.Horizontal = Label::Align::Start;
        p.Children.push_back(text);
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Slider"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-slider";
        p.Hint = SAGE_UI_TEXT("A value picked by dragging");
        p.Box.Size = {240.0f, 30.0f};
        p.HasInteractable = true;
        p.HasRange = true;
        // Дорожка и ручка — цвета САМОГО ползунка. Подложка здесь закрасила бы
        // прямоугольник во всю высоту, а дорожка тонкая и по центру.
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Input Field"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-input";
        p.Hint = SAGE_UI_TEXT("A text field edited from the keyboard");
        p.Box.Size = {260.0f, 40.0f};
        p.HasFill = true;
        p.HasLabel = true;
        p.LabelStyle.Horizontal = Label::Align::Start; // по центру набирать непривычно
        p.HasInteractable = true;
        p.HasInput = true;
        p.Input.Placeholder = "Enter text";
    }
    {
        // Новые заготовки — то, что раньше собиралось руками из пяти сущностей.
        Preset& p = add(SAGE_UI_TEXT("Vertical List"));
        p.Category = SAGE_UI_TEXT("Containers");
        p.Icon = "ui-list";
        p.Hint = SAGE_UI_TEXT("Children stand in a column and do not leave the edges");
        p.Box.Size = {280.0f, 320.0f};
        p.HasFill = true;
        p.HasMask = true; // содержимое не вылезает за края списка
        p.HasStack = true;
        p.StackRule.Direction = Stack::Flow::Vertical;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Toolbar"));
        p.Category = SAGE_UI_TEXT("Containers");
        p.Icon = "ui-toolbar";
        p.Hint = SAGE_UI_TEXT("Children stand in a row across the top");
        p.Box.Anchor = UIAnchor::TopCenter;
        p.Box.Size = {600.0f, 56.0f};
        p.Box.Mode = Element::Stretch::Horizontal;
        p.Box.Margin = {24.0f, 16.0f, 24.0f, 0.0f};
        p.HasFill = true;
        p.HasStack = true;
        p.StackRule.Direction = Stack::Flow::Horizontal;
        p.StackRule.Justify = Stack::Align::Center;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Grid"));
        p.Category = SAGE_UI_TEXT("Containers");
        p.Icon = "ui-grid";
        p.Hint = SAGE_UI_TEXT("Children stand in columns and wrap");
        p.Box.Size = {320.0f, 320.0f};
        p.HasFill = true;
        p.HasMask = true;
        p.HasStack = true;
        p.StackRule.Direction = Stack::Flow::Grid;
        p.StackRule.Columns = 4;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Screen"));
        p.Category = SAGE_UI_TEXT("Screens");
        p.Icon = "ui-screen";
        p.Hint = SAGE_UI_TEXT("A full-screen backing: a menu, a pause, a dimmer");
        p.Box.Anchor = UIAnchor::TopLeft;
        p.Box.Mode = Element::Stretch::Both;
        p.Box.Margin = {0.0f, 0.0f, 0.0f, 0.0f};
        p.HasFill = true;
        p.FillStyle.Color = {0.0f, 0.0f, 0.0f, 0.55f};
        p.FillStyle.Rounding = 0.0f;
    }
    return out;
}

} // namespace

const std::vector<Preset>& Presets() {
    static const std::vector<Preset> presets = BuildPresets();
    return presets;
}

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
    // Прежние имена. Заготовки зовут по имени из меню, из скрипта и из
    // самопроверки, и переименование «Label -> Text» не имеет права молча
    // превратить рабочий вызов в «заготовка неизвестна».
    static const std::pair<const char*, const char*> kRenamed[] = {
        {"Label", "Text"},
        {"Input", "Input Field"},
    };
    for (const auto& [was, now] : kRenamed) {
        if (name != was) continue;
        for (const Preset& p : Presets())
            if (p.Name == now) return &p;
    }
    return nullptr;
}

} // namespace sage::ui
