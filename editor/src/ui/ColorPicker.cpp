#include "ui/ColorPicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "imgui_internal.h"

#include "Localization.h"
#include "ui/UI.h"
#include "EditorIcons.h"

namespace Sage::UI {

// ============================================================================
//  Математика цвета
// ============================================================================
namespace color {
namespace {

float Clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }  // NaN -> 0
int Byte(float v) { return (int)std::lround(Clamp01(v) * 255.0f); }

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<ImVec4> g_recent;
PickerLayout g_layout;

} // namespace

std::string ToHex(const float rgb[3], const float* alpha) {
    char buf[16];
    if (alpha)
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", Byte(rgb[0]), Byte(rgb[1]),
                      Byte(rgb[2]), Byte(*alpha));
    else
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", Byte(rgb[0]), Byte(rgb[1]), Byte(rgb[2]));
    return buf;
}

bool ParseHex(const char* text, float rgb[3], float* alpha) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '#') ++text;
    int digits[8];
    int n = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t') {
            // Пробелы допустимы только хвостом: «#FF 00 00» — не цвет, а опечатка,
            // и молча прочитать её как «#FF» значило бы подставить не то.
            for (const char* q = p; *q; ++q)
                if (*q != ' ' && *q != '\t') return false;
            break;
        }
        const int d = HexDigit(*p);
        if (d < 0 || n >= 8) return false;
        digits[n++] = d;
    }
    int v[4] = {0, 0, 0, 255};
    if (n == 3 || n == 4) {
        for (int i = 0; i < n; ++i) v[i] = digits[i] * 17;           // «F» -> «FF»
    } else if (n == 6 || n == 8) {
        for (int i = 0; i < n / 2; ++i) v[i] = digits[2 * i] * 16 + digits[2 * i + 1];
    } else {
        return false;
    }
    for (int i = 0; i < 3; ++i) rgb[i] = (float)v[i] / 255.0f;
    if (alpha && (n == 4 || n == 8)) *alpha = (float)v[3] / 255.0f;
    return true;
}

bool ParseAny(const char* text, float rgb[3], float* alpha) {
    if (!text) return false;
    if (ParseHex(text, rgb, alpha)) return true;
    // Числа подряд — через запятые, пробелы, точки с запятой, внутри «rgb(…)».
    // Что угодно другое (буквы, кроме имени функции) — не цвет.
    const char* p = text;
    while (*p == ' ' || *p == '\t') ++p;
    bool func = false;
    for (const char* name : {"rgba(", "rgb(", "RGBA(", "RGB("}) {
        if (std::strncmp(p, name, std::strlen(name)) == 0) { p += std::strlen(name); func = true; break; }
    }
    float v[4];
    bool dot[4] = {false, false, false, false};
    int n = 0;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == ';' || (func && *p == ')')) { ++p; continue; }
        char* end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p || n >= 4 || !(f >= 0.0f)) return false;
        for (const char* q = p; q < end; ++q) if (*q == '.') dot[n] = true;
        v[n++] = f;
        p = end;
    }
    if (n != 3 && n != 4) return false;
    // Шкала — одна на три канала: «1, 0.5, 0» — доли, «255, 128, 0» — байты.
    // Решает НАЛИЧИЕ числа больше 1; «1, 1, 1» без точек — байты почти чёрного
    // не бывают на практике, а белый в долях бывает, поэтому единицы — доли.
    bool bytes = false;
    for (int i = 0; i < 3; ++i) if (v[i] > 1.0f) bytes = true;
    float out[3];
    for (int i = 0; i < 3; ++i) {
        out[i] = bytes ? v[i] / 255.0f : v[i];
        if (out[i] > 1.0f) return false;
    }
    std::memcpy(rgb, out, sizeof out);
    // Альфа в CSS — доля даже при байтовых каналах («rgba(255,0,0,0.5)»), а в
    // списке байт — байт.
    if (alpha && n == 4) *alpha = Clamp01(v[3] > 1.0f || (bytes && !dot[3] && !func) ? v[3] / 255.0f : v[3]);
    return true;
}

std::string ToRgbText(const float rgb[3], const float* alpha) {
    char buf[48];
    if (alpha)
        std::snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %.2f)", Byte(rgb[0]), Byte(rgb[1]),
                      Byte(rgb[2]), Clamp01(*alpha));
    else
        std::snprintf(buf, sizeof buf, "rgb(%d, %d, %d)", Byte(rgb[0]), Byte(rgb[1]), Byte(rgb[2]));
    return buf;
}

std::string ToFloatText(const float rgb[3], const float* alpha) {
    char buf[64];
    if (alpha)
        std::snprintf(buf, sizeof buf, "%.3f, %.3f, %.3f, %.3f", Clamp01(rgb[0]), Clamp01(rgb[1]),
                      Clamp01(rgb[2]), Clamp01(*alpha));
    else
        std::snprintf(buf, sizeof buf, "%.3f, %.3f, %.3f", Clamp01(rgb[0]), Clamp01(rgb[1]),
                      Clamp01(rgb[2]));
    return buf;
}

void RgbToHsv(const float rgb[3], float hsv[3]) {
    ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], hsv[0], hsv[1], hsv[2]);
}

void HsvToRgb(const float hsv[3], float rgb[3]) {
    ImGui::ColorConvertHSVtoRGB(hsv[0], hsv[1], hsv[2], rgb[0], rgb[1], rgb[2]);
}

