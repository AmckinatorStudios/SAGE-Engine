// ---------------------------------------------------------------------------
// EDITOR UI KIT: число, ползунок, переключатель, цвет и общие помощники
// (рамка, разделители, кнопки −/+, метки осей). Векторы — _Vector.cpp,
// список и кнопки — _Choice.cpp, компоновка и разделы — _Layout.cpp.
// ---------------------------------------------------------------------------
#include "InspectorControls.h"
#include "InspectorControlsDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ui/ColorPicker.h"

namespace sage::editor::ictl {
namespace {

std::function<void()>& Sink() {
    static std::function<void()> s;
    return s;
}

// Колесо над полем: одна правка — один шаг истории, пока колесо крутят без
// пауз (иначе каждый щелчок колеса стал бы отдельной отменой).
bool WheelGestureStart(ImGuiID id) {
    static ImGuiID s_last = 0;
    static double s_time = -10.0;
    const double now = ImGui::GetTime();
    const bool fresh = s_last != id || now - s_time > 0.6;
    s_last = id;
    s_time = now;
    return fresh;
}

float Clamp(float v, const NumOpts& o) { return std::clamp(v, o.Min, o.Max); }

glm::vec3 Vec3(const ImVec4& c) { return glm::vec3(c.x, c.y, c.z); }

// Кнопка −/+ внутри рамки: значок Tabler по центру, повтор при удержании;
// после секунды удержания шаг вдесятеро. Подсветка — только этого сегмента.
bool StepButton(EditorHost* host, const char* id, const char* icon, float* v, float dir, const NumOpts& o, ImVec2 at,
                float side, ImDrawFlags corners) {
    using EditorTheme::State;
    const float h = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(at);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool hit = ImGui::InvisibleButton(id, ImVec2(side, h));
    ImGui::PopItemFlag();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const State st = held ? State::Active : hovered ? State::Hover : State::Normal;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (const ImU32 fill = EditorTheme::SegmentFill(st))
        dl->AddRectFilled(at, ImVec2(at.x + side, at.y + h), fill, detail::K().Rounding,
                          corners ? corners : ImDrawFlags_RoundCornersNone);
    // Оба значка одного размера и строго по центру сегмента.
    const float is = std::floor(h * detail::K().StepIcon);
    EditorIcons::Overlay(dl, std::floor(at.x + (side - is) * 0.5f), std::floor(at.y + (h - is) * 0.5f), is, icon,
                         Vec3(EditorTheme::IconTint(st == State::Active ? State::Hover : st)));
    if (hovered) ImGui::SetTooltip("%s", T("Hold to repeat; after a second the step grows tenfold"));
    // Снимок — один на всё удержание: на нажатии, а не на каждом повторе.
    if (ImGui::IsItemActivated()) detail::Snapshot(host);
    if (!hit) return false;
    const bool accelerate = ImGui::GetIO().MouseDownDuration[0] > 1.0f;
    const float nv = Clamp(*v + dir * o.Step * (accelerate ? 10.0f : 1.0f), o);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

} // namespace

// --- detail: общее для всех файлов kit ------------------------------------------
namespace detail {

void Snapshot(EditorHost* host) {
    if (Sink()) Sink()();
    else if (host) host->PushUndoSnapshot();
}

void Track(EditorHost* host) {
    if (Sink()) {
        if (ImGui::IsItemActivated()) Sink()();
    } else if (host) {
        host->TrackLastImGuiItem();
    }
}

void Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, EditorTheme::State s, ImDrawFlags corners) {
    const float r = K().Rounding;
    dl->AddRectFilled(a, b, EditorTheme::ControlFill(s == EditorTheme::State::Focused ? EditorTheme::State::Normal : s),
                      r, corners);
    if (const ImU32 border = EditorTheme::ControlBorder(s)) dl->AddRect(a, b, border, r, corners, 1.0f);
}

void InnerSeparator(ImDrawList* dl, float x, float y, float h) {
    const float inset = K().SeparatorInset;
    const float t = K().SeparatorThickness;
    dl->AddRectFilled(ImVec2(std::floor(x), y + inset), ImVec2(std::floor(x) + t, y + h - inset),
                      EditorTheme::Color32(EditorTheme::Role::Line));
}

void InnerSeparatorH(ImDrawList* dl, float x, float y, float w) {
    const float inset = K().SeparatorInset;
    const float t = K().SeparatorThickness;
    dl->AddRectFilled(ImVec2(x + inset, std::floor(y)), ImVec2(x + w - inset, std::floor(y) + t),
                      EditorTheme::Color32(EditorTheme::Role::Line));
}

float StepSide(float width, float h) {
    return std::floor(std::clamp(width * 0.22f, h * K().StepButton, h * K().StepButtonWide));
}

bool StepFits(float width, float h) { return width >= std::floor(h * K().StepButton) * 2.0f + 36.0f; }

bool NumberIn(EditorHost* host, const char* id, float* v, const NumOpts& o, ImVec2 p, float width, bool buttons,
              ImDrawFlags leftCorners, ImDrawFlags rightCorners, bool* active) {
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    buttons = buttons && StepFits(width, h);
    const float side = buttons ? StepSide(width, h) : 0.0f;
    bool changed = buttons && StepButton(host, "##dec", "minus", v, -1.0f, o, p, side, leftCorners);
    ImGui::SetCursorScreenPos(ImVec2(p.x + side, p.y));
    ImGui::SetNextItemWidth(std::max(8.0f, width - 2.0f * side));
    // Поле прозрачное: заливку даёт общая рамка контрола; при наведении —
    // подсветка сегмента тем же цветом, что у кнопок.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, EditorTheme::Color(EditorTheme::Role::Hover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, EditorTheme::Color(EditorTheme::Role::Hover));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    // Поля у числа узкие: кнопки по бокам уже отделяют его от рамки, и
    // обычный отступ кадра только обрезал бы цифры в узкой колонке.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, ImGui::GetStyle().FramePadding.y));
    // Перетаскивание — по полю, двойной щелчок (или Ctrl+щелчок) — ввод числа.
    if (ImGui::DragFloat("##v", v, std::max(o.Step * 0.25f, 0.001f), o.Min, o.Max, o.Format,
                         ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    Track(host);
    const ImGuiID fieldId = ImGui::GetItemID();
    const bool fieldHovered = ImGui::IsItemHovered();
    // Колесо над полем меняет значение, а не листает инспектор.
    if (fieldHovered && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (WheelGestureStart(fieldId)) Snapshot(host);
            const float nv =
                Clamp(*v + (wheel > 0.0f ? 1.0f : -1.0f) * o.Step * (ImGui::GetIO().KeyShift ? 10.0f : 1.0f), o);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
        }
    }
    if (o.HasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != o.Reset) {
        Snapshot(host);
        *v = o.Reset;
        changed = true;
    }
    if (fieldHovered && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s", o.HasReset ? T("Drag, scroll or double-click to type. Right-click resets")
                                           : T("Drag, scroll or double-click to type"));
    if (active) *active = ImGui::IsItemActive() || ImGui::GetActiveID() == fieldId;
    if (buttons) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        InnerSeparator(dl, p.x + side, p.y, h);
        InnerSeparator(dl, p.x + width - side - 1.0f, p.y, h);
        changed |= StepButton(host, "##inc", "plus", v, 1.0f, o, ImVec2(p.x + width - side, p.y), side, rightCorners);
    }
    // Курсор — за поле: следующий элемент строки встаёт после рамки.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
    return changed;
}

float TagWidth(const char* const* tags, int n) {
    float w = 0.0f;
    for (int i = 0; i < n; ++i) w = std::max(w, ImGui::CalcTextSize(tags[i]).x);
    return std::floor(w + K().TagPad);
}

void TagPill(ImDrawList* dl, ImVec2 p, float w, float h, const char* text, int axisColor) {
    const float in = K().TagInset;
    const ImVec2 a(p.x + in, p.y + in), b(p.x + w - 1.0f, p.y + h - in);
    const bool axis = axisColor >= 0;
    dl->AddRectFilled(a, b,
                      axis ? ImGui::GetColorU32(EditorTheme::AxisColor(axisColor))
                           : EditorTheme::Color32(EditorTheme::Role::SurfaceAlt),
                      std::max(2.0f, K().Rounding - 2.0f));
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor(p.y + (h - ts.y) * 0.5f)),
                axis ? IM_COL32(255, 255, 255, 240) : EditorTheme::Color32(EditorTheme::Role::TextDim), text);
}

