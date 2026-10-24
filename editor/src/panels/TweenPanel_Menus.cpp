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



// Всплывающие окна твинов: свойство, плавность, кривая, меню карточки.
// =============================================================================
//  Всплывающие окна
// =============================================================================

void TweenPanel::DrawPopups(EditorHost& host, anim::TweenClip* clip, entt::entity target) {
    if (m_wantProps) {
        ImGui::OpenPopup("##tweenprops");
        m_wantProps = false;
    }
    DrawPropertyMenu(host, target);
    if (!clip) return;
    m_popupTrack = std::clamp(m_popupTrack, 0, std::max(0, (int)clip->Tracks.size() - 1));
    if (m_wantEase) {
        ImGui::OpenPopup("##tweenease");
        m_wantEase = false;
    }
    DrawEasePicker(host, *clip);
    if (m_wantCurve) {
        ImGui::OpenPopup("##tweencurve");
        m_wantCurve = false;
    }
    DrawCurveEditor(host, *clip);
    if (m_wantCardMenu) {
        ImGui::OpenPopup("##tweencard");
        m_wantCardMenu = false;
    }
    DrawCardMenu(host, *clip, target);
}


// Меню свойств: разделы, только то, что у объекта есть; редкое — под «Ещё».
void TweenPanel::DrawPropertyMenu(EditorHost& host, entt::entity target) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweenprops")) return;
    const std::vector<tweenui::Choice> choices = tweenui::Choices(host.CurrentScene().Registry(), target);
    std::string picked;
    const char* section = nullptr;
    bool first = true;
    for (const tweenui::Choice& c : choices) {
        if (!*c.Section) continue;
        if (!section || std::strcmp(section, c.Section) != 0) {
            section = c.Section;
            Sage::UI::MenuSection(T(section), first);
            first = false;
        }
        ImGui::PushID(c.Property->Id.c_str());
        if (EditorIcons::MenuItem(PropIcon(c.Property->Id), c.Title.c_str())) picked = c.Property->Id;
        ImGui::PopID();
    }
    const bool more = std::any_of(choices.begin(), choices.end(), [](const tweenui::Choice& c) { return !*c.Section; });
    if (more) {
        if (!first) ImGui::Separator();
        if (EditorIcons::BeginMenu("dots", T("More"))) {
            for (const tweenui::Choice& c : choices) {
                if (*c.Section) continue;
                ImGui::PushID(c.Property->Id.c_str());
                if (EditorIcons::MenuItem(PropIcon(c.Property->Id), c.Title.c_str())) picked = c.Property->Id;
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
    }
    if (choices.empty()) ImGui::TextDisabled("%s", T("This object has nothing to animate"));
    if (!picked.empty()) AddProperty(host, picked);
    ImGui::EndPopup();
}


// Кривые плитками: картинка говорит больше названия; название — при наведении.
void TweenPanel::DrawEasePicker(EditorHost& host, anim::TweenClip& clip) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweenease")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else EasePickerBody(host, clip);
    ImGui::EndPopup();
}


