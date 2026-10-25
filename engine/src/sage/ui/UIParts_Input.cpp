#include "sage/ui/UIPartsDetail.h"

// Части ввода и раскладки (см. UIPartsDetail.h).
namespace sage::ui::parts {

// --- Диапазон: галка и ползунок ---------------------------------------------

const std::vector<PartField>& RangeFields() {
    // Поля ползунка и поля галки разведены условием «галка»: у галки нет ни
    // дорожки с ручкой, ни шага, у ползунка — отметки. Показанные всегда, они
    // читались бы как «работает, просто ничего не делает».
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"value", SAGE_UI_TEXT("Value"), PartField::Kind::Float, offsetof(Range, Value), -1000.0f,
             1000.0f},
            {"min", SAGE_UI_TEXT("Minimum"), PartField::Kind::Float, offsetof(Range, Min), -1000.0f, 1000.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"max", SAGE_UI_TEXT("Maximum"), PartField::Kind::Float, offsetof(Range, Max), -1000.0f, 1000.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"step", SAGE_UI_TEXT("Step"), PartField::Kind::Float, offsetof(Range, Step), 0.0f, 100.0f,
             "0 is smooth; above zero snaps to the step (volume in 5% notches).", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            // «Галка» — не настройка, а ТИП: её задаёт заготовка, и в
            // инспекторе её не переключают (галка с дорожкой и ручкой — это
            // уже не галка). Читается и пишется ради старых сцен.
            {"toggle", SAGE_UI_TEXT("Checkbox"), PartField::Kind::Bool, offsetof(Range, Toggle)},
            {"trackThickness", SAGE_UI_TEXT("Track thickness"), PartField::Kind::Float,
             offsetof(Range, TrackThickness), 0.02f, 1.0f, "A share of the element height.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            {"knobSize", SAGE_UI_TEXT("Knob size"), PartField::Kind::Float, offsetof(Range, KnobSize), 0.0f,
             2.0f, "A share of the element height; the knob is square. 0 — no knob.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 0},
            {"track", SAGE_UI_TEXT("Track"), PartField::Kind::Look, offsetof(Range, Track), 0.0f, 0.0f,
             nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"filled", SAGE_UI_TEXT("Filled part"), PartField::Kind::Look, offsetof(Range, Filled), 0.0f,
             0.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"knob", SAGE_UI_TEXT("Knob"), PartField::Kind::Look, offsetof(Range, Knob), 0.0f, 0.0f, nullptr,
             nullptr, 0, PartField::Widget::Auto, "toggle", 0},
            {"box", SAGE_UI_TEXT("Box"), PartField::Kind::Look, offsetof(Range, Track), 0.0f, 0.0f, nullptr,
             nullptr, 0, PartField::Widget::Auto, "toggle", 1},
            {"check", SAGE_UI_TEXT("Check mark"), PartField::Kind::Look, offsetof(Range, Check), 0.0f, 0.0f,
             "Without a picture — the engine's check mark in this colour.", nullptr, 0,
             PartField::Widget::Auto, "toggle", 1},
        };
        v[0].Content = v[1].Content = v[2].Content = v[3].Content = v[4].Content = true;
        v[4].Hidden = true;
        // «Квадратик» галки — это та же дорожка (Track), показанная под своим
        // именем. В файл второй раз не пишется: один вид, одно место.
        v[10].EditorOnly = true;
        MarkAdvanced(v, {"trackThickness", "knobSize"});
        MarkCustomOnly(v, {"trackThickness", "knobSize"});
        return v;
    }();
    return f;
}

float RangeFraction(const Range& range) {
    const float span = range.Max - range.Min;
    if (std::fabs(span) < 1e-6f) return 0.0f;
    return std::clamp((range.Value - range.Min) / span, 0.0f, 1.0f);
}

UIRect ToggleBox(const UIRect& r) {
    const float side = std::min(r.w, r.h);
    return {r.x, r.y + (r.h - side) * 0.5f, side, side};
}

