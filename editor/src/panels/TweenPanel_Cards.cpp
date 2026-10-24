#include "TweenPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"   // PushMultiItemsWidths: поля осей ровно по ширине колонки

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ObjectSlot.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "../ui/ColorPicker.h"
#include "../ui/UI.h"
#include "sage/anim/AnimProperty.h"
#include "sage/scene/Scene.h"
#include "TweenPanelDetail.h"
namespace anim = sage::anim;



// Карточки и дорожки твинов, расширенные настройки.
// --- Карточка свойства ---------------------------------------------------------------
//
//   [значок] Позиция · метры              [кривая ▾] [⋯]
//   Начало  [X 0.00][Y 0.00][Z 0.00]   [↻ как сейчас]
//   Конец   [X 0.00][Y 1.00][Z 0.00]   [⌖ взять из объекта]
//   (конец = начало — свойство не изменится: …)
//
// Подписи «Начало/Конец» и буквы осей — потому что раньше это были два
// безымянных ряда чисел со стрелкой, и что из них что, приходилось угадывать.
void TweenPanel::DrawCard(EditorHost& host, anim::TweenClip& clip, size_t index, entt::entity target) {
    anim::TweenTrack& t = clip.Tracks[index];
    const anim::PropertyType* p = anim::FindProperty(t.Property);
    const bool gap = t.Property.empty();
    const bool play = host.InPlayMode();
    entt::registry& reg = host.CurrentScene().Registry();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fh = ImGui::GetFrameHeight();
    const float pad = 6.0f;
    const bool present = p && target != entt::null && anim::HasProperty(*p, reg, target);

    // Значения — до раскладки: от них зависит, нужна ли строка-предупреждение.
    const bool scale = t.Property == "object.scale";
    const glm::vec4 cur = present ? Current(reg, target, *p) : t.From;
    glm::vec4 from = t.FromCurrent ? cur : t.From;
    const int comps = p ? p->Components : 1;
    bool still = !gap && p != nullptr;
    for (int k = 0; k < std::min(comps, 4) && still; ++k) still = std::fabs(from[k] - t.To[k]) < 1e-4f;

    ImGui::PushID((int)index);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float row = fh + st.ItemSpacing.y;
    const float warnH = still ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    const float cardH = gap ? fh + pad * 2.0f : (p ? row * 2.0f + fh : fh) + warnH + pad * 2.0f;
    const float cardR = m_laneX + m_laneW + 34.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(cardR, a.y + cardH), Col(EditorTheme::Role::SurfaceAlt), 6.0f);

    const float x0 = a.x + pad;
    const float infoR = a.x + m_infoW;   // правый край левой колонки
    ImGui::SetCursorScreenPos(ImVec2(x0, a.y + pad));
    ImGui::BeginDisabled(play);

    // Строка 1: значок, название, единицы … кривая, ⋯
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline(PropIcon(t.Property));
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    if (gap) {
        ImGui::TextDisabled("%s", T("Pause step"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A pause: what comes after it waits. Drag the bar to change how long."));
    } else if (!present) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", tweenui::TitleOf(t.Property).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("This object has no such property — it is skipped"));
    }

    // ⋯ — у правого края колонки; кривая — слева от него.
    const float dotsX = infoR - fh - pad;
    const std::string ease = gap ? std::string() : tweenui::EaseName(t.Curve);
    const float thumb = fh - 6.0f;
    const float arrow = ImGui::CalcTextSize("v").x;
    const float chipW = st.FramePadding.x * 2.0f + thumb + 6.0f + ImGui::CalcTextSize(ease.c_str()).x + 8.0f + arrow;
    if (!gap && present) {
        ImGui::TextUnformatted(tweenui::TitleOf(t.Property).c_str());
        // Единицы — только если влезают до кривой; не влезли — в подсказке.
        const char* unit = PropUnit(t.Property);
        const std::string unitText = *unit ? std::string("· ") + unit : std::string();
        const bool fits = *unit && ImGui::GetItemRectMax().x + st.ItemSpacing.x * 2.0f + ImGui::CalcTextSize(unitText.c_str()).x <
                                       dotsX - st.ItemSpacing.x - chipW;
        if (fits) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", unitText.c_str());
        } else if (*unit && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", unit);
        }
    }
    if (!gap) {
        const float chipX = std::max(dotsX - st.ItemSpacing.x - chipW, ImGui::GetItemRectMax().x + st.ItemSpacing.x);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(chipX, a.y + pad));
        if (ImGui::InvisibleButton("##ease", ImVec2(chipW, fh))) {
            m_popupTrack = (int)index;
            m_wantEase = true;
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", T("Curve: how the speed changes on the way from Start to End. Click to choose."));
        const ImVec2 c0 = ImGui::GetItemRectMin(), c1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(c0, c1, Col(hot ? EditorTheme::Role::Hover : EditorTheme::Role::Input), st.FrameRounding);
        float cx = c0.x + st.FramePadding.x;
        const float ty = c0.y + (fh - ImGui::GetTextLineHeight()) * 0.5f;
        PlotEase(dl, ImVec2(cx, c0.y + 3.0f), ImVec2(cx + thumb, c1.y - 3.0f), t.Curve, Col(EditorTheme::Role::Accent),
                 1.5f, thumb * 0.15f);
        cx += thumb + 6.0f;
        dl->AddText(ImVec2(cx, ty), Col(EditorTheme::Role::Text), ease.c_str());
        // Стрелка «раскрывается» — треугольником, а не буквой.
        const float ax = c1.x - st.FramePadding.x - arrow * 0.5f, ay = c0.y + fh * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - 4, ay - 2), ImVec2(ax + 4, ay - 2), ImVec2(ax, ay + 3),
                              Col(EditorTheme::Role::TextDim));
    }
    ImGui::SetCursorScreenPos(ImVec2(dotsX, a.y + pad));
    if (EditorIcons::IconOnlyButton("dots", T("More: order, remove"))) {
        m_popupTrack = (int)index;
        m_wantCardMenu = true;
    }

    // Строки 2–3: Начало и Конец, каждая со своей кнопкой справа.
    if (!gap && p) {
        const char* startLabel = T("Starts at");
        const char* endLabel = T("Ends at");
        const float labelW = std::max(ImGui::CalcTextSize(startLabel).x, ImGui::CalcTextSize(endLabel).x) + st.ItemSpacing.x;
        const float fieldX = x0 + labelW;
        const float fieldW = std::max(infoR - pad - fieldX - fh - st.ItemSpacing.x, 60.0f);
        auto same = [](const glm::vec4& v) { return v.x == v.y && v.y == v.z; };
        const bool uniform = scale && !m_splitAxes.count((int)index) && same(from) && same(t.To);

        // Начало
        const float y1 = a.y + pad + row;
        ImGui::SetCursorScreenPos(ImVec2(x0, y1));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(startLabel);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Where the property starts from"));
        ImGui::SetCursorScreenPos(ImVec2(fieldX, y1));
        // «Как сейчас» — приглушено: это не число, а «то, что будет у объекта
        // в момент старта». Тронул поле — стало своим числом.
        if (t.FromCurrent) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.Alpha * 0.55f);
        if (DrawValue("from", p, from, fieldW, uniform)) {
            StopPreview(host);
            t.From = from;
            t.FromCurrent = false;
        }
        if (t.FromCurrent) ImGui::PopStyleVar();
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", t.FromCurrent ? T("Start = the value the object has when the tween starts. Type a number to set your own.")
                                                  : T("Start value"));
        ImGui::SameLine();
        if (EditorIcons::IconOnlyButton("refresh", t.FromCurrent ? T("Starts from the current value (on). Click to use a fixed number instead")
                                                                 : T("Start from the object's current value"),
                                        t.FromCurrent)) {
            t.FromCurrent = !t.FromCurrent;
            if (!t.FromCurrent && present) t.From = cur;
            Edited(host);
        }

        // Конец
        const float y2 = y1 + row;
        ImGui::SetCursorScreenPos(ImVec2(x0, y2));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(endLabel);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Where the property ends up when the tween finishes"));
        ImGui::SetCursorScreenPos(ImVec2(fieldX, y2));
        if (DrawValue("to", p, t.To, fieldW, uniform)) StopPreview(host);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("End value"));
        ImGui::SameLine();
        ImGui::BeginDisabled(!present);
        if (EditorIcons::IconOnlyButton("eyedropper", T("Take the end from the object: move/rotate/paint the object in the scene, then click"))) {
            t.To = cur;
            Edited(host);
        }
        ImGui::EndDisabled();

        if (still) {
            ImGui::SetCursorScreenPos(ImVec2(fieldX, y2 + fh + st.ItemSpacing.y * 0.5f));
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("End = Start: nothing will change. Set the End value."));
        }
    }
    ImGui::EndDisabled();

    DrawLane(host, clip, index, a.y + pad, a.y + cardH - pad);
    m_cardsBottom = a.y + cardH;

    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + cardH + st.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}


