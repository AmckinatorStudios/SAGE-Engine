#include "EditorTheme.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "EditorPrefs.h"
#include "ui/UIStyle.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace EditorTheme {

namespace {

// --- разбор цвета --------------------------------------------------------
//
// Цвет в теме пишется как "#RRGGBB" или "#RRGGBBAA". Не тройкой чисел 0..1:
// тему правят руками в текстовом редакторе, а шестнадцатеричный код — то, что
// отдаёт любая пипетка и любой макет. Числа 0.137, 0.153, 0.184 не говорят
// человеку ничего.
ImVec4 Hex(const char* s) {
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (!s || *s != '#') return ImVec4(1, 0, 1, 1); // кричаще-розовый: видно сразу
    const size_t len = std::char_traits<char>::length(s);
    if (len == 7) std::sscanf(s + 1, "%2x%2x%2x", &r, &g, &b);
    else if (len == 9) std::sscanf(s + 1, "%2x%2x%2x%2x", &r, &g, &b, &a);
    else return ImVec4(1, 0, 1, 1);
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

std::string ToHex(const ImVec4& c) {
    auto byte = [](float v) {
        return (int)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    char buf[10];
    if (byte(c.w) >= 255) std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(c.x), byte(c.y), byte(c.z));
    else std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(c.x), byte(c.y), byte(c.z), byte(c.w));
    return buf;
}

// Имя роли в файле темы. Один список на чтение и на запись: два разошлись бы.
const char* kRoleNames[(int)Role::_Count] = {
    "bg", "surface", "surfaceAlt", "elevated", "input", "overlay",
    "line", "lineStrong",
    "text", "textDim", "textFaint", "textOnAccent",
    "accent", "accentHover", "accentActive", "accentMuted",
    "hover", "selection",
    "ok", "warn", "danger", "info",
};

// --- встроенные темы -----------------------------------------------------

// Современная тёмная — то, что редактор показывает по умолчанию.
//
// Что делает её «современной», если коротко: слои различаются ЯРКОСТЬЮ ФОНА, а
// не рамками (рамок почти нет); скругления заметные и одинаковые; воздуха
// больше, чем было; акцент ОДИН и работает только там, где что-то выбрано или
// нажато. Предыдущее оформление разделяло всё линиями и красило акцентом любую
// активную кнопку — от этого экран выглядел пёстрым и мелким.
Theme ModernDark() {
    Theme t;
    t.Id = "modern-dark";
    t.Name = "SAGE Dark";
    t.Dark = true;
    ImVec4* c = t.Colors;
    // Слои — лесенкой, шаг примерно в 4 % яркости: меньше не видно, больше
    // рябит. Оттенок чуть холодный (синева в тенях), чтобы жёлтый акцент рядом
    // читался тёплым, а не грязным.
    c[(int)Role::Bg] = Hex("#0E1013");
    c[(int)Role::Surface] = Hex("#15181D");
    c[(int)Role::SurfaceAlt] = Hex("#1B1F26");
    c[(int)Role::Elevated] = Hex("#232830");
    c[(int)Role::Input] = Hex("#1E2229");
    c[(int)Role::Overlay] = Hex("#191D23FA");

    c[(int)Role::Line] = Hex("#272C35");
    c[(int)Role::LineStrong] = Hex("#3A414D");

    c[(int)Role::Text] = Hex("#E8EAEE");
    c[(int)Role::TextDim] = Hex("#9BA3B0");
    c[(int)Role::TextFaint] = Hex("#646C7A");
    // Текст ПОВЕРХ жёлтого — тёмный, а не белый: белое на жёлтом не читается
    // (контраст около 1.7:1 при требуемых 4.5).
    c[(int)Role::TextOnAccent] = Hex("#15181D");

    // SAGE Yellow. Тёплый янтарь, а не лимон: лимонный на тёмном светится и
    // утомляет, а на светлой теме исчезает.
    c[(int)Role::Accent] = Hex("#F0B429");
    c[(int)Role::AccentHover] = Hex("#FFC93C");
    c[(int)Role::AccentActive] = Hex("#D69A18");
    c[(int)Role::AccentMuted] = Hex("#F0B42933");   // подложка выделения

    c[(int)Role::Hover] = Hex("#FFFFFF0F");         // наведение — светлее фона
    c[(int)Role::Selection] = Hex("#F0B4292E");

    c[(int)Role::Ok] = Hex("#4ADE80");
    c[(int)Role::Warn] = Hex("#FBBF24");
    c[(int)Role::Danger] = Hex("#F0616B");
    c[(int)Role::Info] = Hex("#60A5FA");
    return t;
}

// Современная светлая. Не «инверсия тёмной»: на светлом фоне те же насыщенные
// цвета выглядят кислотно и хуже читаются, поэтому акцент и семантика взяты
// темнее, а слои — наоборот, ближе друг к другу.
Theme ModernLight() {
    Theme t = ModernDark();
    t.Id = "modern-light";
    t.Name = "SAGE Light";
    t.Dark = false;
    ImVec4* c = t.Colors;
    // НЕ инверсия тёмной. На светлом фоне лесенка слоёв идёт в другую сторону
    // и шаг мельче: белое рядом с белым различается хуже, чем тёмное рядом с
    // тёмным, поэтому границы здесь держит ещё и линия.
    c[(int)Role::Bg] = Hex("#E4E7EC");
    c[(int)Role::Surface] = Hex("#F7F8FA");
    c[(int)Role::SurfaceAlt] = Hex("#EEF0F4");
    c[(int)Role::Elevated] = Hex("#E4E7EC");
    c[(int)Role::Input] = Hex("#FFFFFF");
    c[(int)Role::Overlay] = Hex("#FFFFFFFA");

    c[(int)Role::Line] = Hex("#D3D8E0");
    c[(int)Role::LineStrong] = Hex("#A8B0BD");

    c[(int)Role::Text] = Hex("#14171C");
    c[(int)Role::TextDim] = Hex("#525A66");
    c[(int)Role::TextFaint] = Hex("#89919E");
    c[(int)Role::TextOnAccent] = Hex("#14171C");

    // Акцент темнее, чем в тёмной теме: тот же #F0B429 на белом даёт контраст
    // 1.8:1 — подпись поверх него не прочесть, да и сама кнопка теряется.
    c[(int)Role::Accent] = Hex("#D99A0B");
    c[(int)Role::AccentHover] = Hex("#EDAA1A");
    c[(int)Role::AccentActive] = Hex("#B87F05");
    c[(int)Role::AccentMuted] = Hex("#D99A0B29");

    c[(int)Role::Hover] = Hex("#0E101314");
    c[(int)Role::Selection] = Hex("#D99A0B33");

    c[(int)Role::Ok] = Hex("#13895A");
    c[(int)Role::Warn] = Hex("#A56A00");
    c[(int)Role::Danger] = Hex("#C43241");
    c[(int)Role::Info] = Hex("#1D6FD0");
    return t;
}

// Почти чёрная: для тёмной комнаты и OLED-экрана, где серый фон светится, а
// чёрный — нет. Акцент тот же фирменный жёлтый: тема меняет ГЛУБИНУ, а не
// принадлежность продукту.
Theme Midnight() {
    Theme t = ModernDark();
    t.Id = "midnight";
    t.Name = "SAGE Midnight";
    ImVec4* c = t.Colors;
    c[(int)Role::Bg] = Hex("#000000");
    c[(int)Role::Surface] = Hex("#08090B");
    c[(int)Role::SurfaceAlt] = Hex("#101216");
    c[(int)Role::Elevated] = Hex("#181B21");
    c[(int)Role::Input] = Hex("#121418");
    c[(int)Role::Overlay] = Hex("#08090BFA");
    c[(int)Role::Line] = Hex("#1C1F25");
    c[(int)Role::LineStrong] = Hex("#333945");
    c[(int)Role::Text] = Hex("#F2F4F7");
    c[(int)Role::TextDim] = Hex("#A6AEBC");
    c[(int)Role::TextFaint] = Hex("#69707E");
    return t;
}

// Прежнее оформление редактора — чтобы новая тема не отняла привычное.
// Метрики тоже прежние: плотнее и с рамками, в этом и была его суть.
Theme Classic() {
    Theme t;
    t.Id = "classic";
    t.Name = "Classic (SAGE 1.x)";
    t.Dark = true;
    ImVec4* c = t.Colors;
    c[(int)Role::Bg] = Hex("#17181B");
    c[(int)Role::Surface] = Hex("#1D1F23");
    c[(int)Role::SurfaceAlt] = Hex("#25272C");
    c[(int)Role::Elevated] = Hex("#2F3238");
    c[(int)Role::Input] = Hex("#25272C");
    c[(int)Role::Overlay] = Hex("#1A1B1FFA");
    c[(int)Role::Line] = Hex("#3D4047");
    c[(int)Role::LineStrong] = Hex("#4B4F57");
    c[(int)Role::Text] = Hex("#E5E8ED");
    c[(int)Role::TextDim] = Hex("#8C919E");
    c[(int)Role::TextFaint] = Hex("#5F646F");
    c[(int)Role::TextOnAccent] = Hex("#FFFFFF");
    c[(int)Role::Accent] = Hex("#4271C7");
    c[(int)Role::AccentHover] = Hex("#5284DE");
    c[(int)Role::AccentActive] = Hex("#37539E");
    c[(int)Role::AccentMuted] = Hex("#4271C744");
    c[(int)Role::Hover] = Hex("#FFFFFF12");
    c[(int)Role::Selection] = Hex("#4271C74D");
    c[(int)Role::Ok] = Hex("#4FBF74");
    c[(int)Role::Warn] = Hex("#D9A441");
    c[(int)Role::Danger] = Hex("#D9534F");
    c[(int)Role::Info] = Hex("#5BA9D8");

    Metrics& m = t.Metric;
    m.Radius = 4.0f;
    m.RadiusFrame = 3.0f;
    m.RadiusSmall = 3.0f;
    m.WindowPadding = ImVec2(8, 8);
    m.FramePadding = ImVec2(8, 4);
    m.ItemSpacing = ImVec2(8, 5);
    m.ItemInnerSpacing = ImVec2(6, 4);
    m.CellPadding = ImVec2(6, 3);
    m.BorderWindow = 1.0f;
    m.TabOverline = 0.0f;
    m.IndentSpacing = 16.0f;
    return t;
}

// --- состояние -----------------------------------------------------------

std::vector<Theme>& Registry() {
    static std::vector<Theme> list;
    return list;
}

std::string& CurrentIdRef() {
    static std::string id = "modern-dark";
    return id;
}

float& ScaleRef() {
    static float scale = 1.0f;
    return scale;
}

const Theme* Find(const std::string& id) {
    for (const Theme& t : Registry())
        if (t.Id == id) return &t;
    return nullptr;
}

// --- темы из файлов ------------------------------------------------------
//
// Дополнительная тема — файл themes/<имя>.json рядом с редактором. Читается
// «мягко»: любое поле можно не писать, недостающее берётся из современной
// тёмной. Так файл темы получается коротким («хочу другой акцент» — три
// строки), а испорченный файл не мешает редактору запуститься.
bool LoadThemeFile(const fs::path& path, Theme& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        LOG_WARN("Editor") << "Тема " << sage::PathToUtf8(path) << " повреждена: " << e.what();
        return false;
    }
    if (!root.is_object()) return false;

