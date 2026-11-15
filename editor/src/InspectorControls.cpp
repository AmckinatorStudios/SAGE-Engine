#include "InspectorControls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor::ictl {
namespace {

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

// Кнопка −/+ с повтором при удержании; после секунды удержания шаг
// вдесятеро — длинный путь не проходят по единице.
bool StepButton(EditorHost& host, const char* text, float* v, float dir, const NumOpts& o, float side) {
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool hit = ImGui::Button(text, ImVec2(side, side));
    ImGui::PopItemFlag();
    // Снимок — один на всё удержание: на нажатии, а не на каждом повторе.
    if (ImGui::IsItemActivated()) host.PushUndoSnapshot();
    if (!hit) return false;
    const bool accelerate = ImGui::GetIO().MouseDownDuration[0] > 1.0f;
    const float nv = Clamp(*v + dir * o.Step * (accelerate ? 10.0f : 1.0f), o);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

std::string PrefKey(const char* what, ImGuiID id) { return std::string("inspector.") + what + "." + std::to_string(id); }

} // namespace

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

bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width) {
    ImGui::PushID(id);
    const float side = ImGui::GetFrameHeight();
    const float spacing = 2.0f;
    if (width <= 0.0f) width = ImGui::CalcItemWidth();
    bool changed = StepButton(host, "-", v, -1.0f, o, side);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Hold to repeat; after a second the step grows tenfold"));
    ImGui::SameLine(0.0f, spacing);
    ImGui::SetNextItemWidth(std::max(30.0f, width - 2.0f * (side + spacing)));
    // Перетаскивание — по полю, двойной щелчок (или Ctrl+щелчок) — ввод числа.
    if (ImGui::DragFloat("##v", v, std::max(o.Step * 0.25f, 0.001f), o.Min, o.Max, o.Format,
                         ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
    }
    host.TrackLastImGuiItem();
    const ImGuiID fieldId = ImGui::GetItemID();
    // Колесо над полем меняет значение, а не листает инспектор.
    if (ImGui::IsItemHovered() && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (WheelGestureStart(fieldId)) host.PushUndoSnapshot();
            const float nv = Clamp(*v + (wheel > 0.0f ? 1.0f : -1.0f) * o.Step * (ImGui::GetIO().KeyShift ? 10.0f : 1.0f), o);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
        }
    }
    if (o.HasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != o.Reset) {
        host.PushUndoSnapshot();
        *v = o.Reset;
        changed = true;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        ImGui::SetTooltip("%s", o.HasReset ? T("Drag, scroll or double-click to type. Right-click resets")
                                           : T("Drag, scroll or double-click to type"));
    ImGui::SameLine(0.0f, spacing);
    changed |= StepButton(host, "+", v, 1.0f, o, side);
    ImGui::PopID();
    return changed;
}

bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o) {
    ImGui::PushID(label);
    const bool changed = NumberCore(host, "n", v, o);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
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
    const float numW = 64.0f;
    const float w = ImGui::CalcItemWidth();
    ImGui::SetNextItemWidth(std::max(40.0f, w - numW - 4.0f));
    bool changed = ImGui::SliderFloat("##s", v, min, max, format);
    host.TrackLastImGuiItem();
    if (hasReset && ImGui::IsItemClicked(ImGuiMouseButton_Right) && *v != reset) {
        host.PushUndoSnapshot();
        *v = reset;
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(numW);
    if (ImGui::InputFloat("##n", v, 0.0f, 0.0f, format)) {
        *v = std::clamp(*v, min, max);
        changed = true;
    }
    host.TrackLastImGuiItem();
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

void GroupTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
}

bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o) {
    ImGui::PushID(id);
    bool changed = false;
    // Подписи осей — одной ширины: поля под ними встают ровным столбцом.
    float tag = 0.0f;
    for (int i = 0; i < n; ++i) tag = std::max(tag, ImGui::CalcTextSize(axes[i]).x);
    tag += ImGui::GetStyle().ItemSpacing.x * 2.0f;
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(axes[i]);
        ImGui::SameLine(x0 + tag);
        changed |= NumberCore(host, "a", &v[i], o, std::max(80.0f, ImGui::GetContentRegionAvail().x));
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    GroupTitle(title);
    static const char* const kXY[2] = {"X", "Y"};
    return Axes(host, title, &v->x, 2, kXY, o);
}