// --- Полоса на шкале: ● начало — середина — конец ● --------------------------------------
void TweenPanel::DrawLane(EditorHost& host, anim::TweenClip& clip, size_t index, float y0, float y1) {
    anim::TweenTrack& t = clip.Tracks[index];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool gap = t.Property.empty();
    const float cy = (y0 + y1) * 0.5f;
    const float barH = std::min(y1 - y0, 22.0f);
    const float by0 = cy - barH * 0.5f, by1 = cy + barH * 0.5f;
    const float x0 = m_laneX + t.Start * m_pxPerSec;
    const float x1 = m_laneX + t.End() * m_pxPerSec;
    const float r = 6.0f;

    // Дорожка — тонкая линия во всю шкалу, чтобы было видно «где ноль».
    dl->AddLine(ImVec2(m_laneX, cy), ImVec2(m_laneX + m_laneW, cy), Col(EditorTheme::Role::Line), 1.0f);

    ImGui::BeginDisabled(host.InPlayMode());
    bool anyHot = false;
    auto handle = [&](const char* id, float hx, float hw, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(hx, by0));
        ImGui::InvisibleButton(id, ImVec2(std::max(hw, 1.0f), barH));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        anyHot |= hot;
        if (hot) ImGui::SetMouseCursor(mode == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s", mode == 0 ? T("Drag: when it starts") : mode == 1 ? T("Drag: start point")
                                                                                         : T("Drag: end point (how long it lasts)"));
        // Сдвиг копится между кадрами: шаг 0.05 с съел бы медленное движение
        // мыши целиком, и полоса не сдвинулась бы вовсе.
        static float carry = 0.0f;
        if (ImGui::IsItemActivated()) {
            StopPreview(host);
            carry = 0.0f;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Шаг 0.05 с: «ровно 0.3 секунды» ставится мышью, а не угадывается.
            // Shift — без шага.
            carry += ImGui::GetIO().MouseDelta.x / m_pxPerSec;
            auto snap = [](float v) { return ImGui::GetIO().KeyShift ? v : std::round(v / 0.05f) * 0.05f; };
            if (mode == 0) {
                const float s = snap(std::max(0.0f, t.Start + carry));
                carry -= s - t.Start;
                t.Start = s;
            } else if (mode == 1) {
                const float end = t.End();
                const float s = snap(std::clamp(t.Start + carry, 0.0f, end - 0.05f));
                carry -= s - t.Start;
                t.Start = s;
                t.Duration = end - s;
            } else {
                const float d = snap(std::max(0.05f, t.Duration + carry));
                carry -= d - t.Duration;
                t.Duration = std::max(d, 0.05f);
            }
        }
        if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    };
    handle("##body", x0 + r, x1 - x0 - r * 2.0f, 0);
    handle("##from", x0 - r, r * 2.0f, 1);
    handle("##to", x1 - r, r * 2.0f, 2);
    ImGui::EndDisabled();

    const ImU32 fill = Col(gap ? EditorTheme::Role::Input : EditorTheme::Role::AccentMuted);
    dl->AddRectFilled(ImVec2(x0, by0), ImVec2(x1, by1), fill, 4.0f);
    if (anyHot) dl->AddRect(ImVec2(x0, by0), ImVec2(x1, by1), Col(EditorTheme::Role::Accent), 4.0f);
    if (!gap && x1 - x0 > 2.0f * r + 8.0f)
        PlotEase(dl, ImVec2(x0 + r, by0 + 2.0f), ImVec2(x1 - r, by1 - 2.0f), t.Curve, Col(EditorTheme::Role::Accent), 1.0f,
                 barH * 0.12f);
    // Точки начала и конца — то, за что тянут.
    const ImU32 dot = Col(gap ? EditorTheme::Role::TextDim : EditorTheme::Role::Accent);
    dl->AddCircleFilled(ImVec2(x0, cy), r - 1.0f, dot);
    dl->AddCircleFilled(ImVec2(x1, cy), r - 1.0f, dot);
    char dur[24];
    std::snprintf(dur, sizeof(dur), "%.2f %s", t.Duration, T("s"));
    const ImVec2 ts = ImGui::CalcTextSize(dur);
    dl->AddText(ImVec2(x1 + r + 3.0f, cy - ts.y * 0.5f), Col(EditorTheme::Role::TextDim), dur);
}