void RgbToHsvKeep(const float rgb[3], const float prevHsv[3], float hsv[3]) {
    RgbToHsv(rgb, hsv);
    const float eps = 1.0f / 4096.0f;
    if (hsv[2] <= eps) {
        hsv[0] = prevHsv[0];
        hsv[1] = prevHsv[1];
    } else if (hsv[1] <= eps) {
        hsv[0] = prevHsv[0];
    }
}

bool WantsDarkText(const float rgb[3]) {
    // Веса яркости Rec.601 по значениям, как они на экране: этого хватает, чтобы
    // решить «чёрным или белым», а точнее здесь и не нужно.
    return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2] > 0.55f;
}

const std::vector<ImVec4>& Recent() { return g_recent; }

void Remember(const ImVec4& c) {
    // Повтор ищется с допуском в полшага байта: цвет, выбранный заново тем же
    // щелчком, не должен занимать второе место в ряду.
    const float tol = 0.5f / 255.0f;
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [&](const ImVec4& r) {
                                      return std::fabs(r.x - c.x) <= tol &&
                                             std::fabs(r.y - c.y) <= tol &&
                                             std::fabs(r.z - c.z) <= tol &&
                                             std::fabs(r.w - c.w) <= tol;
                                  }),
                   g_recent.end());
    g_recent.insert(g_recent.begin(), c);
    if ((int)g_recent.size() > kRecentMax) g_recent.resize(kRecentMax);
}

void ClearRecent() { g_recent.clear(); }

const PickerLayout& LastPickerLayout() { return g_layout; }
MenuLayout g_menuLayout;
const MenuLayout& LastMenuLayout() { return g_menuLayout; }

} // namespace color