void DrawRangePart(const PartDrawContext& c) {
    const Range& source = *static_cast<const Range*>(c.Data);
    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    // Оформление движка: те же значение и цвета, но форма — из темы.
    Range engine;
    if (c.EngineSkin) {
        engine = source;
        engine.Track = EngineLook(source.Track, source.Toggle ? EngineRole::Box : EngineRole::Track);
        engine.Filled = EngineLook(source.Filled, EngineRole::SliderFill);
        engine.Knob = EngineLook(source.Knob, EngineRole::Knob);
        engine.Check = EngineLook(source.Check, EngineRole::Check);
        engine.TrackThickness = 0.28f;
        engine.KnobSize = 1.0f;
    }
    const Range& range = c.EngineSkin ? engine : source;

    if (range.Toggle) {
        // Галка: квадратик у левого края СВОЕГО элемента. Подпись к нему —
        // отдельный объект рядом, а не сдвинутый текст внутри: так галку можно
        // поставить и справа от подписи, и под ней.
        const UIRect box = ToggleBox(r);
        DrawLook(c, range.Track, box);
        // Включена — если значение ближе к верхнему концу диапазона.
        if (range.Value < (range.Min + range.Max) * 0.5f) return;
        if (range.Check.HasTexture()) {
            DrawLook(c, range.Check, box, /*tint=*/false);
            return;
        }
        const float pad = box.h * 0.18f;
        const glm::vec3 rgb = StateTint(c, glm::vec3(range.Check.Color));
        DrawIcon(ui, "check", box.x + pad, box.y + pad, box.h - pad * 2.0f, rgb,
                 AlphaOf(c, range.Check.Color.a));
        return;
    }

    // Ползунок: дорожка по центру, ручка поверх. Попасть в тонкую полоску
    // мышью трудно, а нажатие ловит весь элемент.
    const float thick = std::max(r.h * std::clamp(range.TrackThickness, 0.0f, 1.0f), 2.0f);
    const UIRect track{r.x, r.y + (r.h - thick) * 0.5f, r.w, thick};
    DrawLook(c, range.Track, track);

    const float t = RangeFraction(range);
    const float knob = r.h * std::max(range.KnobSize, 0.0f);
    // Ручка не выезжает за концы: её центр ходит от половины ручки до
    // «ширина минус половина».
    const float kx = r.x + knob * 0.5f + (r.w - knob) * t;
    if (t > 0.0f) {
        UIRect filled = track;
        filled.w = std::max(0.0f, kx - track.x);
        if (knob <= 0.0f) filled.w = track.w * t;
        DrawLook(c, range.Filled, filled);
    }
    if (knob > 0.0f) {
        DrawLook(c, range.Knob, {kx - knob * 0.5f, r.y + (r.h - knob) * 0.5f, knob, knob});
    }
}

// --- Поле ввода -------------------------------------------------------------

const std::vector<PartField>& TextInputFields() {
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"placeholder", SAGE_UI_TEXT("Placeholder"), PartField::Kind::String,
             offsetof(TextInput, Placeholder)},
            {"maxLength", SAGE_UI_TEXT("Max length"), PartField::Kind::Int, offsetof(TextInput, MaxLength),
             0.0f, 4096.0f, "0 means no limit."},
            {"password", SAGE_UI_TEXT("Password"), PartField::Kind::Bool, offsetof(TextInput, Password), 0.0f,
             1.0f, "Hide the content behind dots."},
            {"readOnly", SAGE_UI_TEXT("Read only"), PartField::Kind::Bool, offsetof(TextInput, ReadOnly)},
            {"placeholderColor", SAGE_UI_TEXT("Placeholder colour"), PartField::Kind::Color,
             offsetof(TextInput, PlaceholderColor), 0.0f, 1.0f,
             "Alpha 0 — the text colour, half as bright."},
            {"caretColor", SAGE_UI_TEXT("Caret colour"), PartField::Kind::Color,
             offsetof(TextInput, CaretColor), 0.0f, 1.0f, "Alpha 0 — the text colour."},
            {"caretWidth", SAGE_UI_TEXT("Caret width"), PartField::Kind::Float,
             offsetof(TextInput, CaretWidth), 0.5f, 8.0f},
        };
        for (int i = 0; i < 4; ++i) v[(size_t)i].Content = true;
        MarkAdvanced(v, {"readOnly", "placeholderColor", "caretColor", "caretWidth"});
        MarkCustomOnly(v, {"placeholderColor", "caretColor", "caretWidth"});
        return v;
    }();
    return f;
}