// --- Дополнительно: всё, что нужно не каждому твину --------------------------------------
void TweenPanel::DrawAdvanced(EditorHost& host, anim::TweenComponent& tc, anim::TweenClip& clip) {
    ImGui::Spacing();
    // Свёрнуто по умолчанию — и одной строкой, без рамки раздела.
    if (!m_advanced) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const bool toggled =
        EditorIcons::Button("gear", T("Advanced"), T("Order, delay, repeat, speed, what happens next"), m_advanced);
    if (!m_advanced) ImGui::PopStyleColor();
    if (toggled) m_advanced = !m_advanced;
    if (!m_advanced) return;

    const bool play = host.InPlayMode();
    ImGui::BeginDisabled(play);
    Sage::UI::BeginProperties("tweenadv");

    Sage::UI::PropertyLabel(T("Order"), T("How the properties follow each other on the timeline"));
    if (EditorIcons::Button("step", T("One after another"), T("Each property starts when the previous one ends"))) {
        clip.ArrangeSequence();
        Edited(host);
    }
    ImGui::SameLine();
    if (EditorIcons::Button("layout", T("All together"), T("All properties start at the same time"))) {
        clip.ArrangeParallel();
        Edited(host);
    }
    ImGui::SameLine();
    if (EditorIcons::Button("clock", T("Pause step"), T("A pause: what comes after it waits"))) {
        anim::TweenTrack gap;
        gap.Duration = 0.25f;
        clip.AppendAfter(gap);
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("Delay"), T("Wait before the tween starts"));
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::DragFloat("##delay", &clip.Delay, 0.01f, 0.0f, 600.0f, "%.2f s")) StopPreview(host);
    host.TrackLastImGuiItem();

    Sage::UI::PropertyLabel(T("Repeat mode"));
    static const char* kLoops[] = {"Once", "Round and round", "Ping-pong"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(T(kLoops[i]), (int)clip.Loop == i) && (int)clip.Loop != i) {
            clip.Loop = (anim::TweenLoop)i;
            Edited(host);
        }
    }

    Sage::UI::PropertyLabel(T("Speed"));
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::DragFloat("##speed", &clip.Speed, 0.01f, 0.01f, 20.0f, "x%.2f")) StopPreview(host);
    host.TrackLastImGuiItem();

    Sage::UI::PropertyLabel(T("Direction"));
    if (ImGui::RadioButton(T("Forward"), !clip.Reverse) && clip.Reverse) {
        clip.Reverse = false;
        Edited(host);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T("Backward"), clip.Reverse) && !clip.Reverse) {
        clip.Reverse = true;
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("When finished"), T("Start another tween of this object right after this one"));
    ImGui::SetNextItemWidth(220.0f);
    const std::string thenLabel = clip.Then.empty() ? std::string(T("Nothing")) : std::string(T("Play")) + " " + clip.Then;
    if (ImGui::BeginCombo("##then", thenLabel.c_str())) {
        if (ImGui::Selectable(T("Nothing"), clip.Then.empty()) && !clip.Then.empty()) {
            clip.Then.clear();
            Edited(host);
        }
        for (size_t i = 0; i < tc.Tweens.size(); ++i) {
            const std::string& name = tc.Tweens[i].Name;
            ImGui::PushID((int)i);
            if (ImGui::Selectable((std::string(T("Play")) + " " + name).c_str(), clip.Then == name) && clip.Then != name) {
                clip.Then = name;
                Edited(host);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    Sage::UI::PropertyLabel(T("Start"), T("Starts by itself when the game starts"));
    if (ImGui::Checkbox(T("By itself when the game starts"), &clip.PlayOnStart)) Edited(host);

    Sage::UI::PropertyLabel(T("Animates"), T("The object whose properties change; empty — this object"));
    objectslot::Options opt;
    opt.EmptyLabel = "This object";
    opt.SelfId = m_ownerId;
    const objectslot::Result r = objectslot::Draw(host, "target", clip.Target, opt);
    if (r.Changed) {
        clip.Target = r.Id == m_ownerId ? 0 : r.Id;
        Edited(host);
    }

    Sage::UI::PropertyLabel(T("Name"), T("By this name a script plays it: Tween.play(obj, \"Name\")"));
    char name[64];
    std::snprintf(name, sizeof(name), "%s", clip.Name.c_str());
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##name", name, sizeof(name));
    if (ImGui::IsItemDeactivatedAfterEdit() && name[0] && clip.Name != name) {
        // Ссылки «когда закончится — запустить» идут за именем.
        for (anim::TweenClip& other : tc.Tweens)
            if (other.Then == clip.Name) other.Then = name;
        clip.Name = name;
        Edited(host);
    }

    Sage::UI::EndProperties();
    ImGui::EndDisabled();
}