// ============================================================================
//  Отрисовка
// ============================================================================
namespace {

using color::Box;

// Открытая палитра поля. Открыта всегда одна (это всплывающее окно), поэтому
// одной записи достаточно.
struct Session {
    ImGuiID Owner = 0;
    float Original[4] = {0, 0, 0, 1};
    bool Edited = false;
};
Session g_session;

// ПРАВКА ОДНИМ ДЕЙСТВИЕМ — вставка из буфера и пипетка. У них нет жеста
// «нажал — потянул — отпустил», а отмена правок (TrackLastImGuiItem) видит
// только жесты: начало (IsItemActivated) снимает «до», конец
// (IsItemDeactivatedAfterEdit) пишет запись. Поэтому правка разыгрывается как
// жест из двух кадров на скрытом элементе поля: в первом кадре он становится
// активным (снимок «до» — ещё старого цвета), во втором цвет меняется и
// элемент отпускается. Одна правка — одна запись в истории, как у ползунка.
struct OneShot {
    ImGuiID Id = 0;
    int Stage = 0;   // 1 — начать жест, 2 — применить и отпустить
    float Rgb[3] = {0, 0, 0};
    float Alpha = 1.0f;
    bool HasAlpha = false;
};
OneShot g_oneShot;

void RequestSet(ImGuiID owner, const float rgb[3], const float* alpha) {
    g_oneShot = OneShot{};
    g_oneShot.Id = owner;
    g_oneShot.Stage = 1;
    std::memcpy(g_oneShot.Rgb, rgb, sizeof(float) * 3);
    if (alpha) {
        g_oneShot.Alpha = *alpha;
        g_oneShot.HasAlpha = true;
    }
}

// Разыграть правку для поля owner. true — цвет поменялся в этом кадре.
bool ApplyOneShot(ImGuiID owner, float rgb[3], float* alpha) {
    if (g_oneShot.Id != owner || owner == 0) return false;
    ImGuiContext& g = *GImGui;
    bool changed = false;
    if (g_oneShot.Stage == 1) {
        ImGui::SetActiveID(owner, ImGui::GetCurrentWindow());
        ImGui::KeepAliveID(owner);
        g_oneShot.Stage = 2;
    } else {
        std::memcpy(rgb, g_oneShot.Rgb, sizeof(float) * 3);
        if (alpha && g_oneShot.HasAlpha) *alpha = g_oneShot.Alpha;
        color::Remember(ImVec4(rgb[0], rgb[1], rgb[2], alpha ? *alpha : 1.0f));
        changed = true;
        // Жест мог перехватить кто-то другой (щелчок между кадрами) — тогда
        // цвет всё равно применяется, просто без собственной записи отмены.
        if (g.ActiveId == owner) {
            ImGui::MarkItemEdited(owner);
            ImGui::ClearActiveID();
        }
        g_oneShot = OneShot{};
    }
    // «Последний элемент» — скрытый элемент жеста: по нему вызывающий видит
    // начало и конец правки.
    g.LastItemData.ID = owner;
    // Флаги «отпущен / не отпущен» остались от настоящего последнего элемента
    // и перекрыли бы проверку конца жеста по DeactivatedItemData.
    g.LastItemData.StatusFlags &=
        ~(ImGuiItemStatusFlags_HasDeactivated | ImGuiItemStatusFlags_Deactivated);
    return changed;
}

// --- Пипетка -----------------------------------------------------------------
struct Dropper {
    bool Active = false;
    ImGuiID Owner = 0;
    bool Released = false;     // кнопку, запустившую пипетку, уже отпустили
    float Preview[3] = {0, 0, 0};
    bool HasPreview = false;
    bool WantFb = false;       // в этом кадре цвет берётся из своего кадра
    int FbX = 0, FbY = 0;
    float Fb[3] = {0, 0, 0};
    bool FbValid = false;
};
Dropper g_drop;
eyedropper::Backend g_backend;

// Меню поля по ПКМ: копировать в трёх записях, вставить, пипетка.
void ColorContextMenu(ImGuiID owner, const float rgb[3], const float* alpha, bool readOnly) {
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("###sage_color_ctx")) {
        color::MenuLayout& lay = color::g_menuLayout;
        lay = {};
        auto rect = [] { return Box{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; };
        if (EditorIcons::MenuItem("copy", T("Copy colour"), color::ToHex(rgb, alpha).c_str()))
            ImGui::SetClipboardText(color::ToHex(rgb, alpha).c_str());
        lay.CopyHex = rect();
        if (EditorIcons::MenuItem("copy", T("Copy as RGB"), color::ToRgbText(rgb, alpha).c_str()))
            ImGui::SetClipboardText(color::ToRgbText(rgb, alpha).c_str());
        if (EditorIcons::MenuItem("copy", T("Copy as numbers 0..1")))
            ImGui::SetClipboardText(color::ToFloatText(rgb, alpha).c_str());
        if (!readOnly) {
            ImGui::Separator();
            // Вставка доступна, только если в буфере правда цвет: пункт,
            // который молча ничего не делает, хуже серого.
            const char* clip = ImGui::GetClipboardText();
            float pasted[3] = {rgb[0], rgb[1], rgb[2]};
            float pastedA = alpha ? *alpha : 1.0f;
            const bool canPaste = clip && color::ParseAny(clip, pasted, alpha ? &pastedA : nullptr);
            if (EditorIcons::MenuItem("paste", T("Paste colour"),
                                      canPaste ? color::ToHex(pasted, alpha ? &pastedA : nullptr).c_str()
                                               : nullptr,
                                      canPaste))
                RequestSet(owner, pasted, alpha ? &pastedA : nullptr);
            lay.Paste = rect();
            if (EditorIcons::MenuItem("eyedropper", T("Pick from screen")))
                eyedropper::Start(owner);
            lay.Pick = rect();
        }
        ImGui::EndPopup();
    }
}

// Тон и насыщенность, которые помнит палитра (см. RgbToHsvKeep): пока цвет на
// входе тот, что палитра записала сама, берём её HSV как есть — пересчёт из
// RGB терял бы тон у серого и дрожал бы от округления на каждом кадре.
struct HsvMemory {
    float Rgb[3] = {-1, -1, -1};
    float Hsv[3] = {0, 0, 1};
};
HsvMemory g_hsv;

int g_channelMode = 0;  // 0 — RGB, 1 — HSV; помнится между открытиями

void CurrentHsv(const float rgb[3], float hsv[3]) {
    if (rgb[0] == g_hsv.Rgb[0] && rgb[1] == g_hsv.Rgb[1] && rgb[2] == g_hsv.Rgb[2]) {
        std::memcpy(hsv, g_hsv.Hsv, sizeof g_hsv.Hsv);
        return;
    }
    color::RgbToHsvKeep(rgb, g_hsv.Hsv, hsv);
}

void StoreHsv(const float rgb[3], const float hsv[3]) {
    std::memcpy(g_hsv.Rgb, rgb, sizeof g_hsv.Rgb);
    std::memcpy(g_hsv.Hsv, hsv, sizeof g_hsv.Hsv);
}

ImU32 ToU32(const float rgb[3], float a = 1.0f) {
    // ColorConvertFloat4ToU32, а не GetColorU32: тот домножает на прозрачность
    // стиля, и в отключённой панели образец врал бы о самом цвете.
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], a));
}

ImU32 HueU32(float h) {
    const float hsv[3] = {h, 1.0f, 1.0f};
    float rgb[3];
    color::HsvToRgb(hsv, rgb);
    return ToU32(rgb);
}

void DrawChecker(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 fill,
                 float rounding = 0.0f, ImDrawFlags flags = 0) {
    const float step = std::max(4.0f, std::floor((b.y - a.y) * 0.5f));
    ImGui::RenderColorRectWithAlphaCheckerboard(dl, a, b, fill, step, ImVec2(0, 0), rounding, flags);
}

void DrawHueStrip(ImDrawList* dl, const Box& box) {
    const float w = box.Max.x - box.Min.x;
    for (int i = 0; i < 6; ++i) {
        const float x0 = std::floor(box.Min.x + w * (float)i / 6.0f);
        const float x1 = std::ceil(box.Min.x + w * (float)(i + 1) / 6.0f);
        const ImU32 l = HueU32((float)i / 6.0f), r = HueU32((float)(i + 1) / 6.0f);
        dl->AddRectFilledMultiColor(ImVec2(x0, box.Min.y), ImVec2(std::min(x1, box.Max.x), box.Max.y),
                                    l, r, r, l);
    }
}

void DrawGradient(ImDrawList* dl, const Box& box, ImU32 l, ImU32 r) {
    dl->AddRectFilledMultiColor(box.Min, box.Max, l, r, r, l);
}

void DrawOutline(ImDrawList* dl, const Box& box) {
    dl->AddRect(box.Min, box.Max, ImGui::GetColorU32(ImGuiCol_Border));
}