std::string MaskText(const std::string& text) {
    std::string out;
    const int n = Utf8Length(text);
    out.reserve((size_t)n * 3);
    for (int i = 0; i < n; ++i) out += "•";
    return out;
}

void DrawTextInputPart(const PartDrawContext& c) {
    const TextInput& input = *static_cast<const TextInput*>(c.Data);
    const Label* label = c.Sibling<Label>();
    if (!label) return; // без текста показывать нечего: поле хранит его в нём

    const UIRect& r = c.Rect;
    UIRenderer& ui = *c.Ui;
    const float textScale = label->Scale * c.Scale;
    const float left = r.x + label->PadX * c.Scale;
    const glm::vec3 rgb{label->Color.r, label->Color.g, label->Color.b};
    const UITextStyle style = StyleOf(*label, ui);
    // По реальной высоте строки, а не по номиналу: иначе содержимое поля
    // стоит не по центру, и тем заметнее, чем крупнее кегль.
    const float y = r.y + (r.h - ui.LineHeight(textScale, style)) * 0.5f;
    const bool empty = label->Text.empty();
    if (empty && !input.Placeholder.empty()) {
        // Подсказка бледнее содержимого — иначе пустое поле выглядит
        // заполненным, и человек стирает то, чего не вводил.
        const bool own = input.PlaceholderColor.a > 0.0f && !c.EngineSkin;
        const glm::vec3 prgb = own ? glm::vec3(input.PlaceholderColor) : rgb;
        const float pa = own ? input.PlaceholderColor.a : label->Color.a * 0.45f;
        ui.Text(left, y, textScale, prgb, input.Placeholder, AlphaOf(c, pa), style);
    } else if (!empty) {
        ui.Text(left, y, textScale, rgb, input.Password ? MaskText(label->Text) : label->Text,
                AlphaOf(c, label->Color.a), style);
    }

    // Курсор мигает только в фокусе и только когда поле включено.
    const Interactable* act = c.Sibling<Interactable>();
    if (!c.Focused || !c.Enabled || !act) return;
    const int caret = std::clamp(act->Runtime.Caret, 0, (int)label->Text.size());
    const std::string head = label->Text.substr(0, (size_t)caret);
    const float cx = left + ui.MeasureText(input.Password ? MaskText(head) : head, textScale, style);
    // Полсекунды виден, полсекунды нет; после каждой правки счётчик
    // сбрасывается, чтобы курсор не пропал ровно тогда, когда на него смотрят.
    if (std::fmod(act->Runtime.CaretBlink, 1.0f) < 0.5f) {
        const float ch = ui.LineHeight(textScale, style) * 0.92f;
        const bool own = input.CaretColor.a > 0.0f && !c.EngineSkin;
        const float caretW = c.EngineSkin ? 2.0f : input.CaretWidth;
        ui.Rect(cx, r.y + (r.h - ch) * 0.5f, std::max(caretW * c.Scale, 1.0f), ch,
                own ? glm::vec3(input.CaretColor) : rgb,
                AlphaOf(c, own ? input.CaretColor.a : label->Color.a));
    }
}

// --- Реакция на мышь -----------------------------------------------------------

