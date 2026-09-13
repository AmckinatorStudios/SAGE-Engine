#include "EditorIcons.h"

#include <string>
#include "EditorIconFont.inl"
#include "EditorTheme.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "imgui.h"
#include "imgui_internal.h"

#include "Localization.h"
#include "sage/core/Log.h"

namespace EditorIcons {

namespace {

// Метка kThemeColor -> цвет темы. Одно место, через которое проходят все
// иконки: и рядом с текстом, и в кнопках, и поверх списков.
glm::vec3 Resolve(const glm::vec3& c) {
    if (c.x >= 0.0f) return c;
    const ImVec4 t = EditorTheme::Color(EditorTheme::Role::Text);
    return glm::vec3(t.x, t.y, t.z);
}

} // namespace


namespace {

ImU32 Col(const glm::vec3& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a));
}

// ---------------------------------------------------------------------------
// Перо: рисует в квадрате (o.x, o.y, s, s), координаты — доли стороны.
//
// ПОЧЕМУ ЗДЕСЬ СТОЛЬКО ОКРУГЛЕНИЙ. Иконка редактора живёт на 14–24 пикселях, и
// на таком размере всё решает попадание в пиксельную сетку. Раньше и толщина
// штриха (0.09 * 16 = 1.44 px), и координаты (0.28 * 16 = 4.48 px) были
// дробными: сглаживание размазывало каждую линию на два ряда пикселей с разной
// яркостью, соседние линии одной иконки получались разной насыщенности, а
// прямоугольники — с «мохнатыми» краями. Именно это читается как «иконки
// кривые»: рисунок правильный, растр — нет.
//
// Поэтому:
//   * толщина штриха — ЦЕЛОЕ число пикселей (минимум один);
//   * координаты штрихов округляются до пикселя и, для нечётной толщины,
//     сдвигаются в его ЦЕНТР: ImGui рисует линию симметрично вокруг координаты,
//     и однопиксельный штрих на границе пикселя размазывается ровно пополам;
//   * заливки прижимаются к границам пикселей — там центр, наоборот, вреден.
// Одна и та же формула годится и для 12, и для 32 пикселей.
// ---------------------------------------------------------------------------
// Перо для ЗАГЛУШКИ — единственного, что редактор ещё рисует сам.
//
// Формы иконок переехали в шрифт (см. ниже), и вместе с ними ушли дуги,
// кривые, сектора и стрелки, которыми они складывались. Осталось ровно то, чем
// рисуется пустой квадрат с диагональю на месте неизвестного имени: держать
// ради него десяток неиспользуемых примитивов значило бы оставить в редакторе
// вторую, никем не вызываемую систему рисования иконок.
struct Pen {
    ImDrawList* Dl;
    ImVec2 Origin;
    float S;      // сторона квадрата в пикселях
    ImU32 Color;

    ImVec2 P(float x, float y) const { return ImVec2(Origin.x + x * S, Origin.y + y * S); }
    float W(float w) const { return std::max(1.0f, w * S); }

    void Line(float x0, float y0, float x1, float y1, float w = 0.085f) const {
        Dl->AddLine(P(x0, y0), P(x1, y1), Color, W(w));
    }
    void Rect(float x0, float y0, float x1, float y1, float w = 0.085f, float r = 0.0f) const {
        Dl->AddRect(P(x0, y0), P(x1, y1), Color, r * S, 0, W(w));
    }
};


// ФОРМЫ ИКОНОК ЗДЕСЬ БОЛЬШЕ НЕТ — и это осознанная замена, а не потеря.
//
// Полтысячи строк описывали каждую иконку примитивами: прямоугольник, дуга,
// кривая, всё кратно 1/16. Рисунок был свой, а значит и выглядел своим: набор
// рос по одной иконке за раз, каждая подгонялась на глаз, и «шестерёнка» с
// «лампочкой» жили в разных весах штриха, потому что рисовались в разные
// месяцы. Такой набор нельзя ни быстро расширить, ни выдержать в одном стиле:
// чтобы добавить иконку, её надо было СРИСОВАТЬ примитивами.
//
// Теперь форма берётся из Tabler Icons — набора, который рисовали дизайнеры и
// в котором тысячи иконок в одном весе штриха (MIT, лицензия лежит в
// external/tabler-icons-LICENSE). В редактор попадают только нужные шесть
// десятков: scripts/gen_icon_font.py вырезает их в подмножество шрифта и
// раскладывает в EditorIconFont.inl. Добавить иконку — строка в таблице
// скрипта и один прогон.
//
// Прежний довод против шрифта («бинарный файл, своя лицензия, магические
// константы в коде») никуда не делся — просто оказался дешевле своего
// рисования: файл встроен массивом и потому не может не найтись, лицензия
// лежит рядом, а константы сгенерированы вместе со шрифтом и подписаны
// именами, а не расставлены руками.