// Маркер положения на полосе: рамка, видная и на белом, и на чёрном.
void DrawBarMarker(ImDrawList* dl, const Box& box, float t) {
    const float x = std::round(box.Min.x + (box.Max.x - box.Min.x) * t);
    const ImVec2 a(x - 3.0f, box.Min.y - 2.0f), b(x + 3.0f, box.Max.y + 2.0f);
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 200), 2.0f, 0, 3.0f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), 2.0f, 0, 1.5f);
}

// Полоса-ползунок: невидимая кнопка + положение 0..1 по горизонтали. Нажатие
// сразу ставит значение под курсор — не надо попадать в маркер.
bool BarBehavior(const char* id, const ImVec2& size, float& t, Box& box) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    box = {p0, ImVec2(p0.x + size.x, p0.y + size.y)};
    if (!ImGui::IsItemActive()) return false;
    const float nt = color::Clamp01((ImGui::GetIO().MousePos.x - p0.x) / size.x);
    if (nt == t) return false;
    t = nt;
    ImGui::MarkItemEdited(ImGui::GetItemID());
    return true;
}

// Образец цвета в поле: цвет, справа от него — он же поверх шахматки с
// прозрачностью; поверх — hex и процент. Всё, что нужно знать о цвете, видно
// без щелчка.
void DrawFieldSwatch(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const float rgb[3],
                     const float* alpha, bool compact, bool hovered, bool readOnly) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rounding = style.FrameRounding;
    const ImU32 solid = ToU32(rgb);
    const bool translucent = alpha && *alpha < 1.0f;
    const float split = translucent ? std::round(a.x + (b.x - a.x) * (compact ? 0.5f : 0.62f)) : b.x;
    if (!translucent) {
        dl->AddRectFilled(a, b, solid, rounding);
    } else {
        dl->AddRectFilled(a, ImVec2(split, b.y), solid, rounding, ImDrawFlags_RoundCornersLeft);
        DrawChecker(dl, ImVec2(split, a.y), b, ToU32(rgb, color::Clamp01(*alpha)), rounding,
                    ImDrawFlags_RoundCornersRight);
    }
    const ImU32 border = hovered && !readOnly ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive)
                                              : ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRect(a, b, border, rounding, 0, hovered && !readOnly ? 1.5f : 1.0f);
    if (compact) return;

    const float fs = ImGui::GetFontSize();
    const float y = a.y + std::floor((b.y - a.y - fs) * 0.5f);
    const ImU32 dark = IM_COL32(24, 24, 24, 255), light = IM_COL32(246, 246, 246, 255);
    const std::string hex = color::ToHex(rgb);
    dl->PushClipRect(a, ImVec2(split, b.y), true);
    dl->AddText(ImVec2(a.x + style.FramePadding.x, y), color::WantsDarkText(rgb) ? dark : light,
                hex.c_str());
    dl->PopClipRect();
    if (alpha) {
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", (int)std::lround(color::Clamp01(*alpha) * 100.0f));
        const float tw = ImGui::CalcTextSize(pct).x;
        // Цвет текста — по тому, что под ним видно: цвет, смешанный с серым
        // шахматки в меру прозрачности.
        const float k = color::Clamp01(*alpha);
        const float seen[3] = {rgb[0] * k + 0.7f * (1 - k), rgb[1] * k + 0.7f * (1 - k),
                               rgb[2] * k + 0.7f * (1 - k)};
        const bool darkText = translucent ? color::WantsDarkText(seen) : color::WantsDarkText(rgb);
        dl->AddText(ImVec2(b.x - style.FramePadding.x - tw, y), darkText ? dark : light, pct);
    }
}