const std::vector<PartField>& InteractableFields() {
    // Вид состояния показывается под своей галкой: выключена — подложка
    // подкрашивается множителями (их поля и видны), включена — рисуется свой
    // вид целиком (и видны его поля).
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"enabled", SAGE_UI_TEXT("Enabled"), PartField::Kind::Bool, offsetof(Interactable, Enabled)},
            {"action", SAGE_UI_TEXT("Action"), PartField::Kind::String, offsetof(Interactable, Action), 0.0f,
             0.0f,
             "A name for the game: a script asks whether Continue was pressed, not\n"
             "whether entity 37 was."},
            {"cursor", SAGE_UI_TEXT("Cursor"), PartField::Kind::String, offsetof(Interactable, Cursor)},
            {"hoverBrightness", SAGE_UI_TEXT("Brighter on hover"), PartField::Kind::Float,
             offsetof(Interactable, HoverBrightness), 0.5f, 2.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "useHoverLook", 0},
            {"pressedBrightness", SAGE_UI_TEXT("Darker when pressed"), PartField::Kind::Float,
             offsetof(Interactable, PressedBrightness), 0.5f, 2.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "usePressedLook", 0},
            {"disabledAlpha", SAGE_UI_TEXT("Disabled alpha"), PartField::Kind::Float,
             offsetof(Interactable, DisabledAlpha), 0.0f, 1.0f, nullptr, nullptr, 0,
             PartField::Widget::Auto, "useDisabledLook", 0},
            {"pressedOffset", SAGE_UI_TEXT("Content shift when pressed"), PartField::Kind::Vec2,
             offsetof(Interactable, PressedOffset), -16.0f, 16.0f,
             "Moves everything inside the button while it is held — a caption\n"
             "that sinks together with a pressed picture."},
            {"useHoverLook", SAGE_UI_TEXT("Own look on hover"), PartField::Kind::Bool,
             offsetof(Interactable, UseHoverLook)},
            {"hoverLook", SAGE_UI_TEXT("On hover"), PartField::Kind::Look, offsetof(Interactable, HoverLook),
             0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto, "useHoverLook", 1},
            {"usePressedLook", SAGE_UI_TEXT("Own look when pressed"), PartField::Kind::Bool,
             offsetof(Interactable, UsePressedLook)},
            {"pressedLook", SAGE_UI_TEXT("When pressed"), PartField::Kind::Look,
             offsetof(Interactable, PressedLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "usePressedLook", 1},
            {"useFocusedLook", SAGE_UI_TEXT("Own look when focused"), PartField::Kind::Bool,
             offsetof(Interactable, UseFocusedLook)},
            {"focusedLook", SAGE_UI_TEXT("When focused"), PartField::Kind::Look,
             offsetof(Interactable, FocusedLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "useFocusedLook", 1},
            {"useDisabledLook", SAGE_UI_TEXT("Own look when disabled"), PartField::Kind::Bool,
             offsetof(Interactable, UseDisabledLook)},
            {"disabledLook", SAGE_UI_TEXT("When disabled"), PartField::Kind::Look,
             offsetof(Interactable, DisabledLook), 0.0f, 0.0f, nullptr, nullptr, 0, PartField::Widget::Auto,
             "useDisabledLook", 1},
        };
        v[0].Content = v[1].Content = v[2].Content = true;
        // Состояния — ВКЛАДКАМИ, и свой вид состояния рисует подложка: без
        // неё (у ползунка, галки) эти поля не делают ничего и не показываются.
        MarkTab(v, SAGE_UI_TEXT("On hover"), nullptr, {"hoverBrightness"});
        MarkTab(v, SAGE_UI_TEXT("On hover"), "fill", {"useHoverLook", "hoverLook"});
        MarkTab(v, SAGE_UI_TEXT("When pressed"), nullptr, {"pressedBrightness", "pressedOffset"});
        MarkTab(v, SAGE_UI_TEXT("When pressed"), "fill", {"usePressedLook", "pressedLook"});
        MarkTab(v, SAGE_UI_TEXT("When focused"), "textInput", {"useFocusedLook", "focusedLook"});
        MarkTab(v, SAGE_UI_TEXT("When disabled"), nullptr, {"disabledAlpha"});
        MarkTab(v, SAGE_UI_TEXT("When disabled"), "fill", {"useDisabledLook", "disabledLook"});
        MarkAdvanced(v, {"cursor"});
        // Виды и подкраска состояний — своё оформление; у движка они свои.
        MarkCustomOnly(v, {"hoverBrightness", "pressedBrightness", "disabledAlpha", "pressedOffset",
                           "useHoverLook", "hoverLook", "usePressedLook", "pressedLook",
                           "useFocusedLook", "focusedLook", "useDisabledLook", "disabledLook"});
        return v;
    }();
    return f;
}