glm::vec2 LinkedSize(glm::vec2 before, glm::vec2 now, const NumOpts& o) {
    if (before.x <= 0.0f || before.y <= 0.0f) return now;
    const float ratio = before.y / before.x;
    if (now.x != before.x) now.y = std::clamp(now.x * ratio, o.Min, o.Max);
    else if (now.y != before.y) now.x = std::clamp(now.y / ratio, o.Min, o.Max);
    return now;
}

bool Size(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("size");
    bool combined = prefs::GetBool("inspector.sizeCombined", false);
    bool linked = prefs::GetBool(PrefKey("link", id), false);
    GroupTitle(title);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("fit", combined ? T("Show width and height on separate rows")
                                                    : T("Show width and height in one row"), combined)) {
        combined = !combined;
        prefs::SetBool("inspector.sizeCombined", combined);
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(linked ? "lock" : "unlock",
                                    linked ? T("Proportions linked: the height follows the width")
                                           : T("Link proportions"), linked)) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
    }
    const glm::vec2 before = *v;
    bool changed = false;
    if (combined) {
        // «240 × 80» — два числа в одной строке.
        const float w = ImGui::GetContentRegionAvail().x;
        const float cross = ImGui::CalcTextSize("×").x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        const float half = std::max(70.0f, (w - cross) * 0.5f);
        changed |= NumberCore(host, "w", &v->x, o, half);
        ImGui::SameLine();
        ImGui::TextUnformatted("×");
        ImGui::SameLine();
        changed |= NumberCore(host, "h", &v->y, o, half);
    } else {
        const char* axes[2] = {T("Width"), T("Height")};
        changed = Axes(host, "wh", &v->x, 2, axes, o);
    }
    // Связь пропорций: правили одну сторону — вторая следует за ней.
    if (changed && linked) *v = LinkedSize(before, *v, o);
    ImGui::PopID();
    return changed;
}

bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("sides");
    bool linked = prefs::GetBool(PrefKey("link", id), false);
    GroupTitle(title);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton(linked ? "lock" : "unlock",
                                    linked ? T("All sides together: one value for every side")
                                           : T("Link sides"), linked)) {
        linked = !linked;
        prefs::SetBool(PrefKey("link", id), linked);
        if (linked) *v = glm::vec4(v->x);
    }
    bool changed = false;
    if (linked) {
        float all = v->x;
        static const char* const kAll[1] = {"="};
        if (Axes(host, "all", &all, 1, kAll, o)) {
            *v = glm::vec4(all);
            changed = true;
        }
    } else {
        const char* axes[4] = {T("Left"), T("Top"), T("Right"), T("Bottom")};
        changed = Axes(host, "ltrb", &v->x, 4, axes, o);
    }
    ImGui::PopID();
    return changed;
}

bool Section(const char* id, const char* title, bool defaultOpen) {
    const std::string key = std::string("inspector.section.") + id;
    const bool saved = prefs::GetBool(key, defaultOpen);
    ImGui::SetNextItemOpen(saved, ImGuiCond_Once);
    ImGui::Spacing();
    // Без цветной плашки: заголовок — строкой с треугольником и линией.
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    const std::string label = std::string(title) + "###sec_" + id;
    const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                                                           ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                           ImGuiTreeNodeFlags_NoAutoOpenOnLog);
    ImGui::PopStyleColor();
    if (ImGui::IsItemToggledOpen()) prefs::SetBool(key, open);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), ImGui::GetColorU32(ImGuiCol_Separator));
    return open;
}

} // namespace sage::editor::ictl
