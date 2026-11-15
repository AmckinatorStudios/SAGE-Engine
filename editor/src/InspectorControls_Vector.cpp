// ---------------------------------------------------------------------------
// EDITOR UI KIT: ВЕКТОРЫ — Vector2/3/4, пары (X/Y, W/H, Min/Max), размер со
// связью пропорций, стороны (L/T/R/B).
//
// Вектор — ОДИН контрол: общая рамка, равные сегменты через тонкие
// разделители, в сегменте — метка оси, число и компактные −/+. Не три
// отдельных поля с зазорами: глаз читает «это одна позиция», а не «три числа
// где-то рядом». Узко — Segmented → Compact (без −/+) → Stacked (ось на
// строку); подпись — над полем раньше, чем сегменты начнут сжиматься.
// ---------------------------------------------------------------------------
#include "InspectorControls.h"
#include "InspectorControlsDetail.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor::ictl {

using detail::K;

namespace {

const char* const kAxisTags[4] = {"X", "Y", "Z", "W"};

// Сколько ширины нужно сегменту вида look.
float SegmentNeed(VecLook look, float tagW, float h) {
    if (look == VecLook::Segmented) return tagW + std::floor(h * K().StepButton) * 2.0f + 36.0f;
    return tagW + K().SegmentMin;
}

// Значок-переключатель связи (замок) в конце строки.
bool LinkIcon(bool linked, const char* onTip, const char* offTip) {
    ImGui::SameLine(0.0f, K().TagGap);
    return EditorIcons::IconOnlyButton(linked ? "lock" : "unlock", linked ? onTip : offTip, linked);
}

bool TagsAreAxes(const char* const* tags) { return !tags || std::strcmp(tags[0], "X") == 0; }

} // namespace

VecLook FitVecLook(VecLook wanted, float width, int n, float tagW, float h) {
    if (n <= 0) return wanted;
    const float per = width / (float)n;
    if (wanted == VecLook::Segmented && per >= SegmentNeed(VecLook::Segmented, tagW, h)) return VecLook::Segmented;
    if (wanted != VecLook::Stacked && per >= SegmentNeed(VecLook::Compact, tagW, h)) return VecLook::Compact;
    return VecLook::Stacked;
}

bool VectorCore(EditorHost& host, const char* id, float* v, int n, const NumOpts& o, const VecOpts& vo) {
    return detail::VectorImpl(&host, id, v, n, o, vo);
}

bool DragVec(EditorHost* host, const char* label, float* v, int n, float speed, float min, float max, const char* format) {
    NumOpts o;
    o.Step = speed * 4.0f;   // кнопки и колесо — шаг вчетверо от перетаскивания
    if (max > min) {
        o.Min = min;
        o.Max = max;
    }
    o.Format = format;
    VecOpts vo;
    // Подпись (видимая часть) — слева, как у всех полей kit.
    const char* end = label + std::strlen(label);
    if (const char* hash = std::strstr(label, "##")) end = hash;
    ImGui::PushID(label);
    if (end != label) Label(label, (float)n * SegmentNeed(VecLook::Compact, detail::TagWidth(kAxisTags, n),
                                                          ImGui::GetFrameHeight()));
    const bool changed = detail::VectorImpl(host, "v", v, n, o, vo);
    ImGui::PopID();
    return changed;
}