const char* const kMaskShape[] = {SAGE_UI_TEXT("Rectangle"), SAGE_UI_TEXT("Rounded")};

const std::vector<PartField>& MaskFields() {
    // Обрезка прямоугольная: форма и скругление в файле есть (старые сцены),
    // но отрисовка режет только прямоугольником — показывать их значило бы
    // обещать то, чего не происходит.
    static const std::vector<PartField> f = [] {
        std::vector<PartField> v = {
            {"form", SAGE_UI_TEXT("Shape"), PartField::Kind::Enum, offsetof(Mask, Form), 0.0f, 0.0f, nullptr,
             kMaskShape, 2},
            {"rounding", SAGE_UI_TEXT("Rounding"), PartField::Kind::Float, offsetof(Mask, Rounding), -1.0f,
             64.0f},
            {"padding", SAGE_UI_TEXT("Clip inset (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Mask, Padding),
             0.0f, 256.0f, "Shrinks the clipping window inside the container."},
            {"showOutside", SAGE_UI_TEXT("Do not clip"), PartField::Kind::Bool, offsetof(Mask, ShowOutside)},
        };
        v[0].Hidden = v[1].Hidden = v[3].Hidden = true;
        return v;
    }();
    return f;
}

const char* const kFlow[] = {SAGE_UI_TEXT("Row"), SAGE_UI_TEXT("Column"), SAGE_UI_TEXT("Grid")};
const char* const kJustify[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("End"),
                                SAGE_UI_TEXT("Even")};
const char* const kCross[] = {SAGE_UI_TEXT("Start"), SAGE_UI_TEXT("Center"), SAGE_UI_TEXT("End"),
                              SAGE_UI_TEXT("Stretch")};

const std::vector<PartField>& LayoutFields() {
    // Поля сетки видны только у сетки, перенос — только у ряда и столбца:
    // «столбцы» у ряда и «перенос» у сетки не значат ничего.
    static const std::vector<PartField> f = {
        {"direction", SAGE_UI_TEXT("Direction"), PartField::Kind::Enum, offsetof(Stack, Direction), 0.0f, 0.0f,
         nullptr, kFlow, 3},
        {"justify", SAGE_UI_TEXT("Along"), PartField::Kind::Enum, offsetof(Stack, Justify), 0.0f, 0.0f,
         "Where the children gather along the main direction when there is\n"
         "room left. Children with Grow take that room instead.",
         kJustify, 4},
        {"cross", SAGE_UI_TEXT("Across"), PartField::Kind::Enum, offsetof(Stack, Cross), 0.0f, 0.0f,
         "Where each child stands across the main direction — or stretch it\n"
         "to the full width (height) of the container.",
         kCross, 4},
        {"spacing", SAGE_UI_TEXT("Spacing"), PartField::Kind::Float, offsetof(Stack, Spacing), 0.0f, 128.0f},
        {"padding", SAGE_UI_TEXT("Padding (l,t,r,b)"), PartField::Kind::Vec4, offsetof(Stack, Padding), 0.0f,
         256.0f},
        {"wrap", SAGE_UI_TEXT("Wrap"), PartField::Kind::Bool, offsetof(Stack, Wrap), 0.0f, 1.0f,
         "Children that do not fit go on to the next line.", nullptr, 0, PartField::Widget::Auto,
         "direction", ShowIfNot((int)Stack::Flow::Grid)},
        {"columns", SAGE_UI_TEXT("Columns"), PartField::Kind::Int, offsetof(Stack, Columns), 0.0f, 32.0f,
         "0 — as many as fit at the cell size.", nullptr, 0, PartField::Widget::Auto, "direction",
         (int)Stack::Flow::Grid},
        {"cellSize", SAGE_UI_TEXT("Cell size"), PartField::Kind::Vec2, offsetof(Stack, CellSize), 0.0f,
         2048.0f, "0 — width from the columns, height from the tallest child.", nullptr, 0,
         PartField::Widget::Auto, "direction", (int)Stack::Flow::Grid},
        {"fitContent", SAGE_UI_TEXT("Size from content"), PartField::Kind::Bool,
         offsetof(Stack, FitContent)},
    };
    static const bool marked = [] {
        MarkAdvanced(const_cast<std::vector<PartField>&>(f), {"cellSize", "fitContent", "padding"});
        return true;
    }();
    (void)marked;
    return f;
}

const std::vector<PartField>& ScrollFields() {
    static const std::vector<PartField> f = {
        {"offset", SAGE_UI_TEXT("Offset"), PartField::Kind::Vec2, offsetof(Scroll, Offset), -100000.0f,
         100000.0f, "How far the content has moved. Also the starting position."},
        {"horizontal", SAGE_UI_TEXT("Scroll sideways"), PartField::Kind::Bool,
         offsetof(Scroll, Horizontal)},
        {"vertical", SAGE_UI_TEXT("Scroll up and down"), PartField::Kind::Bool,
         offsetof(Scroll, Vertical)},
        {"speed", SAGE_UI_TEXT("Wheel step"), PartField::Kind::Float, offsetof(Scroll, Speed), 1.0f,
         400.0f},
        {"clamp", SAGE_UI_TEXT("Stop at the edges"), PartField::Kind::Bool, offsetof(Scroll, Clamp),
         0.0f, 1.0f, "Off where the edge is deliberate: a map, an endless feed."},
    };
    static const bool marked = [] {
        MarkAdvanced(const_cast<std::vector<PartField>&>(f), {"offset", "speed", "clamp"});
        return true;
    }();
    (void)marked;
    return f;
}

const char* const kCanvasScale[] = {SAGE_UI_TEXT("Pixels"), SAGE_UI_TEXT("Scale to reference"),
                                    SAGE_UI_TEXT("Whole-number scale")};

const std::vector<PartField>& CanvasFields() {
    static const std::vector<PartField> f = {
        {"mode", SAGE_UI_TEXT("Scale mode"), PartField::Kind::Enum, offsetof(Canvas, Mode), 0.0f, 0.0f, nullptr,
         kCanvasScale, 3},
        {"reference", SAGE_UI_TEXT("Reference resolution"), PartField::Kind::Vec2, offsetof(Canvas, Reference),
         64.0f, 8192.0f},
        {"matchWidthOrHeight", SAGE_UI_TEXT("Follow"), PartField::Kind::Float,
         offsetof(Canvas, MatchWidthOrHeight), 0.0f, 1.0f,
         "0 follows the width, 1 the height, 0.5 the average."},
        {"maxScale", SAGE_UI_TEXT("Largest scale"), PartField::Kind::Int, offsetof(Canvas, MaxScale), 0.0f,
         16.0f, "Whole-number scale only. 0 — as large as fits."},
        {"sortOrder", SAGE_UI_TEXT("Root order"), PartField::Kind::Int, offsetof(Canvas, SortOrder), -100.0f,
         100.0f, "The HUD under the pause menu, the menu under a dialog."},
    };
    return f;
}

const std::vector<PartField>& GroupFields() {
    static const std::vector<PartField> f = {
        {"alpha", SAGE_UI_TEXT("Alpha"), PartField::Kind::Float, offsetof(Group, Alpha), 0.0f, 1.0f},
        {"interactable", SAGE_UI_TEXT("Catches the mouse"), PartField::Kind::Bool, offsetof(Group, Interactable)},
        {"blockRaycasts", SAGE_UI_TEXT("Block raycasts"), PartField::Kind::Bool,
         offsetof(Group, BlockRaycasts)},
    };
    return f;
}


} // namespace sage::ui::parts