// Сама палитра — общая для всплывающего окна поля и для встроенной.
// original — цвет до открытия (RGBA) для «было/стало»; nullptr — без него.
bool PickerBody(float rgb[3], float* alpha, const float* original, float width, ImGuiID owner) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fs = ImGui::GetFontSize();
    const float frameH = ImGui::GetFrameHeight();
    const float W = width > 0.0f ? width : std::round(fs * 17.0f);
    const float inner = style.ItemInnerSpacing.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    color::PickerLayout& lay = color::g_layout;
    lay = {};
    bool changed = false;

    float hsv[3];
    CurrentHsv(rgb, hsv);
    auto applyHsv = [&]() {
        color::HsvToRgb(hsv, rgb);
        StoreHsv(rgb, hsv);
        changed = true;
    };

    // --- квадрат: насыщенность по горизонтали, яркость по вертикали ---------
    {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(W, std::round(W * 0.62f));
        ImGui::InvisibleButton("##sv", size);
        lay.SV = {p0, ImVec2(p0.x + size.x, p0.y + size.y)};
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float s = color::Clamp01((m.x - p0.x) / size.x);
            const float v = 1.0f - color::Clamp01((m.y - p0.y) / size.y);
            if (s != hsv[1] || v != hsv[2]) {
                hsv[1] = s;
                hsv[2] = v;
                applyHsv();
                ImGui::MarkItemEdited(ImGui::GetItemID());
            }
        }
        const ImU32 white = IM_COL32(255, 255, 255, 255), hue = HueU32(hsv[0]);
        const ImU32 clear = IM_COL32(0, 0, 0, 0), black = IM_COL32(0, 0, 0, 255);
        dl->AddRectFilledMultiColor(lay.SV.Min, lay.SV.Max, white, hue, hue, white);
        dl->AddRectFilledMultiColor(lay.SV.Min, lay.SV.Max, clear, clear, black, black);
        DrawOutline(dl, lay.SV);
        const ImVec2 at = lay.SV.At(hsv[1], 1.0f - hsv[2]);
        const float r = std::round(fs * 0.42f);
        dl->AddCircleFilled(at, r, ToU32(rgb), 20);
        dl->AddCircle(at, r + 1.0f, IM_COL32(0, 0, 0, 200), 20, 2.5f);
        dl->AddCircle(at, r, IM_COL32(255, 255, 255, 255), 20, 1.5f);
    }

    // --- тон ----------------------------------------------------------------
    {
        float t = hsv[0];
        if (BarBehavior("##hue", ImVec2(W, std::round(frameH * 0.8f)), t, lay.Hue)) {
            hsv[0] = t;
            applyHsv();
        }
        DrawHueStrip(dl, lay.Hue);
        DrawOutline(dl, lay.Hue);
        DrawBarMarker(dl, lay.Hue, hsv[0]);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Hue"));
    }

    // --- прозрачность: крупно, с числом рядом --------------------------------
    //
    // Ради неё палитру и переделывали: в стандартной она узкой полосой сбоку,
    // без чисел, и попасть в «полупрозрачное» можно было только на глаз.
    if (alpha) {
        const float numW = std::round(fs * 3.6f);
        const float barW = W - numW - inner;
        float t = color::Clamp01(*alpha);
        if (BarBehavior("##alpha", ImVec2(barW, frameH), t, lay.Alpha)) {
            *alpha = t;
            changed = true;
        }
        const bool hot = ImGui::IsItemHovered();
        DrawChecker(dl, lay.Alpha.Min, lay.Alpha.Max, IM_COL32(0, 0, 0, 0));
        DrawGradient(dl, lay.Alpha, ToU32(rgb, 0.0f), ToU32(rgb, 1.0f));
        DrawOutline(dl, lay.Alpha);
        DrawBarMarker(dl, lay.Alpha, color::Clamp01(*alpha));
        if (hot) ImGui::SetTooltip("%s", T("Opacity. Drag the bar or type a percentage."));
        ImGui::SameLine(0.0f, inner);
        float pct = color::Clamp01(*alpha) * 100.0f;
        ImGui::SetNextItemWidth(numW);
        if (ImGui::DragFloat("##alpha_pct", &pct, 0.5f, 0.0f, 100.0f, "%.0f%%",
                             ImGuiSliderFlags_AlwaysClamp)) {
            *alpha = color::Clamp01(pct / 100.0f);
            changed = true;
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    // --- «было/стало» и hex -------------------------------------------------
    {
        const float cmpW = std::round(fs * 4.6f);
        const float half = std::round(cmpW * 0.5f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float a = alpha ? color::Clamp01(*alpha) : 1.0f;
        ImGui::Dummy(ImVec2(original ? half : cmpW, frameH));
        const Box now{p0, ImVec2(p0.x + (original ? half : cmpW), p0.y + frameH)};
        DrawChecker(dl, now.Min, now.Max, ToU32(rgb, a));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("New colour"));
        if (original) {
            ImGui::SameLine(0.0f, 0.0f);
            const ImVec2 q0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##original", ImVec2(cmpW - half, frameH))) {
                std::memcpy(rgb, original, sizeof(float) * 3);
                if (alpha) *alpha = original[3];
                ImGui::MarkItemEdited(ImGui::GetItemID());
                changed = true;
            }
            lay.Original = {q0, ImVec2(q0.x + cmpW - half, q0.y + frameH)};
            DrawChecker(dl, lay.Original.Min, lay.Original.Max,
                        ToU32(original, alpha ? original[3] : 1.0f));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Original colour. Click to restore it."));
        }
        dl->AddRect(p0, ImVec2(p0.x + cmpW, p0.y + frameH), ImGui::GetColorU32(ImGuiCol_Border));

        ImGui::SameLine(0.0f, inner);
        // Буфер свой, пока поле не в работе: цвет мог поменяться ползунком, и
        // строка обязана показывать его. Во время набора текст держит ImGui, и
        // недонабранное «#8C5» не превращается в цвет, пока не станет им.
        static char buf[16];
        const ImGuiID hexId = ImGui::GetID("##hex");
        if (ImGui::GetActiveID() != hexId)
            std::snprintf(buf, sizeof buf, "%s", color::ToHex(rgb, alpha).c_str());
        // Пипетка — рядом с hex: оба отвечают на вопрос «какой именно цвет»,
        // один числом, другой — «вот этот, с экрана».
        const float dropW = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(W - cmpW - inner * 2.0f - dropW);
        if (ImGui::InputText("##hex", buf, sizeof buf,
                             ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll)) {
            float next[3] = {rgb[0], rgb[1], rgb[2]};
            float nextA = alpha ? *alpha : 1.0f;
            if (color::ParseHex(buf, next, alpha ? &nextA : nullptr)) {
                std::memcpy(rgb, next, sizeof next);
                if (alpha) *alpha = nextA;
                changed = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Hex colour: #RRGGBB or #RRGGBBAA. Ctrl+C and Ctrl+V copy and paste it."));
        ImGui::SameLine(0.0f, inner);
        if (EditorIcons::IconOnlyButton("eyedropper",
                                        T("Pick a colour from the screen — anywhere, even outside the "
                                          "editor. Click to take it, Esc to cancel."),
                                        eyedropper::Active()))
            eyedropper::Start(owner);
    }

    // --- каналы полосами ----------------------------------------------------
    //
    // Полоса с градиентом, а не голое число: видно, куда поедет цвет, если
    // потянуть канал, — и тянуть можно прямо по полосе.
    {
        auto mode = [&](const char* text, int m) {
            const bool on = g_channelMode == m;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(text)) g_channelMode = m;
            if (on) ImGui::PopStyleColor();
        };
        mode(T("RGB"), 0);
        ImGui::SameLine(0.0f, inner);
        mode(T("HSV"), 1);

        CurrentHsv(rgb, hsv);  // hex или «было» могли поменять цвет выше
        const float labelW = ImGui::CalcTextSize("W").x;
        const float numW = std::round(fs * 3.6f);
        const float barW = W - labelW - numW - inner * 2.0f;
        auto row = [&](int i, const char* name, float& t, ImU32 l, ImU32 r, bool hueStrip,
                       int maxValue, const char* fmt) -> bool {
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name);
            // Отступ — от ширины буквы, а не от координаты: «G» шире «I», и
            // полосы иначе начинались бы вразнобой.
            ImGui::SameLine(0.0f, labelW - ImGui::CalcTextSize(name).x + inner);
            Box box;
            bool edited = BarBehavior("##bar", ImVec2(barW, frameH), t, box);
            if (hueStrip) DrawHueStrip(dl, box);
            else DrawGradient(dl, box, l, r);
            DrawOutline(dl, box);
            DrawBarMarker(dl, box, t);
            ImGui::SameLine(0.0f, inner);
            int v = (int)std::lround(color::Clamp01(t) * (float)maxValue);
            ImGui::SetNextItemWidth(numW);
            if (ImGui::DragInt("##num", &v, 1.0f, 0, maxValue, fmt, ImGuiSliderFlags_AlwaysClamp)) {
                t = (float)v / (float)maxValue;
                edited = true;
            }
            ImGui::PopID();
            return edited;
        };
        if (g_channelMode == 0) {
            static const char* kNames[3] = {"R", "G", "B"};
            for (int c = 0; c < 3; ++c) {
                float lo[3] = {rgb[0], rgb[1], rgb[2]}, hi[3] = {rgb[0], rgb[1], rgb[2]};
                lo[c] = 0.0f;
                hi[c] = 1.0f;
                float t = color::Clamp01(rgb[c]);
                if (row(c, kNames[c], t, ToU32(lo), ToU32(hi), false, 255, "%d")) {
                    rgb[c] = t;
                    changed = true;
                }
            }
        } else {
            const float s0[3] = {hsv[0], 0.0f, hsv[2]}, s1[3] = {hsv[0], 1.0f, hsv[2]};
            const float v1[3] = {hsv[0], hsv[1], 1.0f};
            float c0[3], c1[3], cv[3];
            color::HsvToRgb(s0, c0);
            color::HsvToRgb(s1, c1);
            color::HsvToRgb(v1, cv);
            const float black[3] = {0, 0, 0};
            if (row(0, "H", hsv[0], 0, 0, true, 360, "%d")) applyHsv();
            if (row(1, "S", hsv[1], ToU32(c0), ToU32(c1), false, 100, "%d%%")) applyHsv();
            if (row(2, "V", hsv[2], ToU32(black), ToU32(cv), false, 100, "%d%%")) applyHsv();
        }
    }

    // --- недавние -----------------------------------------------------------
    const std::vector<ImVec4>& recent = color::Recent();
    if (!recent.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::TextDisabled("%s", T("Recent"));
        const float sw = frameH;
        const float gap = 4.0f;
        const int perRow = std::max(1, (int)((W + gap) / (sw + gap)));
        for (int i = 0; i < (int)recent.size(); ++i) {
            if (i % perRow) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(i);
            const ImGuiColorEditFlags f = ImGuiColorEditFlags_AlphaPreviewHalf |
                                          (alpha ? 0 : ImGuiColorEditFlags_NoAlpha);
            if (ImGui::ColorButton("##recent", recent[i], f, ImVec2(sw, sw))) {
                rgb[0] = recent[i].x;
                rgb[1] = recent[i].y;
                rgb[2] = recent[i].z;
                if (alpha) *alpha = recent[i].w;
                ImGui::MarkItemEdited(ImGui::GetItemID());
                changed = true;
            }
            ImGui::PopID();
        }
    }
    return changed;
}

} // namespace