namespace detail {
bool VectorImpl(EditorHost* hostPtr, const char* id, float* v, int n, const NumOpts& o, const VecOpts& vo) {
    ImGui::PushID(id);
    const char* const* tags = vo.Tags ? vo.Tags : kAxisTags;
    const float h = ImGui::GetFrameHeight();
    const float tagW = detail::TagWidth(tags, n);
    const float width = std::max(n * (tagW + K().SegmentMin), ImGui::GetContentRegionAvail().x - vo.Trailing);
    const VecLook look = FitVecLook(vo.Look, width, n, tagW, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;
    float totalH = h;
    if (look == VecLook::Stacked) {
        // Ось на строку: каждая — своя рамка той же ширины, столбцы ровные.
        const float step = h + ImGui::GetStyle().ItemSpacing.y;
        totalH = step * (float)n - ImGui::GetStyle().ItemSpacing.y;
        for (int i = 0; i < n; ++i) {
            const ImVec2 r(p.x, p.y + step * (float)i);
            detail::Frame(dl, r, ImVec2(r.x + width, r.y + h), EditorTheme::State::Normal);
            detail::TagPill(dl, r, tagW, h, tags[i], vo.AxisColors ? i : -1);
            bool active = false;
            ImGui::PushID(i);
            changed |= detail::NumberIn(hostPtr, "v", &v[i], o, ImVec2(r.x + tagW, r.y), width - tagW, true,
                                        ImDrawFlags_RoundCornersNone, ImDrawFlags_RoundCornersRight, &active);
            ImGui::PopID();
            if (active)
                dl->AddRect(r, ImVec2(r.x + width, r.y + h), EditorTheme::ControlBorder(EditorTheme::State::Focused),
                            K().Rounding);
        }
    } else {
        // Одна рамка, равные сегменты; остаток ширины — последнему.
        detail::Frame(dl, p, ImVec2(p.x + width, p.y + h), EditorTheme::State::Normal);
        const float seg = std::floor(width / (float)n);
        bool anyActive = false;
        for (int i = 0; i < n; ++i) {
            const float sx = p.x + seg * (float)i;
            const float sw = i + 1 == n ? width - seg * (float)i : seg;
            if (i) detail::InnerSeparator(dl, sx, p.y, h);
            detail::TagPill(dl, ImVec2(sx, p.y), tagW, h, tags[i], vo.AxisColors ? i : -1);
            bool active = false;
            ImGui::PushID(i);
            changed |= detail::NumberIn(hostPtr, "v", &v[i], o, ImVec2(sx + tagW, p.y), sw - tagW,
                                        look == VecLook::Segmented, ImDrawFlags_RoundCornersNone,
                                        i + 1 == n ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone,
                                        &active);
            ImGui::PopID();
            anyActive |= active;
        }
        if (anyActive)
            dl->AddRect(p, ImVec2(p.x + width, p.y + h), EditorTheme::ControlBorder(EditorTheme::State::Focused),
                        K().Rounding);
    }
    // Весь вектор — один элемент строки: значок справа (SameLine) встаёт
    // за рамкой, а не за последним сегментом.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, totalH));
    ImGui::PopID();
    return changed;
}
} // namespace detail

bool VectorN(EditorHost& host, const char* label, float* v, int n, const NumOpts& o, const VecOpts& vo) {
    ImGui::PushID(label);
    const char* const* tags = vo.Tags ? vo.Tags : kAxisTags;
    const float h = ImGui::GetFrameHeight();
    const float tagW = detail::TagWidth(tags, n);
    // Подпись — над полем раньше, чем сегменты начнут терять −/+.
    Label(label, (float)n * SegmentNeed(vo.Look == VecLook::Stacked ? VecLook::Compact : vo.Look, tagW, h) +
                     vo.Trailing);
    // Правый щелчок по подписи — сброс всей строки (у поля оси — одной оси).
    if (o.HasReset) {
        bool differs = false;
        for (int i = 0; i < n; ++i) differs |= v[i] != o.Reset;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && differs) {
            detail::Snapshot(&host);
            for (int i = 0; i < n; ++i) v[i] = o.Reset;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Right-click: reset the whole row"));
    }
    const bool changed = VectorCore(host, "v", v, n, o, vo);
    ImGui::PopID();
    return changed;
}

bool Vector2(EditorHost& host, const char* label, glm::vec2* v, const NumOpts& o, const VecOpts& vo) {
    return VectorN(host, label, &v->x, 2, o, vo);
}

bool Vector3(EditorHost& host, const char* label, glm::vec3* v, const NumOpts& o, float trailing, VecLook look) {
    VecOpts vo;
    vo.Trailing = trailing;
    vo.Look = look;
    return VectorN(host, label, &v->x, 3, o, vo);
}

bool Vector4(EditorHost& host, const char* label, glm::vec4* v, const NumOpts& o, const VecOpts& vo) {
    return VectorN(host, label, &v->x, 4, o, vo);
}

bool Pair(EditorHost& host, const char* id, float* a, float* b, const char* tagA, const char* tagB,
          const NumOpts& o) {
    const char* tags[2] = {tagA, tagB};
    float v[2] = {*a, *b};
    VecOpts vo;
    vo.Tags = tags;
    vo.AxisColors = TagsAreAxes(tags);
    const bool changed = VectorCore(host, id, v, 2, o, vo);
    *a = v[0];
    *b = v[1];
    return changed;
}

bool PairRow(EditorHost& host, const char* label, float* a, float* b, const char* tagA, const char* tagB,
             const NumOpts& o) {
    const char* tags[2] = {tagA, tagB};
    float v[2] = {*a, *b};
    VecOpts vo;
    vo.Tags = tags;
    vo.AxisColors = TagsAreAxes(tags);
    const bool changed = VectorN(host, label, v, 2, o, vo);
    *a = v[0];
    *b = v[1];
    return changed;
}

bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o) {
    VecOpts vo;
    vo.Tags = axes;
    vo.AxisColors = TagsAreAxes(axes);
    return VectorCore(host, id, v, n, o, vo);
}

bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o) {
    return PairRow(host, title, &v->x, &v->y, "X", "Y", o);
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
    bool linked = prefs::GetBool(detail::PrefKey("link", id), false);
    // «Размер: [W .. | H ..] [замок]» — замок справа от всего контрола; при
    // связи вторая сторона следует за правленой в прежнем соотношении.
    static const char* const kTags[2] = {"W", "H"};
    VecOpts vo;
    vo.Tags = kTags;
    vo.AxisColors = false;
    vo.Trailing = ImGui::GetFrameHeight() + K().TagGap;
    const glm::vec2 before = *v;
    bool changed = VectorN(host, title, &v->x, 2, o, vo);
    if (LinkIcon(linked, T("Proportions linked: the height follows the width"), T("Link proportions"))) {
        linked = !linked;
        prefs::SetBool(detail::PrefKey("link", id), linked);
    }
    if (changed && linked && !(before == *v)) *v = LinkedSize(before, *v, o);
    ImGui::PopID();
    return changed;
}

bool Range(EditorHost& host, const char* title, float* min, float* max, const NumOpts& o) {
    // Min и Max — короткими подписями, как X и Y: длинные «Минимум /
    // Максимум» съедали половину ширины полей.
    return PairRow(host, title, min, max, "Min", "Max", o);
}

bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o) {
    ImGui::PushID(title);
    const ImGuiID id = ImGui::GetID("sides");
    bool linked = prefs::GetBool(detail::PrefKey("link", id), false);
    VecOpts vo;
    vo.AxisColors = false;
    vo.Trailing = ImGui::GetFrameHeight() + K().TagGap;
    bool changed = false;
    if (linked) {
        // Все стороны вместе — одно число.
        const char* tags[1] = {T("All")};
        vo.Tags = tags;
        float all = v->x;
        if (VectorN(host, title, &all, 1, o, vo)) {
            *v = glm::vec4(all);
            changed = true;
        }
    } else {
        static const char* const kTags[4] = {"L", "T", "R", "B"};
        vo.Tags = kTags;
        // Порядок полей вектора — L, T, R, B: x, y, z, w.
        changed = VectorN(host, title, &v->x, 4, o, vo);
    }
    if (LinkIcon(linked, T("All sides together: one value for every side"), T("Link sides"))) {
        linked = !linked;
        prefs::SetBool(detail::PrefKey("link", id), linked);
        if (linked) *v = glm::vec4(v->x);
    }
    ImGui::PopID();
    return changed;
}

} // namespace sage::editor::ictl