// Заглушка для незнакомого имени: пустой квадрат с диагональю. Заметно, но не
// ломает вёрстку — ровно как у иконок игрового интерфейса.
void Unknown(const Pen& p) {
    p.Rect(0.1875f, 0.1875f, 0.8125f, 0.8125f, 0.075f);
    p.Line(0.1875f, 0.1875f, 0.8125f, 0.8125f, 0.06f);
}

// --- Шрифт иконок -----------------------------------------------------------
//
// Один ImFont на весь редактор, собранный из встроенного подмножества Tabler
// (см. EditorIconFont.inl). НЕ подмешан к текстовому шрифту (merge): иконки
// рисуются явным вызовом в явном месте и в явном размере, а слияние поменяло бы
// метрики обычного текста ради глифов, которые в тексте не встречаются.
ImFont* g_iconFont = nullptr;

// Кодовая точка по имени иконки. Таблица маленькая и постоянная, поэтому
// линейный поиск: хеш-таблица здесь дороже самого поиска.
unsigned int CodeOf(const char* icon) {
    if (!icon) return 0;
    for (int i = 0; i < EditorIconFont::kGlyphCount; ++i)
        if (std::strcmp(EditorIconFont::kGlyphs[i].Name, icon) == 0)
            return EditorIconFont::kGlyphs[i].Code;
    return 0;
}

void DrawAt(ImDrawList* dl, ImVec2 pos, float size, const char* icon, ImU32 color) {
    // Целый размер и целая позиция: половина пикселя сдвигает ВЕСЬ глиф и
    // размывает его края.
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    size = std::floor(size);

    const unsigned int code = CodeOf(icon);
    if (!g_iconFont || code == 0) {
        // Шрифт не собрался или имени нет в таблице — пустой квадрат с
        // диагональю. Заметно, но вёрстку не ломает.
        Pen pen{dl, pos, size, color};
        Unknown(pen);
        return;
    }

    char utf8[8] = {};
    ImTextStrToUtf8(utf8, (int)sizeof(utf8), (const ImWchar*)&code, (const ImWchar*)&code + 1);

    // Глиф Tabler занимает клетку 24x24 с полем, а иконка обязана быть вписана
    // в квадрат стороной size — иначе кнопки с иконками разной плотности
    // выглядели бы разного размера. Поэтому меряем НАСТОЯЩИЙ размер глифа и
    // центрируем его в отведённом квадрате.
    const ImVec2 measured = g_iconFont->CalcTextSizeA(size, FLT_MAX, 0.0f, utf8);
    const ImVec2 at(pos.x + std::floor((size - measured.x) * 0.5f),
                    pos.y + std::floor((size - measured.y) * 0.5f));
    dl->AddText(g_iconFont, size, at, color, utf8);
}

// Все имена — для страницы проверки и для тестов. Порядок сгруппирован так же,
// как формы выше.
const char* const kNames[] = {
    "play", "pause", "stop", "step",
    "select",
    "move", "rotate", "scale", "universal", "rect", "align", "drop",
    "world", "dots", "pilot",
    "grid", "wire",
    "cube", "sphere", "light", "sun", "camera", "script", "particles", "anim", "ik", "probe",
    "network",
    "physics",
    "folder", "folder-full", "file", "scene", "material", "project", "prefab", "texture",
    "shader", "audio",
    "model",
    "up", "refresh", "folder-plus", "search", "clock", "list", "import", "pencil",
    "undo", "redo", "capsule", "cone",
    "code", "question", "layout", "gear",
    "magnet",
    "warn", "error", "info", "debug",
    "trash", "copy", "save", "open", "plus", "eye", "lock", "unlock",
};

} // namespace


