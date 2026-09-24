#include "ui/ColorPicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"

#include "Localization.h"
#include "ui/UI.h"

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
bool PickerBody(float rgb[3], float* alpha, const float* original, float width) {
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
        ImGui::SetNextItemWidth(W - cmpW - inner);
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
    }

    // --- каналы полосами ----------------------------------------------------
    //
    // Полоса с градиентом, а не голое число: видно, куда поедет цвет, если
    // потянуть канал, — и тянуть можно прямо по полосе.
    {
        auto mode = [&](const char* text, int m) {
            const bool on = g_channelMode == m;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(text)) g_channelMode = m;
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
            ImGui::SetTooltip("%s", T("Click to pick a colour. Drag it onto another colour to copy it."));

        if (Sage::UI::MenuScope menu; ImGui::BeginPopup("###sage_color")) {
            picker = g.CurrentWindow;
            const float* original = g_session.Owner == popupId ? g_session.Original : nullptr;
            if (PickerBody(rgb, alpha, original, 0.0f)) {
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
    ImGui::PopID();

    // ПРАВКА В ПАЛИТРЕ — ПРАВКА ЭТОГО ПОЛЯ. Палитра — отдельное окно, и без этой
    // строки IsItemActive/IsItemActivated после поля молчали бы, пока тянут
    // ползунок в ней: отмена правок (TrackLastImGuiItem) не видела бы начала
    // жеста. Тот же приём, что у ImGui::ColorEdit4.
    if (picker && g.ActiveId != 0 && g.ActiveIdWindow == picker) g.LastItemData.ID = g.ActiveId;
    if (changed && g.LastItemData.ID != 0) ImGui::MarkItemEdited(g.LastItemData.ID);
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
    ImGui::BeginGroup();
    const bool changed = PickerBody(rgb, alpha, nullptr, width);
    ImGui::EndGroup();
    ImGui::PopID();
    if (ImGui::IsItemDeactivatedAfterEdit())
        color::Remember(ImVec4(rgb[0], rgb[1], rgb[2], alpha ? *alpha : 1.0f));
    return changed;
}

} // namespace Sage::UI