bool ColorFieldAlpha(const char* label, float rgb[3], float* alpha, ColorFieldFlags flags) {
    ImGuiContext& g = *GImGui;
    if (ImGui::GetCurrentWindow()->SkipItems) return false;
    const ImGuiStyle& style = g.Style;
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const bool compact = (flags & ColorField_Compact) != 0;
    const bool readOnly = (flags & ColorField_ReadOnly) != 0;
    bool changed = false;

    ImGui::PushID(label);
    // Ключ поля для правок одним действием (вставка, пипетка) — см. OneShot.
    const ImGuiID owner = ImGui::GetID("##oneshot");
    ImGui::BeginGroup();
    const float h = ImGui::GetFrameHeight();
    const float w = compact ? h : std::max(ImGui::CalcItemWidth(), h);
    // Подпись справа — на одной линии с текстом других полей, а не прижата
    // к верхнему краю образца.
    ImGui::AlignTextToFramePadding();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + w, a.y + h);
    const bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    // ПКМ — меню: копировать, вставить, пипетка. Открывается и у поля
    // «только смотреть»: скопировать вычисленный цвет — законная просьба.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("###sage_color_ctx");
    DrawFieldSwatch(ImGui::GetWindowDrawList(), a, b, rgb, alpha, compact, hovered, readOnly);

    ImGuiWindow* picker = nullptr;
    if (readOnly) {
        if (hovered) ImGui::SetTooltip("%s", color::ToHex(rgb, alpha).c_str());
    } else {
        // ЦВЕТ ПЕРЕТАСКИВАЕТСЯ — формат ImGui, так что понимают и чужие поля.
        if (ImGui::BeginDragDropSource()) {
            const float c4[4] = {rgb[0], rgb[1], rgb[2], alpha ? *alpha : 1.0f};
            ImGui::SetDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F, c4, sizeof c4, ImGuiCond_Once);
            ImGui::ColorButton("##drag", ImVec4(c4[0], c4[1], c4[2], c4[3]),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf);
            ImGui::SameLine();
            ImGui::TextUnformatted(color::ToHex(rgb, alpha).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_3F)) {
                std::memcpy(rgb, p->Data, sizeof(float) * 3);
                changed = true;
            } else if (const ImGuiPayload* p4 = ImGui::AcceptDragDropPayload(IMGUI_PAYLOAD_TYPE_COLOR_4F)) {
                const float* c = (const float*)p4->Data;
                std::memcpy(rgb, c, sizeof(float) * 3);
                if (alpha) *alpha = c[3];
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }

        const ImGuiID popupId = ImGui::GetID("###sage_color");
        if (clicked) {
            g_session = Session{};
            g_session.Owner = popupId;
            std::memcpy(g_session.Original, rgb, sizeof(float) * 3);
            g_session.Original[3] = alpha ? *alpha : 1.0f;
            ImGui::OpenPopup("###sage_color");
            // Палитра — под полем, а не под курсором: так она не закрывает то,
            // что правят, и открывается на одном месте, куда ни щёлкни.
            ImGui::SetNextWindowPos(ImVec2(a.x, b.y + style.ItemSpacing.y));
        }
        if (hovered && !g.DragDropActive && !ImGui::IsPopupOpen("###sage_color"))
            ImGui::SetTooltip("%s", T("Click to pick a colour. Drag it onto another colour to copy it.\n"
                                      "Right-click: copy, paste, pick from the screen."));

        if (Sage::UI::MenuScope menu; ImGui::BeginPopup("###sage_color")) {
            picker = g.CurrentWindow;
            const float* original = g_session.Owner == popupId ? g_session.Original : nullptr;
            if (PickerBody(rgb, alpha, original, 0.0f, owner)) {
                changed = true;
                g_session.Edited = true;
            }
            ImGui::EndPopup();
        } else if (g_session.Owner == popupId) {
            // Палитра закрылась: выбранный цвет — в недавние.
            if (g_session.Edited) color::Remember(ImVec4(rgb[0], rgb[1], rgb[2], alpha ? *alpha : 1.0f));
            g_session = Session{};
        }
    }

    if (label != labelEnd) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::TextEx(label, labelEnd);
    }
    ImGui::EndGroup();
    // Меню — ЗА ГРУППОЙ поля: внутри неё щелчок по пункту группа сочла бы
    // своим жестом, и отмена правок получила бы лишнее «начало».
    ColorContextMenu(owner, rgb, alpha, readOnly);
    ImGui::PopID();

    // ПРАВКА В ПАЛИТРЕ — ПРАВКА ЭТОГО ПОЛЯ. Палитра — отдельное окно, и без этой
    // строки IsItemActive/IsItemActivated после поля молчали бы, пока тянут
    // ползунок в ней: отмена правок (TrackLastImGuiItem) не видела бы начала
    // жеста. Тот же приём, что у ImGui::ColorEdit4.
    if (picker && g.ActiveId != 0 && g.ActiveIdWindow == picker) g.LastItemData.ID = g.ActiveId;
    if (changed && g.LastItemData.ID != 0) ImGui::MarkItemEdited(g.LastItemData.ID);
    // Вставка и пипетка — последними: их «последний элемент» — скрытый элемент
    // жеста (см. OneShot), и он не должен быть перебит правкой в палитре.
    if (!readOnly && ApplyOneShot(owner, rgb, alpha)) changed = true;
    return changed;
}

