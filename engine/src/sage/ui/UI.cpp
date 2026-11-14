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

namespace {

// Где ребёнок встаёт поперёк оси внутри строки высотой lineCross. Возвращает
// сдвиг от начала строки; у ребёнка «во всё место» (fill) меняет сам размер.
float PlaceAcross(Arrangement::Align a, bool fill, float lineCross, float& size) {
    if (fill) {
        size = lineCross;
        return 0.0f;
    }
    switch (a) {
        case Arrangement::Align::Center: return (lineCross - size) * 0.5f;
        case Arrangement::Align::End: return lineCross - size;
        default: return 0.0f;
    }
}

} // namespace

glm::vec2 ApplyLayout(const Arrangement& arr, const UIRect& container,
                      std::vector<LayoutSlot>& slots) {
    if (slots.empty() || !arr.Arranges()) return glm::vec2(0.0f);
    // ПОЛЯ РЕБЁНКА. Раскладка считает место под ребёнка вместе с его полями, а
    // сам он встаёт внутри — так поля работают одинаково в ряду, столбце,
    // сетке и при растяжении.
    for (LayoutSlot& s : slots) s.Size += glm::vec2(s.Margin.x + s.Margin.z, s.Margin.y + s.Margin.w);
    struct Unpad {
        std::vector<LayoutSlot>& Slots;
        ~Unpad() {
            for (LayoutSlot& s : Slots) {
                s.Pos += glm::vec2(s.Margin.x, s.Margin.y);
                s.Size = glm::max(glm::vec2(0.0f),
                                  s.Size - glm::vec2(s.Margin.x + s.Margin.z, s.Margin.y + s.Margin.w));
            }
        }
    } unpad{slots};

    const float left = container.x + arr.Padding.x;
    const float top = container.y + arr.Padding.y;
    const float innerW = std::max(0.0f, container.w - arr.Padding.x - arr.Padding.z);
    const float innerH = std::max(0.0f, container.h - arr.Padding.y - arr.Padding.w);
    const float sp = arr.Spacing;
    const bool fillW = arr.ChildWidth == Arrangement::ChildSize::Fill;
    const bool fillH = arr.ChildHeight == Arrangement::ChildSize::Fill;

    if (arr.Mode == Arrangement::Kind::Grid) {
        // Ячейка сетки одна на всех: инвентарь с ячейками разной ширины — это
        // не сетка, а список, и притворяться сеткой ему незачем.
        float widest = 0.0f, tallest = 0.0f;
        for (const LayoutSlot& s : slots) {
            widest = std::max(widest, s.Size.x);
            tallest = std::max(tallest, s.Size.y);
        }
        const int n = (int)slots.size();
        float cellW = arr.CellSize.x;
        int columns = arr.Columns;
        // Задано только число строк — столбцов столько, чтобы все поместились.
        if (columns <= 0 && arr.Rows > 0) columns = (n + arr.Rows - 1) / arr.Rows;
        if (columns <= 0) {
            // Столбцов «сколько влезет»: ячейки заданного размера (или по
            // самому широкому ребёнку) до правого края — инвентарь, который
            // перестраивается под ширину окна.
            const float w = cellW > 0.0f ? cellW : std::max(widest, 1.0f);
            columns = std::max(1, (int)std::floor((innerW + sp) / (w + sp)));
        }
        if (cellW <= 0.0f) cellW = std::max(0.0f, (innerW - sp * (float)(columns - 1)) / (float)columns);
        const int rows = (n + columns - 1) / columns;
        float cellH = arr.CellSize.y;
        if (cellH <= 0.0f)
            cellH = arr.Rows > 0 && fillH
                        ? std::max(0.0f, (innerH - sp * (float)(arr.Rows - 1)) / (float)arr.Rows)
                        : tallest;
        for (int i = 0; i < n; ++i) {
            LayoutSlot& s = slots[(size_t)i];
            const float x0 = left + (float)(i % columns) * (cellW + sp);
            const float y0 = top + (float)(i / columns) * (cellH + sp);
            // Поперёк в сетке — по обеим осям ячейки: «по центру ячейки» —
            // это центр и по ширине, и по высоте.
            const float dx = PlaceAcross(arr.Alignment, fillW, cellW, s.Size.x);
            const float dy = PlaceAcross(arr.Alignment, fillH, cellH, s.Size.y);
            s.Pos = {x0 + dx, y0 + dy};
        }
        const float usedW = (float)columns * cellW + sp * (float)(columns - 1);
        return glm::vec2(std::max(usedW, arr.Columns > 0 ? innerW : usedW),
                         (float)rows * cellH + sp * (float)(rows - 1));
    }

    const bool horizontal = arr.Horizontal();
    const bool wrap = arr.Wraps();
    const float mainSpace = horizontal ? innerW : innerH;
    const float crossSpace = horizontal ? innerH : innerW;
    const bool fillMain = horizontal ? fillW : fillH;
    const bool fillCross = horizontal ? fillH : fillW;
    auto mainOf = [horizontal](glm::vec2& v) -> float& { return horizontal ? v.x : v.y; };
    auto crossOf = [horizontal](glm::vec2& v) -> float& { return horizontal ? v.y : v.x; };
    // «Во всё место» вдоль оси — каждый ребёнок берёт долю свободного места;
    // своё Element::Grow остаётся для неравных долей (1:2).
    if (fillMain)
        for (LayoutSlot& s : slots) s.Grow = std::max(s.Grow, 1.0f);

    // СТРОКИ. Без переноса строка одна; с переносом ребёнок, который не
    // влезает в остаток строки, начинает следующую (но в строке всегда хотя
    // бы один — иначе широкий ребёнок пропал бы вовсе).
    std::vector<std::pair<size_t, size_t>> lines;   // [начало, конец)
    {
        size_t start = 0;
        float used = 0.0f;
        for (size_t i = 0; i < slots.size(); ++i) {
            const float m = mainOf(slots[i].Size);
            const float need = (i == start) ? m : used + sp + m;
            if (wrap && i > start && need > mainSpace + 0.5f) {
                lines.push_back({start, i});
                start = i;
                used = m;
            } else {
                used = need;
            }
        }
        lines.push_back({start, slots.size()});
    }

    float crossCursor = 0.0f;
    float mainExtent = 0.0f;
    for (size_t li = 0; li < lines.size(); ++li) {
        const auto [b, e] = lines[li];
        const size_t n = e - b;
        float content = sp * (float)(n - 1);
        float growSum = 0.0f;
        float lineCross = 0.0f;
        for (size_t i = b; i < e; ++i) {
            content += mainOf(slots[i].Size);
            growSum += std::max(slots[i].Grow, 0.0f);
            lineCross = std::max(lineCross, crossOf(slots[i].Size));
        }
        // Без переноса строка одна и занимает весь контейнер поперёк:
        // «по центру» и «во всё место» считаются от него.
        if (!wrap) lineCross = crossSpace;

        // РОСТ: свободное место раздаётся тем, у кого Grow > 0, пропорционально.
        float free = mainSpace - content;
        if (growSum > 0.0f && free > 0.0f) {
            for (size_t i = b; i < e; ++i)
                mainOf(slots[i].Size) += free * std::max(slots[i].Grow, 0.0f) / growSum;
            content += free;
            free = 0.0f;
        }

        float cursor = 0.0f;
        float gap = sp;
        switch (arr.Distribution) {
            case Arrangement::Spread::Start: break;
            case Arrangement::Spread::Center: cursor = free * 0.5f; break;
            case Arrangement::Spread::End: cursor = free; break;
            case Arrangement::Spread::SpaceBetween:
                // Свободное место — МЕЖДУ детьми, а не по краям: строка
                // «Назад ... Далее» внизу окна.
                if (n > 1) gap = sp + std::max(0.0f, free) / (float)(n - 1);
                break;
            case Arrangement::Spread::SpaceAround: {
                const float each = std::max(0.0f, free) / (float)n;
                cursor = each * 0.5f;
                gap = sp + each;
                break;
            }
            case Arrangement::Spread::SpaceEvenly: {
                const float each = std::max(0.0f, free) / (float)(n + 1);
                cursor = each;
                gap = sp + each;
                break;
            }
        }

        float lineExtent = 0.0f;
        for (size_t i = b; i < e; ++i) {
            LayoutSlot& s = slots[i];
            float cross = crossOf(s.Size);
            const float shift = PlaceAcross(arr.Alignment, fillCross, lineCross, cross);
            crossOf(s.Size) = cross;
            if (horizontal) s.Pos = {left + cursor, top + crossCursor + shift};
            else s.Pos = {left + crossCursor + shift, top + cursor};
            cursor += mainOf(s.Size) + gap;
            lineExtent = std::max(lineExtent, cross);
        }
        mainExtent = std::max(mainExtent, cursor - gap);
        // Без переноса занятое поперёк — самый крупный ребёнок (по нему
        // контейнер «по содержимому» подгоняет себя); с переносом — сумма строк.
        crossCursor += (wrap ? lineCross : lineExtent) + (li + 1 < lines.size() ? sp : 0.0f);
    }
    return horizontal ? glm::vec2(mainExtent, crossCursor) : glm::vec2(crossCursor, mainExtent);
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
        child.Box.Skin = Element::SkinMode::Engine;
        return child;
    };

    auto add = [&out](const char* name) -> Preset& {
        out.push_back(Preset{});
        out.back().Name = name;
        out.back().Box.Anchor = UIAnchor::Center;
        out.back().Box.Position = {0.0f, 0.0f};
        // Новый элемент из меню — в оформлении движка: сразу выглядит
        // собранно, а из настроек у него только то, что правят всегда.
        out.back().Box.Skin = Element::SkinMode::Engine;
        return out.back();
    };

    // ===================== КОНТЕЙНЕРЫ ===================================
    //
    // Детей держат ТОЛЬКО они (и главный контейнер интерфейса). Как дети
    // расставлены — режим Arrangement самого контейнера (ручной, ряд, столбец,
    // сетка, перенос), а не отдельный тип «ряд» или «столбец».
    {
        // CONTAINER — невидимый: группирует и расставляет детей.
        Preset& p = add(SAGE_UI_TEXT("Container"));
        p.Category = SAGE_UI_TEXT("Containers");
        p.Container = true;
        p.Icon = "layout";
        p.Hint = SAGE_UI_TEXT("Invisible: groups and arranges its children");
        p.Box.Size = {320.0f, 240.0f};
        p.HasArrangement = true;
    }
    {
        // PANEL — то же плюс своё оформление: цвет, картинка, девятина.
        Preset& p = add(SAGE_UI_TEXT("Panel"));
        p.Category = SAGE_UI_TEXT("Containers");
        p.Container = true;
        p.Icon = "ui-panel";
        p.Hint = SAGE_UI_TEXT("A visible container: a shape or a picture behind its children");
        p.Box.Size = {320.0f, 240.0f};
        // Оформление по умолчанию — форма: скруглённый прямоугольник цветом.
        // Картинку (Appearance: Image) выбирают в инспекторе.
        p.HasShape = true;
        p.HasArrangement = true;
    }

    // ===================== ЭЛЕМЕНТЫ =====================================
    //
    // Элемент — то, что ВИДНО: у каждого типа своё устройство (части), свои
    // состояния и свои настройки. Детей он не держит — для этого есть
    // Container и Panel.
    {
        Preset& p = add(SAGE_UI_TEXT("Text"));
        p.Category = SAGE_UI_TEXT("Text");
        p.Icon = "ui-text";
        p.Hint = SAGE_UI_TEXT("A line or a paragraph of text");
        p.Box.Size = {220.0f, 40.0f};
        p.HasLabel = true;
        p.LabelStyle.Text = "Text";
        p.LabelStyle.AutoWidth = true;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Image"));
        p.Category = SAGE_UI_TEXT("Graphics");
        p.Icon = "ui-image";
        p.Hint = SAGE_UI_TEXT("A picture from the project");
        p.Box.Size = {160.0f, 160.0f};
        p.HasImage = true;
    }
    {
        // ФОРМА — самостоятельный графический элемент: та же система, что у
        // оформления Panel, только без детей.
        Preset& p = add(SAGE_UI_TEXT("Shape"));
        p.Category = SAGE_UI_TEXT("Graphics");
        p.Icon = "rect";
        p.Hint = SAGE_UI_TEXT("A rectangle, rounded rectangle, circle or ellipse with fill and stroke");
        p.Box.Size = {120.0f, 120.0f};
        p.HasShape = true;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Icon"));
        p.Category = SAGE_UI_TEXT("Graphics");
        p.Icon = "sun";
        p.Hint = SAGE_UI_TEXT("A vector icon of the engine, in any colour");
        p.Box.Size = {40.0f, 40.0f};
        p.HasIcon = true;
        p.IconStyle.Name = "heart";
    }
    {
        // Кнопка = подложка, реагирующая на мышь, ПЛЮС отдельный объект-надпись
        // внутри. Вид на каждое состояние — у подложки: цвет или своя картинка.
        Preset& p = add(SAGE_UI_TEXT("Button"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-button";
        p.Hint = SAGE_UI_TEXT("Reacts to the mouse; its own look for every state, a caption inside");
        p.Box.Size = {200.0f, 52.0f};
        p.HasFill = true;
        p.FillStyle.Color = {0.16f, 0.22f, 0.34f, 0.95f};
        p.FillStyle.BorderThickness = 1.0f;
        p.HasInteractable = true; // без этого «кнопка» — просто прямоугольник
        Preset text = TextChild("Текст", "Button");
        text.LabelStyle.StateColors = true;
        text.LabelStyle.HoverColor = {1.0f, 1.0f, 1.0f, 1.0f};
        text.LabelStyle.PressedColor = {0.85f, 0.88f, 0.95f, 1.0f};
        p.Children.push_back(text);
    }
    {
        // Галка = квадратик слева ПЛЮС подпись рядом отдельным объектом.
        Preset& p = add(SAGE_UI_TEXT("Checkbox"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-check";
        p.Hint = SAGE_UI_TEXT("An on/off switch: a box, a check mark and a caption");
        p.Box.Size = {200.0f, 36.0f};
        p.HasInteractable = true;
        p.HasRange = true;
        p.RangeValue.Toggle = true;
        p.RangeValue.Step = 1.0f;
        p.RangeValue.Value = 0.0f;
        // Подложки (Fill) у галки НЕТ: она закрасила бы весь элемент вместе с
        // местом под подпись. Квадратик — свой вид галки.
        p.RangeValue.Track.BorderThickness = 1.0f;
        p.RangeValue.Track.BorderColor = {0.55f, 0.60f, 0.72f, 0.9f};
        Preset text = TextChild("Текст", "Checkbox");
        text.Box.Anchor = UIAnchor::CenterLeft;
        text.Box.Mode = Element::Stretch::None;
        text.Box.Position = {44.0f, 0.0f};      // правее квадратика
        text.Box.Size = {150.0f, 28.0f};
        text.LabelStyle.Horizontal = Label::Align::Start;
        p.Children.push_back(text);
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Slider"));
        p.Category = SAGE_UI_TEXT("Controls");
        p.Icon = "ui-slider";
        p.Hint = SAGE_UI_TEXT("A value picked by dragging: track, filled part, knob");
        p.Box.Size = {240.0f, 30.0f};
        p.HasInteractable = true;
        p.HasRange = true;
        // Дорожка — «таблетка»: скругление больше половины толщины.
        p.RangeValue.Track.Rounding = 64.0f;
        p.RangeValue.Filled.Rounding = 64.0f;
    }
    {
        Preset& p = add(SAGE_UI_TEXT("Progress Bar"));
        p.Category = SAGE_UI_TEXT("Indicators");
        p.Icon = "ui-bar";
        p.Hint = SAGE_UI_TEXT("A fill level: health, loading, progress");
        p.Box.Size = {240.0f, 26.0f};
        p.HasFill = true;
        p.FillStyle.Rounding = 6.0f;
        p.HasBar = true;
        p.BarStyle.Value = 0.6f; // пустая полоса неотличима от панели
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
    return nullptr;
}

} // namespace sage::ui