std::string PrefKey(const char* what, ImGuiID id) {
    return std::string("inspector.") + what + "." + std::to_string(id);
}

} // namespace detail

using detail::K;

UndoScope::UndoScope(std::function<void()> push) : m_prev(std::move(Sink())) { Sink() = std::move(push); }
UndoScope::~UndoScope() { Sink() = std::move(m_prev); }

NumOpts OptsForRange(float min, float max) {
    NumOpts o;
    o.Min = min;
    o.Max = max;
    const float span = (max > min && max < FLT_MAX * 0.5f && min > -FLT_MAX * 0.5f) ? max - min : 1e9f;
    if (span <= 2.0f) {
        o.Step = 0.01f;
        o.Format = "%.2f";
    } else if (span <= 20.0f) {
        o.Step = 0.1f;
        o.Format = "%.1f";
    } else if (span <= 10000.0f) {
        o.Step = 1.0f;
        o.Format = "%.0f";
    } else {
        // Почти без границ (значение шкалы, смещение): число может быть и
        // долей, и тысячами — показываем как есть, без округления до целых.
        o.Step = 1.0f;
        o.Format = "%.6g";
    }
    return o;
}

float NumberWidth() { return std::clamp(ImGui::GetContentRegionAvail().x, K().NumberMin, K().NumberMax); }

bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width, NumLook look) {
    ImGui::PushID(id);
    if (width <= 0.0f) width = NumberWidth();
    // ОДИН КОНТРОЛ: [−|значение|+] общей рамкой высотой с поле; кнопки —
    // сегменты со значками Tabler, отделены тонкими линиями.
    const float h = ImGui::GetFrameHeight();
    const bool buttons = look == NumLook::Standard && detail::StepFits(width, h);
    width = std::max(width, buttons ? detail::StepSide(width, h) * 2.0f + 28.0f : 24.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    detail::Frame(dl, p, ImVec2(p.x + width, p.y + h), EditorTheme::State::Normal);
    bool active = false;
    const bool changed = detail::NumberIn(&host, "n", v, o, p, width, buttons, ImDrawFlags_RoundCornersLeft,
                                          ImDrawFlags_RoundCornersRight, &active);
    if (active)
        dl->AddRect(p, ImVec2(p.x + width, p.y + h), EditorTheme::ControlBorder(EditorTheme::State::Focused),
                    K().Rounding);
    ImGui::PopID();
    return changed;
}

bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o, NumLook look) {
    ImGui::PushID(label);
    Label(label);
    const bool changed = NumberCore(host, "n", v, o, 0.0f, look);
    ImGui::PopID();
    return changed;
}

bool Integer(EditorHost& host, const char* label, int* v, int min, int max, bool hasReset, int reset) {
    NumOpts o;
    o.Min = (float)min;
    o.Max = (float)max;
    o.HasReset = hasReset;
    o.Reset = (float)reset;
    float f = (float)*v;
    if (!Number(host, label, &f, o)) return false;
    *v = (int)std::lround(f);
    return true;
}

bool SliderNumber(EditorHost& host, const char* label, float* v, float min, float max, const char* format,
                  bool hasReset, float reset) {
    ImGui::PushID(label);
    Label(label);
    const float numW = std::floor(K().FieldMin);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(40.0f, w - numW - K().TagGap));
    bool changed = ImGui::SliderFloat("##s", v, min, max, "", ImGuiSliderFlags_AlwaysClamp);
    detail::Track(&host);
    if (hasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != reset) {
        detail::Snapshot(&host);
        *v = reset;
        changed = true;
    }
    ImGui::SameLine(0.0f, K().TagGap);
    ImGui::SetNextItemWidth(numW);
    if (ImGui::InputFloat("##n", v, 0.0f, 0.0f, format)) {
        *v = std::clamp(*v, min, max);
        changed = true;
    }
    detail::Track(&host);
    ImGui::PopID();
    return changed;
}