// Собирает шрифт иконок в атлас ImGui. Зовётся ОДИН раз, сразу после того как
// в атлас добавлен текстовый шрифт (см. EditorTheme::LoadFont), и до первого
// кадра: атлас после сборки уже не пополняется.
//
// Размер растеризации заметно больше того, в котором иконки обычно рисуются:
// уменьшение глифа остаётся резким, а увеличение размывает. Иконки редактора
// встречаются и в строке списка (около 14 пикселей), и обложкой в слоте (28), и
// масштаб интерфейса поднимает оба.
void LoadFont() {
    ImGuiIO& io = ImGui::GetIO();

    // Диапазоны — по одной точке на иконку. Просить у ImGui весь блок
    // 0xE000..0xF8FF значило бы растеризовать пять тысяч пустых клеток.
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        for (int i = 0; i < EditorIconFont::kGlyphCount; ++i) {
            ranges.push_back((ImWchar)EditorIconFont::kGlyphs[i].Code);
            ranges.push_back((ImWchar)EditorIconFont::kGlyphs[i].Code);
        }
        ranges.push_back(0);
    }

    ImFontConfig cfg;
    // Массив статический и живёт всю программу — атлас не должен его освобождать.
    cfg.FontDataOwnedByAtlas = false;
    cfg.PixelSnapH = true;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "Tabler Icons");
    g_iconFont = io.Fonts->AddFontFromMemoryTTF(
        (void*)EditorIconFont::kTablerSubset, (int)EditorIconFont::kTablerSubsetSize, 40.0f, &cfg,
        ranges.Data);
}

bool FontReady() { return g_iconFont != nullptr; }

bool HasGlyph(const char* icon) { return CodeOf(icon) != 0; }

int Count() { return (int)(sizeof(kNames) / sizeof(kNames[0])); }
const char* NameAt(int index) {
    return (index >= 0 && index < Count()) ? kNames[index] : nullptr;
}

bool Has(const char* icon) {
    // ПРОСТО СРАВНЕНИЕ ИМЁН по тому же списку kNames, из которого работают
    // Count/NameAt.
    //
    // Раньше здесь ради ответа «есть ли такая иконка» СОБИРАЛСЯ временный
    // ImDrawList и по нему прогонялась отрисовка «в никуда». Две беды сразу.
    // Первая — цена: список отрисовки выделяет память, а спрашивают об иконке
    // в цикле по строкам списка, то есть десятки раз за кадр. Вторая — падение:
    // ImDrawList, созданный вручную, в ImGui 1.92 не готов к PushClipRect, и
    // первый же такой вызов роняет редактор. Ни то, ни другое не нужно, чтобы
    // сверить строку со списком строк.
    if (!icon || !*icon) return false;
    for (int i = 0; i < Count(); ++i) {
        const char* name = NameAt(i);
        if (name && std::strcmp(name, icon) == 0) return true;
    }
    return false;
}


// Совпадающие ID — В ЛОГ, а не только под курсор.
//
// ImGui умеет ловить два видимых элемента с одинаковым ID, но замечает это
// ТОЛЬКО когда мышь стоит на одном из них: проверка привязана к наведённому
// элементу. Поэтому дефект живёт в редакторе месяцами и всплывает у человека
// красным окном «Programmer error: 2 visible items with conflicting ID» ровно в
// тот момент, когда он навёл мышь на кнопку — и выглядит это как поломка
// редактора, а не как наша опечатка. А последствие настоящее: у двух кнопок с
// одним ID нажатие достаётся одной, и вторая просто не работает.
//
// Здесь проверка идёт ПО ФАКТУ ПОДАЧИ элемента, без всякого наведения: значит,
// её видит headless-прогон, и конфликт ловится в CI, а не глазами пользователя.
// Кнопки редактора почти все проходят через EditorIcons, а имя подписи сразу
// говорит, какие именно две кнопки столкнулись.
void CheckDuplicateId(const char* what) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;
    const ImGuiID id = window->GetID("##btn_probe");
    static std::unordered_map<ImGuiID, int> seen;
    auto it = seen.find(id);
    if (it != seen.end() && it->second == g.FrameCount) {
        LOG_ERROR("Editor") << "одинаковый ID у двух элементов интерфейса: '" << what
                            << "' в окне '" << window->Name
                            << "' — нажатие достанется только одному из них "
                               "(нужен PushID или ##суффикс)";
    }
    seen[id] = g.FrameCount;
}