bool ColorField3(const char* label, float rgb[3], ColorFieldFlags flags) {
    return ColorFieldAlpha(label, rgb, nullptr, flags);
}

bool ColorField4(const char* label, float rgba[4], ColorFieldFlags flags) {
    return ColorFieldAlpha(label, rgba, &rgba[3], flags);
}

bool ColorPickerInline(const char* id, float rgb[3], float* alpha, float width) {
    ImGui::PushID(id);
    const ImGuiID owner = ImGui::GetID("##oneshot");
    ImGui::BeginGroup();
    bool changed = PickerBody(rgb, alpha, nullptr, width, owner);
    ImGui::EndGroup();
    ImGui::PopID();
    if (ApplyOneShot(owner, rgb, alpha)) changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit())
        color::Remember(ImVec4(rgb[0], rgb[1], rgb[2], alpha ? *alpha : 1.0f));
    return changed;
}

// ============================================================================
//  Пипетка
// ============================================================================
namespace eyedropper {

void SetBackend(const Backend& backend) { g_backend = backend; }

void Start(ImGuiID owner) {
    g_drop = Dropper{};
    g_drop.Active = owner != 0;
    g_drop.Owner = owner;
}

bool Active() { return g_drop.Active; }
void Cancel() { g_drop = Dropper{}; }

bool FramebufferPoint(int* x, int* y) {
    if (!g_drop.Active || !g_drop.WantFb) return false;
    *x = g_drop.FbX;
    *y = g_drop.FbY;
    return true;
}

void SetFramebufferColor(const float rgb[3]) {
    std::memcpy(g_drop.Fb, rgb, sizeof(float) * 3);
    g_drop.FbValid = true;
}

void Frame() {
    if (!g_drop.Active) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* main = ImGui::GetMainViewport();

    // ГДЕ КУРСОР. Над своим окном — цвет из СВОЕГО КАДРА (точно тот, что на
    // экране, включая 3D-вьюпорт и прозрачность панелей); за окном — у системы.
    const bool mouseOk = ImGui::IsMousePosValid();
    const bool overOwn = mouseOk && (g_backend.OverOwnWindow ? g_backend.OverOwnWindow() : true) &&
                         io.MousePos.x >= main->Pos.x && io.MousePos.y >= main->Pos.y &&
                         io.MousePos.x < main->Pos.x + main->Size.x &&
                         io.MousePos.y < main->Pos.y + main->Size.y;
    g_drop.WantFb = overOwn;
    if (overOwn) {
        g_drop.FbX = (int)((io.MousePos.x - main->Pos.x) * io.DisplayFramebufferScale.x);
        g_drop.FbY = (int)((io.MousePos.y - main->Pos.y) * io.DisplayFramebufferScale.y);
        if (g_drop.FbValid) {
            std::memcpy(g_drop.Preview, g_drop.Fb, sizeof g_drop.Preview);
            g_drop.HasPreview = true;
        }
    } else if (g_backend.SampleScreen) {
        float c[3];
        if (g_backend.SampleScreen(c)) {
            std::memcpy(g_drop.Preview, c, sizeof c);
            g_drop.HasPreview = true;
        }
    }

    // ЛОВУШКА ЩЕЛЧКА поверх всего окна: щелчок пипетки берёт цвет, а не
    // нажимает кнопку, которая оказалась под курсором.
    ImGui::SetNextWindowPos(main->Pos);
    ImGui::SetNextWindowSize(main->Size);
    ImGui::SetNextWindowViewport(main->ID);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##sage_eyedropper", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove);
    ImGui::InvisibleButton("##catch", main->Size);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::End();

    // КНОПКИ — и свои, и системные: за окном ImGui щелчков не видит.
    const bool down = io.MouseDown[0] || (g_backend.GlobalMouseDown && g_backend.GlobalMouseDown(0));
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseDown[1] ||
                        (g_backend.GlobalEscapeDown && g_backend.GlobalEscapeDown()) ||
                        (g_backend.GlobalMouseDown && g_backend.GlobalMouseDown(1));
    if (cancel) {
        Cancel();
        return;
    }
    // Кнопка, которой запустили пипетку (пункт меню, кнопка в палитре), ещё
    // может быть зажата — её отпускание не выбор.
    if (!g_drop.Released) {
        if (!down) g_drop.Released = true;
    } else if (down) {
        if (g_drop.HasPreview) RequestSet(g_drop.Owner, g_drop.Preview, nullptr);
        Cancel();
        return;
    }

    // ЛУПА у курсора: цвет и hex. Сбоку от курсора, а не под ним: иначе
    // пипетка прочла бы из кадра свою же лупу.
    if (mouseOk && overOwn) {
        ImDrawList* dl = ImGui::GetForegroundDrawList(main);
        const float fs = ImGui::GetFontSize();
        const ImVec2 p0(io.MousePos.x + fs * 1.2f, io.MousePos.y + fs * 1.2f);
        const std::string hex = g_drop.HasPreview ? color::ToHex(g_drop.Preview) : std::string("…");
        const char* hint = T("Click — take, Esc — cancel");
        const float tw = std::max(ImGui::CalcTextSize(hex.c_str()).x, ImGui::CalcTextSize(hint).x);
        const ImVec2 p1(p0.x + fs * 2.6f + tw + fs * 0.8f, p0.y + fs * 2.9f);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_PopupBg), fs * 0.3f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), fs * 0.3f);
        const ImVec2 s0(p0.x + fs * 0.4f, p0.y + fs * 0.4f), s1(s0.x + fs * 2.0f, s0.y + fs * 2.1f);
        dl->AddRectFilled(s0, s1, g_drop.HasPreview ? ToU32(g_drop.Preview) : IM_COL32(0, 0, 0, 0), fs * 0.2f);
        dl->AddRect(s0, s1, ImGui::GetColorU32(ImGuiCol_Border), fs * 0.2f);
        dl->AddText(ImVec2(s1.x + fs * 0.5f, p0.y + fs * 0.35f), ImGui::GetColorU32(ImGuiCol_Text), hex.c_str());
        dl->AddText(ImVec2(s1.x + fs * 0.5f, p0.y + fs * 1.45f), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }
}

} // namespace eyedropper

} // namespace Sage::UI