    // За основу — та тема, что названа в "base", иначе современная тёмная.
    const std::string base = root.value("base", std::string("modern-dark"));
    const Theme* baseTheme = Find(base);
    out = baseTheme ? *baseTheme : ModernDark();

    out.Id = root.value("id", sage::PathToUtf8(path.stem()));
    out.Name = root.value("name", out.Id);
    out.Dark = root.value("dark", out.Dark);

    if (const auto it = root.find("colors"); it != root.end() && it->is_object()) {
        for (int i = 0; i < (int)Role::_Count; ++i) {
            const auto v = it->find(kRoleNames[i]);
            if (v != it->end() && v->is_string()) out.Colors[i] = Hex(v->get<std::string>().c_str());
        }
    }
    if (const auto it = root.find("metrics"); it != root.end() && it->is_object()) {
        const json& m = *it;
        auto num = [&m](const char* key, float& dst) {
            if (const auto v = m.find(key); v != m.end() && v->is_number()) dst = v->get<float>();
        };
        auto vec = [&m](const char* key, ImVec2& dst) {
            if (const auto v = m.find(key); v != m.end() && v->is_array() && v->size() == 2)
                dst = ImVec2((*v)[0].get<float>(), (*v)[1].get<float>());
        };
        num("fontSize", out.Metric.FontSize);
        num("radius", out.Metric.Radius);
        num("radiusFrame", out.Metric.RadiusFrame);
        num("radiusSmall", out.Metric.RadiusSmall);
        vec("windowPadding", out.Metric.WindowPadding);
        vec("framePadding", out.Metric.FramePadding);
        vec("itemSpacing", out.Metric.ItemSpacing);
        vec("itemInnerSpacing", out.Metric.ItemInnerSpacing);
        vec("cellPadding", out.Metric.CellPadding);
        num("scrollbarSize", out.Metric.ScrollbarSize);
        num("grabMinSize", out.Metric.GrabMinSize);
        num("indentSpacing", out.Metric.IndentSpacing);
        num("borderWindow", out.Metric.BorderWindow);
        num("borderFrame", out.Metric.BorderFrame);
        num("borderPopup", out.Metric.BorderPopup);
        num("tabOverline", out.Metric.TabOverline);
        num("separatorSize", out.Metric.SeparatorSize);
    }
    return true;
}