float TextGap() {
    // От высоты строки, а не константой в пикселях: масштаб интерфейса меняет
    // шрифт, и зазор обязан меняться вместе с ним.
    return std::floor(ImGui::GetFontSize() * 0.42f);
}

float LabeledWidth(float line, const char* text) {
    const float icon_s = std::floor(line);
    const float textW = (text && *text) ? ImGui::CalcTextSize(text).x : 0.0f;
    return icon_s + ((text && *text) ? TextGap() + textW : 0.0f);
}

float DrawLabeled(ImDrawList* dl, ImVec2 pos, float line, const char* icon, ImU32 iconColor,
                  const char* text, ImU32 textColor) {
    const float icon_s = std::floor(line);
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    DrawAt(dl, pos, icon_s, icon, iconColor);
    if (!text || !*text) return icon_s;
    const float gap = TextGap();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    // Подпись — по СЕРЕДИНЕ той же высоты, что и значок. Выравнивание по
    // верхнему краю даёт ту самую «кривизну»: у значка поле сверху и снизу, у
    // текста — только сверху, и пара выглядит съехавшей вниз.
    dl->AddText(ImVec2(pos.x + icon_s + gap, std::floor(pos.y + (icon_s - ts.y) * 0.5f)),
                textColor, text);
    return icon_s + gap + ts.x;
}

bool Button(const char* icon, const char* label, const char* tooltip, bool active) {
    const float h = ImGui::GetFrameHeight();
    const float icon_s = std::floor(h * 0.68f);
    ImGui::PushID(label);
    CheckDuplicateId(label);
    // Нажатое состояние — акцент ПОДЛОЖКОЙ, а не заливкой.
    //
    // Сплошной жёлтый на каждой включённой кнопке превращает тулбар в жёлтую
    // ленту: акцентом перестаёт быть что бы то ни было, потому что акцентом
    // становится всё. Подложка в 20 % и жёлтый значок читаются как «включено»
    // не хуже, а рядом стоящее главное действие (Играть) снова выделяется.
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::AccentMuted));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
    }

    // Значок и подпись — ОДНИМ блоком (DrawLabeled), а не двумя отдельными
    // рисунками: зазор между ними и вертикальное выравнивание обязаны быть
    // такими же, как во всех остальных местах редактора.
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float content = LabeledWidth(icon_s, label);
    const ImVec2 size(content + pad.x * 2.0f, h);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##btn", size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    DrawLabeled(dl, ImVec2(cursor.x + pad.x, cursor.y + std::floor((h - icon_s) * 0.5f)), icon_s,
                icon, col, label, col);

    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

bool IconOnlyButton(const char* icon, const char* tooltip, bool active, const glm::vec3& tint,
                    const glm::vec3* hoverTint) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(icon);
    CheckDuplicateId(icon);
    // Нажатое состояние — акцент ПОДЛОЖКОЙ, а не заливкой.
    //
    // Сплошной жёлтый на каждой включённой кнопке превращает тулбар в жёлтую
    // ленту: акцентом перестаёт быть что бы то ни было, потому что акцентом
    // становится всё. Подложка в 20 % и жёлтый значок читаются как «включено»
    // не хуже, а рядом стоящее главное действие (Играть) снова выделяется.
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::AccentMuted));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##ibtn", ImVec2(h, h));
    // Наведение опрашивается СРАЗУ после кнопки: значок рисуется поверх неё, и
    // выбрать его цвет надо до того, как он ляжет на залитую подложку.
    const bool hovered = ImGui::IsItemHovered();
    const glm::vec3 use = (hovered && hoverTint) ? *hoverTint : tint;
    const float icon_s = std::floor(h * 0.64f);
    const float off = std::floor((h - icon_s) * 0.5f);
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(cursor.x + off, cursor.y + off), icon_s, icon,
           Col(Resolve(use)));
    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

