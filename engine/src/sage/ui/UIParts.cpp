// ---------------------------------------------------------------------------
// ВСТРОЕННЫЕ ЧАСТИ ЭЛЕМЕНТА — по одной регистрации на каждую.
//
// Здесь и только здесь записано, ЧТО такое подложка, картинка, текст, значок,
// шкала, поле ввода. Движок про них не знает: он перебирает реестр (UIPart.h).
// Своя часть добавляется таким же файлом рядом — с таблицей полей, функцией
// отрисовки и вызовом RegisterPart, — и сама появляется в отрисовке, в записи
// сцены и в редакторе.
//
// ПРАВИЛО, КОТОРОЕ ДЕРЖИТ ВСЮ СИСТЕМУ: часть рисует себя в прямоугольнике
// элемента и НЕ ДВИГАЕТ соседей. Раньше было наоборот — значок сдвигал текст,
// галка сдвигала текст, картинка отменяла подложку, — и каждая новая часть
// обязана была вписаться в этот клубок. Отсюда же следует, как собирать
// «значок слева, подпись справа»: это не одна хитрая часть, а контейнер с
// раскладкой и два ОБЪЕКТА внутри.
// ---------------------------------------------------------------------------
#include "sage/ui/ImageFit.h"
#include "sage/ui/UIPart.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <list>
#include <string_view>
#include <string>
#include <vector>

#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"

#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

#include "sage/ui/UIPartsDetail.h"

namespace sage::ui {

using namespace parts;

namespace {

// --- Регистрация ------------------------------------------------------------

void RegisterBuiltins() {
    // Порядок: тень 5, подложка 10, картинка 20, шкала 30, диапазон 40,
    // значок 50, текст 60, поле ввода 70. Шаг в десять — чтобы чужая часть
    // могла встать между своими, не переписывая чужие числа.
    PartType fill = MakePart<Fill>("fill", SAGE_UI_TEXT("Fill"), 10, &FillFields(), DrawFill, DrawFillBorder);
    fill.Icon = "rect";
    fill.Hint = SAGE_UI_TEXT("Background, rounding, border, gradient, shadow");
    RegisterPart(fill);

    // Тень — отдельной записью того же компонента: она обязана лечь ПОД всё, а
    // подложка рисуется уже над ней. Так порядок задан числом, а не порядком
    // строк внутри одной функции.
    PartType shadow = MakePart<Fill>("fill.shadow", SAGE_UI_TEXT("Fill shadow"), 5, nullptr, DrawFillShadow);
    RegisterPart(shadow);

    PartType image = MakePart<Image>("image", SAGE_UI_TEXT("Image"), 20, &ImageFields(), DrawImagePart);
    image.Icon = "texture";
    image.Hint = SAGE_UI_TEXT("A file, a piece of a sprite sheet, 9-slice, tiling");
    RegisterPart(image);

    PartType bar = MakePart<Bar>("bar", SAGE_UI_TEXT("Bar"), 30, &BarFields(), DrawBarPart);
    bar.Icon = "wire";
    bar.Hint = SAGE_UI_TEXT("Health, progress, loading");
    RegisterPart(bar);

    PartType range = MakePart<Range>("range", SAGE_UI_TEXT("Range"), 40, &RangeFields(), DrawRangePart);
    range.Icon = "align";
    range.Hint = SAGE_UI_TEXT("A slider or a checkbox");
    RegisterPart(range);

    PartType icon = MakePart<Icon>("icon", SAGE_UI_TEXT("Icon"), 50, &IconFields(), DrawIconPart);
    icon.Icon = "sun";
    icon.Hint = SAGE_UI_TEXT("A vector icon of the engine");
    RegisterPart(icon);

    PartType label = MakePart<Label>("label", SAGE_UI_TEXT("Text"), 60, &LabelFields(), DrawLabelPart);
    label.Icon = "file";
    label.Hint = SAGE_UI_TEXT("A caption: size, colour, alignment, wrapping");
    RegisterPart(label);

    PartType input = MakePart<TextInput>("textInput", SAGE_UI_TEXT("Text Input"), 70, &TextInputFields(),
                                         DrawTextInputPart);
    input.Icon = "code";
    input.Hint = SAGE_UI_TEXT("Edited from the keyboard; the value lives in the Text part");
    RegisterPart(input);

    // Реакция на мышь — УСТРОЙСТВО ТИПА (кнопка, галка, ползунок, поле
    // ввода), а не добавка: её не включают галкой на картинке. Рисовать ей
    // нечего — виды состояний рисует подложка.
    PartType act = MakePart<Interactable>("interactable", SAGE_UI_TEXT("Interaction"), 100,
                                          &InteractableFields());
    act.Icon = "cube";
    act.Hint = SAGE_UI_TEXT("Hover, press, click, looks for each state and what the element does");
    RegisterPart(act);

    // --- Части КОНТЕЙНЕРОВ: невидимая раскладка детей ------------------------
    PartType mask = MakePart<Mask>("mask", SAGE_UI_TEXT("Clipping"), 100, &MaskFields());
    mask.Icon = "rect";
    mask.Hint = SAGE_UI_TEXT("Children are clipped by this container");
    mask.Container = true;
    RegisterPart(mask);

    PartType layout = MakePart<Stack>("layout", SAGE_UI_TEXT("Layout"), 100, &LayoutFields());
    layout.Icon = "layout";
    layout.Hint = SAGE_UI_TEXT("Children stand in a row, a column or a grid");
    layout.Container = true;
    RegisterPart(layout);

    PartType scroll = MakePart<Scroll>("scroll", SAGE_UI_TEXT("Scrolling"), 100, &ScrollFields());
    scroll.Icon = "list";
    scroll.Hint = SAGE_UI_TEXT("The content moves inside the container and is clipped by it");
    scroll.Container = true;
    RegisterPart(scroll);

    // Холст корня — из тех времён, когда корнем интерфейса был элемент. Теперь
    // масштаб задаёт сам интерфейс; часть читается ради старых сцен.
    PartType canvas = MakePart<Canvas>("canvas", SAGE_UI_TEXT("Canvas"), 100, &CanvasFields());
    canvas.Icon = "grid";
    canvas.Hint = SAGE_UI_TEXT("A UI root: reference resolution and order between roots");
    canvas.Hidden = true;
    RegisterPart(canvas);

    // Прозрачность и приём ввода всего поддерева — общая настройка ЛЮБОГО
    // элемента (инспектор показывает её в разделе «Элемент»), а не добавка.
    PartType group = MakePart<Group>("group", SAGE_UI_TEXT("Opacity and input"), 100, &GroupFields());
    group.Icon = "copy";
    group.Hint = SAGE_UI_TEXT("Opacity and input for the whole subtree");
    group.Hidden = true;
    RegisterPart(group);
}

std::vector<PartType>& Registry() {
    static std::vector<PartType> parts;
    return parts;
}

bool& Registered() {
    static bool done = false;
    return done;
}

} // namespace

// --- Общие помощники по тексту (объявлены в UIPart.h) -----------------------

namespace {
// Граница следующего символа UTF-8: резать длинное слово можно только по ней,
// иначе символ разваливается пополам и строка перестаёт быть валидной.
int NextCharBoundary(const std::string& s, int i) {
    const int n = (int)s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(s[(size_t)i]) & 0xC0) == 0x80) ++i;
    return i;
}
} // namespace