void LoadExternalThemes() {
    const fs::path dir = ThemesDir();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        Theme t;
        if (!LoadThemeFile(e.path(), t)) continue;
        // Файл с тем же идентификатором заменяет встроенную тему: так её можно
        // подправить, не трогая исходники и не заводя вторую строку в меню.
        auto it = std::find_if(Registry().begin(), Registry().end(),
                               [&t](const Theme& x) { return x.Id == t.Id; });
        if (it != Registry().end()) *it = t;
        else Registry().push_back(t);
        LOG_INFO("Editor") << "Тема оформления: " << t.Name << " (" << sage::PathToUtf8(e.path()) << ")";
    }
}

} // namespace

std::string ThemesDir() {
    const fs::path exe = sage::ExecutableDir();
    if (exe.empty()) return "themes";
    return sage::PathToUtf8(exe / "themes");
}

const std::vector<Theme>& Themes() { return Registry(); }

const Theme& Current() {
    if (const Theme* t = Find(CurrentIdRef())) return *t;
    // Registry пуст только до Init — статическая копия спасает вызов из
    // конструктора панели, случись он раньше времени.
    static const Theme fallback = ModernDark();
    return fallback;
}

const std::string& CurrentId() { return CurrentIdRef(); }

float UiScale() { return ScaleRef(); }