// Сколько пробелов освободит в подписи место шириной width. Пробел — единица
// сдвига, доступная внутри ОДНОГО элемента ImGui, а пункт меню обязан остаться
// одним элементом: разбей его на «значок + текст», и сломаются и клик по
// строке, и подсветка под курсором, и переход стрелками.
static int SpacesFor(float width) {
    const float spaceW = ImGui::CalcTextSize(" ").x;
    if (spaceW <= 0.0f) return 1;
    return (int)std::ceil(width / spaceW);
}

bool MenuItem(const char* icon, const char* label, const char* shortcut, bool enabled) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetTextLineHeight();
    const float gap = TextGap();
    // Пробелов — РОВНО столько, сколько нужно под значок с зазором, а не пять
    // «на глаз». Пять пробелов — это разная ширина при разном шрифте и масштабе
    // интерфейса: в одном меню подпись прилипала к рисунку, в другом уезжала.
    const int count = SpacesFor(size + gap);
    const std::string padded = std::string((size_t)count, ' ') + label;
    const bool clicked = ImGui::MenuItem(padded.c_str(), shortcut, false, enabled);

    // Значок ПРИЖАТ К ПОДПИСИ с тем же зазором: остаток от округления пробелов
    // уходит слева, где он никому не мешает, а не между рисунком и словом, где
    // он и читается как кривизна.
    const float textStart = count * ImGui::CalcTextSize(" ").x;
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    // Погашенный пункт и значок имеет погашенный: живой значок у мёртвой строки
    // читается как «работает, просто не нажимается».
    const ImU32 color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    DrawAt(ImGui::GetWindowDrawList(),
           ImVec2(at.x + textStart - gap - size, std::floor(r0.y + ((r1.y - r0.y) - size) * 0.5f)),
           size, icon, color);
    return clicked;
}

void Inline(const char* icon, const glm::vec3& color) {
    const float s = std::floor(ImGui::GetTextLineHeight());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(cursor.x, cursor.y), s, icon, Col(Resolve(color)));
    // Место под иконку резервируется Dummy: без него следующий SameLine лёг бы
    // поверх рисунка, потому что ImGui о нарисованном напрямую не знает.
    //
    // И вместе с ЗАЗОРОМ до подписи: раньше каждый вызывающий добавлял его сам
    // (кто 4 пикселя, кто 6, кто ничего), и в одном списке значок прилипал к
    // тексту, а в соседнем стоял с просветом. Поэтому после Inline идёт
    // SameLine(0, 0) — зазор уже учтён.
    ImGui::Dummy(ImVec2(s + TextGap(), s));
}

void Overlay(float x, float y, float size, const char* icon, const glm::vec3& color) {
    // Ни курсора, ни Dummy: рисунок ложится в список отрисовки окна, а «последний
    // элемент» ImGui остаётся тем, что подали до вызова.
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(x, y), size, icon, Col(Resolve(color)));
}

void Overlay(ImDrawList* dl, float x, float y, float size, const char* icon,
             const glm::vec3& color) {
    if (!dl) return;
    DrawAt(dl, ImVec2(x, y), size, icon, Col(Resolve(color)));
}

void DrawSheet(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(1000, 780), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Icon sheet" "###IconSheet"), open)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(T("Each icon at the sizes it is actually used at."));
    ImGui::Separator();

    // Все иконки на одном экране: 32 px показывает ЗАМЫСЕЛ (форму), 16 и 12 —
    // РАСТР, то есть то, каким его увидят в тулбаре и в списках. Обе стороны
    // нужны сразу: форма может быть хороша, а на 12 пикселях превращаться в
    // пятно — и наоборот.
    static const float kSizes[] = {32.0f, 16.0f, 12.0f};
    const int columns = 4;
    if (ImGui::BeginTable("icons", columns,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < Count(); ++i) {
            if (i % columns == 0) ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            float x = c.x + 6.0f;
            for (float size : kSizes) {
                DrawAt(dl, ImVec2(x, c.y + std::floor((36.0f - size) * 0.5f)), size, kNames[i], col);
                x += size + 12.0f;
            }
            ImGui::Dummy(ImVec2(104.0f, 36.0f));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kNames[i]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace EditorIcons