void TweenPanel::EasePickerBody(EditorHost& host, anim::TweenClip& clip) {
    anim::TweenTrack& t = clip.Tracks[(size_t)m_popupTrack];
    ImGui::TextUnformatted(tweenui::EaseName(t.Curve).c_str());
    const ImVec2 tile(64.0f, 46.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int cols = 4;
    int n = 0;
    for (const Preset& pr : kPresets) {
        const anim::Ease e = PresetEase(pr);
        if (n % cols) ImGui::SameLine();
        ImGui::PushID(n++);
        const bool on = t.Curve == e;
        if (ImGui::InvisibleButton("##tile", tile) && !on) {
            t.Curve = e;
            Edited(host);
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetTooltip("%s", T(pr.Name));
        const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        dl->AddRectFilled(p0, p1, Col(on ? EditorTheme::Role::AccentMuted : hot ? EditorTheme::Role::Hover
                                                                              : EditorTheme::Role::Input), 5.0f);
        if (on) dl->AddRect(p0, p1, Col(EditorTheme::Role::Accent), 5.0f, 0, 1.5f);
        PlotEase(dl, ImVec2(p0.x + 8, p0.y + 4), ImVec2(p1.x - 8, p1.y - 4), e,
                 Col(on ? EditorTheme::Role::Accent : EditorTheme::Role::Text), 1.5f, tile.y * 0.2f);
        ImGui::PopID();
    }
    ImGui::Separator();
    if (EditorIcons::BeginMenu("chart", T("Professional"))) {
        for (int s = (int)anim::EaseShape::Quad; s < (int)anim::EaseShape::Curve; ++s) {
            if (!ImGui::BeginMenu(kShapeNames[s])) continue;
            for (int m = 0; m < (int)anim::EaseMode::Count; ++m) {
                const anim::Ease e = anim::Ease::Make((anim::EaseShape)s, (anim::EaseMode)m);
                if (ImGui::MenuItem(ModeLabel((anim::EaseMode)m), nullptr, t.Curve == e) && t.Curve != e) {
                    t.Curve = e;
                    Edited(host);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (EditorIcons::MenuItem("pencil", T("Edit curve..."))) m_wantCurve = true;
}


// Своя кривая — по просьбе. Две ручки: тянешь — меняешь форму.
void TweenPanel::DrawCurveEditor(EditorHost& host, anim::TweenClip& clip) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweencurve")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else CurveEditorBody(host, clip);
    ImGui::EndPopup();
}


void TweenPanel::CurveEditorBody(EditorHost& host, anim::TweenClip& clip) {
    anim::TweenTrack& t = clip.Tracks[(size_t)m_popupTrack];
    ImGui::TextUnformatted(tweenui::EaseName(t.Curve).c_str());
    ImGui::TextDisabled("%s", T("Drag the two points to shape the curve"));

    const float side = 220.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + side, a.y + side);
    const float pad = side * 0.2f;   // y ∈ [-0.25, 1.25] — место на перелёт
    const ImVec2 g0(a.x + 8.0f, a.y + pad), g1(b.x - 8.0f, b.y - pad);
    auto toScreen = [&](float x, float y) { return ImVec2(g0.x + x * (g1.x - g0.x), g1.y - y * (g1.y - g0.y)); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, Col(EditorTheme::Role::Input), 5.0f);
    const ImU32 grid = Col(EditorTheme::Role::Line);
    dl->AddLine(toScreen(0, 0), toScreen(1, 0), grid);
    dl->AddLine(toScreen(0, 1), toScreen(1, 1), grid);
    dl->AddLine(toScreen(0, 0), toScreen(1, 1), grid);   // прямая — для сравнения
    PlotEase(dl, ImVec2(g0.x, g0.y), ImVec2(g1.x, g1.y), t.Curve, Col(EditorTheme::Role::Accent), 2.0f);

    // Ручки есть всегда: у готовой кривой они стоят там, где её повторяет
    // своя, и первое же движение превращает её в свою.
    const glm::vec4 c = t.Curve.Shape == anim::EaseShape::Curve ? t.Curve.Bezier : anim::BezierFor(t.Curve);
    const ImU32 handleCol = Col(EditorTheme::Role::Warn);
    for (int k = 0; k < 2; ++k) {
        const float hx = k == 0 ? c.x : c.z;
        const float hy = k == 0 ? c.y : c.w;
        const ImVec2 anchor = k == 0 ? toScreen(0, 0) : toScreen(1, 1);
        const ImVec2 p = toScreen(hx, hy);
        dl->AddLine(anchor, p, handleCol);
        ImGui::SetCursorScreenPos(ImVec2(p.x - 7, p.y - 7));
        ImGui::PushID(k);
        ImGui::InvisibleButton("##h", ImVec2(14, 14));
        ImGui::PopID();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        dl->AddCircleFilled(p, hot ? 6.0f : 4.5f, handleCol);
        if (ImGui::IsItemActivated()) {
            StopPreview(host);
            if (t.Curve.Shape != anim::EaseShape::Curve) {
                t.Curve.Bezier = c;
                t.Curve.Shape = anim::EaseShape::Curve;
            }
        }
        if (ImGui::IsItemActive()) {
            // x держится в [0, 1] (время не идёт назад), y свободен — так
            // кривая перелетает цель.
            const ImVec2 m = ImGui::GetIO().MousePos;
            float& ox = k == 0 ? t.Curve.Bezier.x : t.Curve.Bezier.z;
            float& oy = k == 0 ? t.Curve.Bezier.y : t.Curve.Bezier.w;
            ox = std::clamp((m.x - g0.x) / (g1.x - g0.x), 0.0f, 1.0f);
            oy = std::clamp((g1.y - m.y) / (g1.y - g0.y), -1.0f, 2.0f);
        }
        if (ImGui::IsItemDeactivated()) host.PushUndoSnapshot();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 4.0f));
    ImGui::Dummy(ImVec2(side, 0.0f));
}


// ⋯ карточки: редкое про одно свойство.
void TweenPanel::DrawCardMenu(EditorHost& host, anim::TweenClip& clip, entt::entity target) {
    Sage::UI::MenuScope menu;
    if (!ImGui::BeginPopup("##tweencard")) return;
    if (clip.Tracks.empty()) ImGui::CloseCurrentPopup();
    else CardMenuBody(host, clip, target);
    ImGui::EndPopup();
}


void TweenPanel::CardMenuBody(EditorHost& host, anim::TweenClip& clip, entt::entity target) {
    const int i = m_popupTrack;
    anim::TweenTrack& t = clip.Tracks[(size_t)i];
    const anim::PropertyType* p = anim::FindProperty(t.Property);
    entt::registry& reg = host.CurrentScene().Registry();
    const bool has = p && target != entt::null && anim::HasProperty(*p, reg, target);
    if (p) {
        if (EditorIcons::MenuItemSelected("refresh", T("Start from the current value"), t.FromCurrent)) {
            if (!t.FromCurrent) {
                t.FromCurrent = true;
            } else {
                t.FromCurrent = false;
                if (has) t.From = Current(reg, target, *p);
            }
            Edited(host);
        }
        if (EditorIcons::MenuItem("eyedropper", T("Set the end to the current value"), nullptr, has)) {
            t.To = Current(reg, target, *p);
            Edited(host);
        }
        if (t.Property == "object.scale") {
            const bool split = m_splitAxes.count(i) != 0;
            if (EditorIcons::MenuItemSelected("scale", T("Each axis separately"), split)) {
                if (split) m_splitAxes.erase(i);
                else m_splitAxes.insert(i);
            }
        }
        ImGui::Separator();
    }
    if (EditorIcons::MenuItem("up", T("Move up"), nullptr, i > 0)) {
        std::swap(clip.Tracks[(size_t)i], clip.Tracks[(size_t)i - 1]);
        m_splitAxes.clear();
        Edited(host);
    }
    if (EditorIcons::MenuItem("align-bottom", T("Move down"), nullptr, i + 1 < (int)clip.Tracks.size())) {
        std::swap(clip.Tracks[(size_t)i], clip.Tracks[(size_t)i + 1]);
        m_splitAxes.clear();
        Edited(host);
    }
    if (EditorIcons::MenuItem("trash", T("Remove"))) {
        StopPreview(host);
        clip.Tracks.erase(clip.Tracks.begin() + i);
        m_splitAxes.clear();
        host.PushUndoSnapshot();
    }
}