namespace {
// «Таблетка» переключателя; true — нажали (значение меняет вызывающий:
// Switch сперва снимает историю, Check — нет).
bool SwitchCore(const char* label, bool v0, SwitchLook look) {
    bool* v = &v0;
    ImGui::PushID(label);
    if (look == SwitchLook::Standard) Label(label);
    const float fh = ImGui::GetFrameHeight();
    const float h = std::floor(fh * K().SwitchHeight);
    const float w = std::floor(h * K().SwitchWidth);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (fh - h) * 0.5f;
    // Inline — подпись справа от «таблетки»; Standard — «Вкл/Выкл».
    const char* end = label + std::strlen(label);
    if (const char* hash = std::strstr(label, "##")) end = hash;
    const std::string inlineText(label, end);
    const char* text = look == SwitchLook::Inline ? inlineText.c_str() : (*v ? T("On") : T("Off"));
    const float textW = ImGui::CalcTextSize(text).x;
    const bool hit = ImGui::InvisibleButton("##sw", ImVec2(w + K().LabelGap + textW, fh));
    const bool hovered = ImGui::IsItemHovered();
    if (hit) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = *v ? EditorTheme::Color32(hovered ? EditorTheme::Role::AccentHover : EditorTheme::Role::Accent)
                           : EditorTheme::ControlFill(hovered ? EditorTheme::State::Hover : EditorTheme::State::Normal);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), track, h * 0.5f);
    const float k = h - 4.0f;
    const float kx = *v ? p.x + w - 2.0f - k : p.x + 2.0f;
    dl->AddCircleFilled(ImVec2(kx + k * 0.5f, y + h * 0.5f), k * 0.5f,
                        EditorTheme::Color32(*v ? EditorTheme::Role::TextOnAccent : EditorTheme::Role::TextDim));
    dl->AddText(ImVec2(p.x + w + K().LabelGap, p.y + ImGui::GetStyle().FramePadding.y),
                EditorTheme::Color32(*v || look == SwitchLook::Inline ? EditorTheme::Role::Text
                                                                      : EditorTheme::Role::TextDim),
                text);
    ImGui::PopID();
    return hit;
}
} // namespace

bool Switch(EditorHost& host, const char* label, bool* v, SwitchLook look) {
    if (!SwitchCore(label, *v, look)) return false;
    detail::Snapshot(&host);
    *v = !*v;
    return true;
}

bool Check(const char* label, bool* v) {
    if (!SwitchCore(label, *v, SwitchLook::Inline)) return false;
    *v = !*v;
    return true;
}

bool Color(EditorHost& host, const char* label, glm::vec4* rgba, ColorLook look) {
    ImGui::PushID(label);
    // [■][#HEX][%] целиком: уже — подпись над строкой, а не обрезанный процент.
    Label(label, look == ColorLook::Standard ? 176.0f : 0.0f);
    bool changed = false;
    // Образец — тот же, что во всём редакторе: щелчок открывает палитру.
    if (Sage::UI::ColorField4("##sw", &rgba->x, Sage::UI::ColorField_Compact)) changed = true;
    detail::Track(&host);
    if (look == ColorLook::Compact) {
        ImGui::PopID();
        return changed;
    }
    ImGui::SameLine(0.0f, K().TagGap);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%s", Sage::UI::color::ToHex(&rgba->x).c_str());
    ImGui::SetNextItemWidth(std::max(70.0f, ImGui::GetContentRegionAvail().x - 70.0f));
    if (ImGui::InputText("##hex", hex, sizeof(hex), ImGuiInputTextFlags_CharsUppercase)) {
        float rgb[3];
        if (Sage::UI::color::ParseHex(hex, rgb)) {
            rgba->r = rgb[0];
            rgba->g = rgb[1];
            rgba->b = rgb[2];
            changed = true;
        }
    }
    detail::Track(&host);
    ImGui::SameLine(0.0f, K().TagGap);
    // Прозрачность — процентами, тем же числом, что и альфа цвета (одна
    // величина, а не Alpha и Opacity порознь).
    float pct = rgba->a * 100.0f;
    ImGui::SetNextItemWidth(std::max(50.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::DragFloat("##a", &pct, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        rgba->a = pct / 100.0f;
        changed = true;
    }
    detail::Track(&host);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Opacity"));
    ImGui::PopID();
    return changed;
}

bool Color3(EditorHost& host, const char* label, glm::vec3* rgb, ColorLook look) {
    ImGui::PushID(label);
    Label(label, look == ColorLook::Standard ? 150.0f : 0.0f);
    bool changed = Sage::UI::ColorField3("##sw", &rgb->x, Sage::UI::ColorField_Compact);
    detail::Track(&host);
    if (look == ColorLook::Standard) {
        ImGui::SameLine(0.0f, K().TagGap);
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%s", Sage::UI::color::ToHex(&rgb->x).c_str());
        ImGui::SetNextItemWidth(std::max(70.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputText("##hex", hex, sizeof(hex), ImGuiInputTextFlags_CharsUppercase)) {
            float c[3];
            if (Sage::UI::color::ParseHex(hex, c)) {
                *rgb = glm::vec3(c[0], c[1], c[2]);
                changed = true;
            }
        }
        detail::Track(&host);
    }
    ImGui::PopID();
    return changed;
}

} // namespace sage::editor::ictl