void SetUiScale(float scale) {
    ScaleRef() = std::clamp(scale, 0.75f, 2.0f);
    sage::editor::prefs::SetFloat("uiScale", ScaleRef());
    Apply();
}

ImVec4 Color(Role role) { return Current().Colors[(int)role]; }
ImU32 Color32(Role role) { return ImGui::ColorConvertFloat4ToU32(Color(role)); }

ImVec4 ColorAlpha(Role role, float alpha) {
    ImVec4 c = Color(role);
    c.w *= alpha;
    return c;
}

ImU32 Color32Alpha(Role role, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ColorAlpha(role, alpha));
}

bool SetTheme(const std::string& id) {
    if (!Find(id)) return false;
    CurrentIdRef() = id;
    sage::editor::prefs::SetString("theme", id);
    Apply();
    return true;
}

void Init() {
    if (!Registry().empty()) return;
    Registry().push_back(ModernDark());
    Registry().push_back(ModernLight());
    Registry().push_back(Midnight());
    Registry().push_back(Classic());
    LoadExternalThemes();

    ScaleRef() = std::clamp(sage::editor::prefs::GetFloat("uiScale", 1.0f), 0.75f, 2.0f);
    // SAGE_EDITOR_UI_SCALE — масштаб интерфейса для прогона без человека.
    //
    // Масштаб — то, что ломает раскладку чаще всего: при 150% подписи растут, а
    // ширины, посчитанные в точках, нет, и панель, аккуратная на своей машине,
    // на чужой едет. Проверить это можно было только руками на экране с другим
    // DPI, то есть практически никогда. Настройка НЕ сохраняется: это ключ на
    // один запуск, а не выбор человека.
    if (const std::string s = sage::EnvString("SAGE_EDITOR_UI_SCALE"); !s.empty()) {
        try {
            ScaleRef() = std::clamp(std::stof(s), 0.75f, 2.0f);
        } catch (const std::exception&) {
            LOG_WARN("Editor") << "SAGE_EDITOR_UI_SCALE: не число — '" << s << "'";
        }
    }
    const std::string saved = sage::editor::prefs::GetString("theme", "modern-dark");
    // Сохранённой темы может не быть: файл темы удалили, а выбор остался.
    // Тогда молча берём тему по умолчанию — редактор без оформления не бывает.
    CurrentIdRef() = Find(saved) ? saved : "modern-dark";
    Apply();
}

void LoadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "assets/fonts/editor.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* path : candidates) {
        FILE* probe = std::fopen(path, "rb");
        if (!probe) continue;
        std::fclose(probe);
        io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        return;
    }
    // Ничего не нашлось — остаёмся на встроенном ProggyClean (ASCII).
}

void Apply() {
    const Theme& t = Current();
    const Metrics& m = t.Metric;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();

    // Токены пересчитываются ПЕРЕД раскладкой стиля: ниже почти всё берётся из
    // них, а не из чисел на месте (см. ui/UIStyle.h).
    Sage::UI::Rebuild(ScaleRef());
    const Sage::UI::Style& ui = Sage::UI::Get();

    // Полный сброс перед раскладкой темы. Без него смена темы накапливала бы
    // остатки предыдущей: поля, которые новая тема не трогает, оставались бы от
    // старой, и «вернуться на классическую» давало бы не классическую.
    style = ImGuiStyle();

    // --- кегль и масштаб --------------------------------------------------
    // FontSizeBase, а не пересборка атласа: в ImGui 1.92 шрифт растеризуется
    // динамически, поэтому размер меняется в любом кадре и без мыла.
    style.FontSizeBase = m.FontSize;
    style.FontScaleMain = ScaleRef();

    // --- метрики: ТОЛЬКО из токенов --------------------------------------
    //
    // Тема задаёт характер (плотнее/просторнее, с рамками или без), а сетка —
    // порядок. Поэтому радиусы и отступы берутся из темы, но кратны шагу, а
    // высоты элементов — общие: кнопка в тулбаре и поле в инспекторе обязаны
    // быть одной высоты, иначе ряд «плывёт».
    style.WindowRounding = m.Radius;
    style.ChildRounding = m.RadiusFrame;
    style.PopupRounding = m.Radius;
    style.FrameRounding = m.RadiusFrame;
    style.TabRounding = m.RadiusFrame;
    style.GrabRounding = m.RadiusSmall;
    style.ScrollbarRounding = m.RadiusSmall;
    style.ImageRounding = m.RadiusSmall;
    // Пункты меню и строки списков — со скруглением: прямоугольная подсветка
    // внутри скруглённого окна выдаёт «тему поверх старого вида».
    style.MenuItemRounding = m.RadiusSmall;
    style.SelectableRounding = m.RadiusSmall;

    style.WindowPadding = m.WindowPadding;
    style.FramePadding = m.FramePadding;
    style.ItemSpacing = m.ItemSpacing;
    style.ItemInnerSpacing = m.ItemInnerSpacing;
    style.CellPadding = m.CellPadding;
    style.IndentSpacing = m.IndentSpacing;
    style.ScrollbarSize = ui.ScrollbarSize;
    style.GrabMinSize = ui.SpacingMD;

    style.WindowBorderSize = m.BorderWindow;
    style.ChildBorderSize = m.BorderWindow;
    style.FrameBorderSize = m.BorderFrame;
    style.PopupBorderSize = m.BorderPopup;
    style.TabBarBorderSize = ui.BorderWidth;
    style.TabBarOverlineSize = m.TabOverline;
    style.SeparatorSize = ui.BorderWidth;
    style.SeparatorTextBorderSize = ui.BorderWidth;
    style.DockingSeparatorSize = ui.BorderWidth * 2.0f;
    // Крестик закрытия — только у той вкладки, на которую навели. Ряд из
    // одинаковых «×» рядом с каждым заголовком — самая узнаваемая примета
    // старого интерфейса, и заодно источник случайно закрытых панелей.
    style.TabCloseButtonMinWidthUnselected = 0.0f;

    style.WindowMenuButtonPosition = ImGuiDir_None; // без стрелки в заголовке
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(ui.SpacingLG, m.FramePadding.y);
    // Линии дерева: иерархия сцены читается по вложенности, а не по отступу.
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes;
    style.TreeLinesSize = ui.BorderWidth;
    style.TreeLinesRounding = m.RadiusSmall;
    style.DisabledAlpha = 0.45f;

    // --- цвета ------------------------------------------------------------
    const ImVec4 bg = t.Colors[(int)Role::Bg];
    const ImVec4 surface = t.Colors[(int)Role::Surface];
    const ImVec4 surfaceAlt = t.Colors[(int)Role::SurfaceAlt];
    const ImVec4 elevated = t.Colors[(int)Role::Elevated];
    const ImVec4 input = t.Colors[(int)Role::Input];
    const ImVec4 overlay = t.Colors[(int)Role::Overlay];
    const ImVec4 line = t.Colors[(int)Role::Line];
    const ImVec4 lineStrong = t.Colors[(int)Role::LineStrong];
    const ImVec4 text = t.Colors[(int)Role::Text];
    const ImVec4 textDim = t.Colors[(int)Role::TextDim];
    const ImVec4 textFaint = t.Colors[(int)Role::TextFaint];
    const ImVec4 accent = t.Colors[(int)Role::Accent];
    const ImVec4 accentHover = t.Colors[(int)Role::AccentHover];
    const ImVec4 accentActive = t.Colors[(int)Role::AccentActive];
    const ImVec4 hover = t.Colors[(int)Role::Hover];
    const ImVec4 selection = t.Colors[(int)Role::Selection];
    const ImVec4 info = t.Colors[(int)Role::Info];

    auto tint = [](const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); };

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textFaint;
    c[ImGuiCol_WindowBg] = surface;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = overlay;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = input;
    c[ImGuiCol_FrameBgHovered] = elevated;
    c[ImGuiCol_FrameBgActive] = elevated;

    // Заголовок окна = его же поверхность: рамка вокруг каждой панели — самый
    // заметный признак «старого» интерфейса, а док и без неё показывает границы.
    c[ImGuiCol_TitleBg] = surface;
    c[ImGuiCol_TitleBgActive] = surface;
    c[ImGuiCol_TitleBgCollapsed] = surface;
    c[ImGuiCol_MenuBarBg] = bg;

    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = tint(lineStrong, 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = tint(lineStrong, 0.85f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;

    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;

    // Кнопка по умолчанию — НЕЙТРАЛЬНАЯ. Жёлтый остаётся акцентом: если им
    // покрасить каждую кнопку, акцента не станет вовсе. Главное действие
    // помечается явно (Sage::UI::Button со стилем Primary).
    c[ImGuiCol_Button] = input;
    c[ImGuiCol_ButtonHovered] = elevated;
    c[ImGuiCol_ButtonActive] = lineStrong;

    // Выбранная строка — акцент ПОЛУПРОЗРАЧНЫЙ. Сплошная заливка на каждой
    // выделенной строке дерева превращает иерархию в жёлтую простыню.
    c[ImGuiCol_Header] = selection;
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = selection;

    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = tint(accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = accent;

    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);   // невидим, пока не наведён
    c[ImGuiCol_ResizeGripHovered] = tint(accent, 0.55f);
    c[ImGuiCol_ResizeGripActive] = accent;

    // Вкладки: невыбранная сливается с фоном, выбранная — поверхность окна плюс
    // полоса акцента сверху (TabBarOverlineSize). Так вкладка читается как
    // продолжение панели, а не как отдельная кнопка.
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = elevated;
    c[ImGuiCol_TabSelected] = surface;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = surface;
    c[ImGuiCol_TabDimmedSelectedOverline] = tint(lineStrong, 0.8f);

    c[ImGuiCol_DockingPreview] = tint(accent, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = bg;

    c[ImGuiCol_PlotLines] = accentHover;
    c[ImGuiCol_PlotLinesHovered] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accentHover;

    c[ImGuiCol_TableHeaderBg] = surfaceAlt;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = tint(line, 0.5f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = tint(text, 0.025f);

    c[ImGuiCol_TextLink] = info;
    c[ImGuiCol_TextSelectedBg] = tint(accent, 0.35f);
    c[ImGuiCol_TreeLines] = tint(lineStrong, 0.55f);
    c[ImGuiCol_DragDropTarget] = accentHover;
    c[ImGuiCol_NavCursor] = accentHover;
    c[ImGuiCol_NavWindowingHighlight] = tint(text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = tint(bg, 0.6f);
    c[ImGuiCol_ModalWindowDimBg] = tint(bg, 0.65f);
    c[ImGuiCol_InputTextCursor] = accent;

    (void)textDim;      // роль для панелей; своего слота у ImGui под неё нет
    (void)accentActive; // используется компонентами (Sage::UI), не ImGui

    // Мульти-вьюпорт: платформенные окна рисуются без скруглений и без
    // прозрачности, иначе вытащенная панель выглядит как обрезок, а не как окно
    // операционной системы.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        c[ImGuiCol_WindowBg].w = 1.0f;
    }
}

bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags) {
    ImGui::PushStyleColor(ImGuiCol_Header, Color(Role::SurfaceAlt));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Color(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Color(Role::Elevated));
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

bool ColoredButton(const char* label, Role role, const ImVec2& size) {
    const ImVec4 base = Color(role);
    // Наведение светлее, нажатие темнее — на любом цвете и в любой теме, без
    // второй и третьей записи в палитре под каждую кнопку.
    auto shade = [](const ImVec4& c, float k) {
        return ImVec4(std::clamp(c.x * k, 0.0f, 1.0f), std::clamp(c.y * k, 0.0f, 1.0f),
                      std::clamp(c.z * k, 0.0f, 1.0f), c.w);
    };
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(base, 1.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(base, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, Color(Role::TextOnAccent));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool ExportTheme(const Theme& theme, const std::string& path) {
    json root;
    root["id"] = theme.Id;
    root["name"] = theme.Name;
    root["dark"] = theme.Dark;
    json colors = json::object();
    for (int i = 0; i < (int)Role::_Count; ++i) colors[kRoleNames[i]] = ToHex(theme.Colors[i]);
    root["colors"] = colors;

    const Metrics& m = theme.Metric;
    json metrics = json::object();
    metrics["fontSize"] = m.FontSize;
    metrics["radius"] = m.Radius;
    metrics["radiusFrame"] = m.RadiusFrame;
    metrics["radiusSmall"] = m.RadiusSmall;
    metrics["windowPadding"] = json::array({m.WindowPadding.x, m.WindowPadding.y});
    metrics["framePadding"] = json::array({m.FramePadding.x, m.FramePadding.y});
    metrics["itemSpacing"] = json::array({m.ItemSpacing.x, m.ItemSpacing.y});
    metrics["itemInnerSpacing"] = json::array({m.ItemInnerSpacing.x, m.ItemInnerSpacing.y});
    metrics["cellPadding"] = json::array({m.CellPadding.x, m.CellPadding.y});
    metrics["scrollbarSize"] = m.ScrollbarSize;
    metrics["grabMinSize"] = m.GrabMinSize;
    metrics["indentSpacing"] = m.IndentSpacing;
    metrics["borderWindow"] = m.BorderWindow;
    metrics["borderFrame"] = m.BorderFrame;
    metrics["borderPopup"] = m.BorderPopup;
    metrics["tabOverline"] = m.TabOverline;
    metrics["separatorSize"] = m.SeparatorSize;
    root["metrics"] = metrics;

    const fs::path out = sage::PathFromUtf8(path);
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    std::ofstream file(out);
    if (!file.is_open()) {
        LOG_ERROR("Editor") << "Тему не записать: " << path;
        return false;
    }
    file << root.dump(2) << "\n";
    return true;
}

} // namespace EditorTheme