int Utf8Length(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Разбивает текст на строки по ширине. Перенос по СЛОВАМ; слово, которое не
// влезает целиком, режется по символам — иначе одно длинное слово молча уехало
// бы за край, то есть ровно то, от чего перенос и спасает. Явные \n уважаются.
std::vector<std::string> WrapLines(const std::string& text, float maxWidth, float scale,
                                   UIRenderer& ui, const UITextStyle* styleOrNull) {
    const UITextStyle style = styleOrNull ? *styleOrNull : UITextStyle{};
    std::vector<std::string> lines;
    if (maxWidth <= 0.0f) { lines.push_back(text); return lines; }

    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string paragraph = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                                : nl - start);
        std::string line;
        size_t i = 0;
        while (i < paragraph.size()) {
            // Следующее слово вместе с ведущими пробелами.
            size_t wordEnd = paragraph.find(' ', i);
            if (wordEnd == std::string::npos) wordEnd = paragraph.size();
            const std::string word = paragraph.substr(i, wordEnd - i);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (ui.MeasureText(candidate, scale, style) <= maxWidth || line.empty()) {
                if (ui.MeasureText(candidate, scale, style) > maxWidth && line.empty()) {
                    // Слово шире строки — режем по символам (по границам UTF-8).
                    std::string chunk;
                    size_t c = 0;
                    while (c < word.size()) {
                        const size_t next = (size_t)NextCharBoundary(word, (int)c);
                        const std::string grown = chunk + word.substr(c, next - c);
                        if (!chunk.empty() && ui.MeasureText(grown, scale, style) > maxWidth) {
                            lines.push_back(chunk);
                            chunk.clear();
                            continue;
                        }
                        chunk = grown;
                        c = next;
                    }
                    line = chunk;
                } else {
                    line = candidate;
                }
            } else {
                lines.push_back(line);
                line = word;
            }
            i = wordEnd + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

void RegisterPart(const PartType& type) {
    std::vector<PartType>& parts = Registry();
    for (PartType& p : parts) {
        // Тот же ключ — замена: так игра подменяет встроенную часть своей, а не
        // получает две записи, из которых рисуются обе.
        if (p.Id && type.Id && std::string(p.Id) == type.Id) {
            p = type;
            std::stable_sort(parts.begin(), parts.end(),
                             [](const PartType& a, const PartType& b) { return a.Order < b.Order; });
            return;
        }
    }
    parts.push_back(type);
    std::stable_sort(parts.begin(), parts.end(),
                     [](const PartType& a, const PartType& b) { return a.Order < b.Order; });
}

const std::vector<PartType>& Parts() {
    // Регистрация встроенных — ЛЕНИВАЯ, а не в статических конструкторах:
    // порядок инициализации статиков между единицами трансляции не определён, и
    // реестр, собранный до своего вектора, — это падение на старте.
    if (!Registered()) {
        Registered() = true;
        RegisterBuiltins();
    }
    return Registry();
}

void CopyField(const PartField& f, const void* src, void* dst) {
    if (!src || !dst) return;
    const char* s = static_cast<const char*>(src) + f.Offset;
    char* d = static_cast<char*>(dst) + f.Offset;
    switch (f.Type) {
        case PartField::Kind::Bool:   *reinterpret_cast<bool*>(d) = *reinterpret_cast<const bool*>(s); break;
        // Enum хранится int'ом — тем же, что и Int: перечисление в компоненте
        // объявлено через int, иначе offsetof по нему не работал бы.
        case PartField::Kind::Int:
        case PartField::Kind::Enum:   *reinterpret_cast<int*>(d) = *reinterpret_cast<const int*>(s); break;
        case PartField::Kind::Float:  *reinterpret_cast<float*>(d) = *reinterpret_cast<const float*>(s); break;
        case PartField::Kind::String: *reinterpret_cast<std::string*>(d) = *reinterpret_cast<const std::string*>(s); break;
        case PartField::Kind::Vec2:   *reinterpret_cast<glm::vec2*>(d) = *reinterpret_cast<const glm::vec2*>(s); break;
        case PartField::Kind::Color:
        case PartField::Kind::Vec4:   *reinterpret_cast<glm::vec4*>(d) = *reinterpret_cast<const glm::vec4*>(s); break;
        case PartField::Kind::Look:
            *reinterpret_cast<Look*>(d) = *reinterpret_cast<const Look*>(s);
            break;
    }
}

const std::vector<PartField>& LookFields() { return LookFieldTable(); }

void BakeEngineSkin(entt::registry& reg, entt::entity e) {
    // Роль подложки — тем же правилом, что при отрисовке.
    PartDrawContext c;
    c.Reg = &reg;
    c.Entity = e;
    if (Fill* fill = reg.try_get<Fill>(e)) {
        const Look l = EngineLook(*fill, FillRole(c));
        static_cast<Look&>(*fill) = l;
    }
    if (Bar* bar = reg.try_get<Bar>(e)) {
        bar->Filled = EngineLook(bar->Filled, EngineRole::BarFill);
        bar->Padding = glm::vec4(3.0f);
        bar->Mode = Bar::FillMode::Stretch;
    }
    if (Range* r = reg.try_get<Range>(e)) {
        r->Track = EngineLook(r->Track, r->Toggle ? EngineRole::Box : EngineRole::Track);
        r->Filled = EngineLook(r->Filled, EngineRole::SliderFill);
        r->Knob = EngineLook(r->Knob, EngineRole::Knob);
        r->Check = EngineLook(r->Check, EngineRole::Check);
        r->TrackThickness = 0.28f;
        r->KnobSize = 1.0f;
    }
    if (Interactable* act = reg.try_get<Interactable>(e)) {
        act->HoverBrightness = 1.15f;
        act->PressedBrightness = 0.85f;
        act->DisabledAlpha = 0.45f;
        act->UseHoverLook = act->UsePressedLook = act->UseFocusedLook = act->UseDisabledLook = false;
    }
    if (Label* label = reg.try_get<Label>(e)) {
        label->ShadowOffset = glm::vec2(0.0f);
        label->OutlineWidth = 0.0f;
        label->StateColors = false;
    }
    if (TextInput* in = reg.try_get<TextInput>(e)) {
        in->CaretColor = glm::vec4(0.0f);
        in->PlaceholderColor = glm::vec4(0.0f);
        in->CaretWidth = 2.0f;
    }
}

const std::vector<PartField>& LookFieldsOf(const PartField& lookField) {
    // Таблица на каждый вид строится один раз: редактор спрашивает её каждый
    // кадр, а ключи в ней — постоянные строки.
    static std::list<std::pair<std::string, std::vector<PartField>>> cache;
    const std::string id = std::string(lookField.Key ? lookField.Key : "") + "@" +
                           std::to_string(lookField.Offset);
    for (const auto& [key, fields] : cache)
        if (key == id) return fields;
    cache.emplace_back(id, ShiftLookFields(Intern(lookField.Key ? lookField.Key : ""),
                                           lookField.Offset, &lookField));
    return cache.back().second;
}

std::vector<PartField> EditableFields(const std::vector<PartField>& fields) {
    std::vector<PartField> out;
    for (const PartField& f : fields) {
        out.push_back(f);
        if (f.Type != PartField::Kind::Look) continue;
        const std::vector<PartField>& inner = LookFieldsOf(f);
        out.insert(out.end(), inner.begin(), inner.end());
    }
    return out;
}

const PartType* FindPart(std::string_view id) {
    for (const PartType& p : Parts())
        if (p.Id && id == p.Id) return &p;
    return nullptr;
}

} // namespace sage::ui
