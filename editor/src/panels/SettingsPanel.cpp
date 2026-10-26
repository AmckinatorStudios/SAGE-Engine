#include "SettingsPanel.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Project.h"
#include "panels/settings/SettingsUi.h"
#include "sage/app/Application.h"
#include "../Localization.h"
#include "../ui/UI.h"

namespace ui = sage::editor::settingsui;

namespace {
struct CategoryInfo {
    const char* Icon;
    const char* Name;
    const char* About;
};
// Названия — через T() в месте использования (каталог перевода видит литералы).
CategoryInfo Info(SettingsPanel::Category c) {
    switch (c) {
        case SettingsPanel::Category::Screen:
            return {"window", T("Screen"), T("Window mode, monitor, resolution and frame rate of the game.")};
        case SettingsPanel::Category::Quality:
            return {"chart", T("Graphics Quality"), T("Quality profile, render resolution and anti-aliasing.")};
        case SettingsPanel::Category::Lighting:
            return {"sun", T("Lighting and shadows"),
                    T("Shadows of the sun and of lamps, bounced light and reflections.")};
        case SettingsPanel::Category::Controls:
            return {"keyboard", T("Game controls"), T("Game actions and the keys, buttons and sticks they are bound to.")};
        case SettingsPanel::Category::Advanced:
            return {"gear", T("Advanced"), T("Graphics API of the game and information about this machine.")};
        case SettingsPanel::Category::General:
            return {"language", T("General"),
                    T("Language of the editor and the program that opens code. Stored for you, not in the project.")};
        case SettingsPanel::Category::Appearance:
            return {"theme", T("Appearance"),
                    T("Theme and scale of the editor. Stored for you, not in the project, and applied at once.")};
        default: return {"gear", "", ""};
    }
}

} // namespace

const std::vector<sage::display::Monitor>& SettingsPanel::Monitors() {
    // Список мониторов — раз в две секунды, а не каждый кадр: опрос идёт к
    // системе, а монитор подключают не чаще, чем человек смотрит на список.
    const double now = glfwGetTime();
    if (now - m_monitorsAt > 2.0) {
        m_monitors = sage::display::QueryMonitors();
        m_monitorsAt = now;
    }
    return m_monitors;
}

void SettingsPanel::SyncSaved(EditorHost& host) {
    // Снимок «как в проекте» — при открытии проекта. Всё, что отличается от
    // него, — несохранённое.
    const std::string key = host.CurrentProject().Dir().string();
    if (key == m_savedFor) return;
    m_savedFor = key;
    m_saved = host.Settings();
}

bool SettingsPanel::HasUnsavedChanges(const EditorHost& host) const {
    return const_cast<EditorHost&>(host).Settings().ToJsonString() != m_saved.ToJsonString();
}

bool SettingsPanel::SaveToProject(EditorHost& host) {
    const std::string path = (host.CurrentProject().Dir() / "sage.cfg").string();
    if (!host.Settings().SaveFile(path)) {
        host.SetStatusMessage(T("Could not save sage.cfg"));
        return false;
    }
    m_saved = host.Settings();
    host.SetStatusMessage(T("Settings saved: sage.cfg"));
    return true;
}

void SettingsPanel::RevertUnsaved(EditorHost& host) {
    host.Settings() = m_saved;
    host.SetStatusMessage(T("Unsaved settings reverted"));
}

void SettingsPanel::SetSearch(const char* text) {
    std::snprintf(m_search, sizeof(m_search), "%s", text ? text : "");
}

bool SettingsPanel::SelectByKey(const char* key) {
    static const char* const kKeys[] = {"screen",   "quality", "lighting",  "controls",
                                        "advanced", "general", "appearance"};
    static_assert(sizeof(kKeys) / sizeof(kKeys[0]) == (size_t)Category::Count, "ключ на каждый раздел");
    for (int i = 0; i < (int)Category::Count; ++i) {
        if (key && std::strcmp(key, kKeys[i]) == 0) {
            m_cat = (Category)i;
            return true;
        }
    }
    return false;
}

void SettingsPanel::Draw(EditorHost& host, bool& open) {
    // Окно проверки режима живёт и при закрытом окне настроек: экран уже
    // переключён, и спросить «оставить?» обязаны в любом случае.
    if (m_trial.Active()) {
        if (m_trial.Tick(sage::Application::Get().GetWindow(), glfwGetTime()))
            host.SetStatusMessage(T("Display mode reverted: no confirmation"));
        DrawTrialPopup(host);
    }
    if (!open) return;
    SyncSaved(host);

    ImGui::SetNextWindowSize(ImVec2(980, 660), ImGuiCond_FirstUseEver);
    if (m_forceW > 0.0f) {
        ImGui::SetNextWindowSize(ImVec2(m_forceW, m_forceH), ImGuiCond_Always);
        m_forceW = 0.0f;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 420), ImVec2(FLT_MAX, FLT_MAX));
    // Скрытый идентификатор прежний: раскладка доков помнит окно по нему.
    if (!ImGui::Begin(EditorIcons::WindowTitle("gear", T("Settings"), "Game Settings").c_str(), &open)) {
        ImGui::End();
        return;
    }
    const Sage::UI::Style& st = Sage::UI::Get();
    const float footerH = ImGui::GetFrameHeight() + st.SpacingLG * 1.5f;
    const float avail = ImGui::GetContentRegionAvail().x;
    // Узкое окно — разделы колонкой иконок, параметры в одну колонку.
    const bool compactNav = avail < 720.0f;
    const float navW = compactNav ? ImGui::GetFrameHeight() + st.SpacingSM * 4.0f : std::clamp(avail * 0.22f, 190.0f, 240.0f);

    // --- Разделы ------------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(EditorTheme::Role::SurfaceAlt));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(st.SpacingSM, st.SpacingSM));
    ImGui::BeginChild("##nav", ImVec2(navW, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (!compactNav) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        Sage::UI::SearchField("##search", m_search, sizeof(m_search), T("Search settings"));
        ImGui::Dummy(ImVec2(0.0f, st.SpacingXS));
    }
    for (int i = 0; i < (int)Category::Count; ++i) {
        const Category c = (Category)i;
        const CategoryInfo info = Info(c);
        const bool sel = m_cat == c && m_search[0] == 0;
        // Заголовки «Игра» и «Редактор»: первое уезжает в сборку, второе —
        // только у этого человека, и путать их нельзя. В узкой колонке
        // значков подписи не влезают — там их место занимает черта.
        if (!compactNav && (i == 0 || c == Category::General)) {
            if (i) ImGui::Dummy(ImVec2(0.0f, st.SpacingSM));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + st.SpacingSM);
            Sage::UI::TextSecondary("%s", i == 0 ? T("Game") : T("Editor"));
        } else if (compactNav && c == Category::General) {
            ImGui::Dummy(ImVec2(0.0f, st.SpacingXS));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, st.SpacingXS));
        }
        ImGui::PushID(i);
        // Строка раздела рисуется сама, а не Selectable: у Selectable фон
        // вылезает за отступ окна, а полоса акцента, нарисованная левее него,
        // упиралась в край колонки — выбранный пункт выглядел съехавшим.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetFrameHeight() + st.SpacingSM;
        if (ImGui::InvisibleButton("##cat", ImVec2(w, h))) {
            m_cat = c;
            m_search[0] = 0;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (compactNav && hovered) ImGui::SetTooltip("%s", info.Name);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q(p.x + w, p.y + h);
        if (sel || hovered) {
            ImVec4 bg = EditorTheme::Color(sel ? EditorTheme::Role::Accent : EditorTheme::Role::Elevated);
            bg.w = sel ? 0.16f : 0.9f;
            dl->AddRectFilled(p, q, ImGui::ColorConvertFloat4ToU32(bg), 5.0f);
        }
        // Выбранный раздел — полоса акцента у левого края строки, внутри неё.
        if (sel)
            dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + 3.0f, q.y - h * 0.22f),
                              EditorTheme::Color32(EditorTheme::Role::Accent), 1.5f);
        const ImU32 col = EditorTheme::Color32(sel ? EditorTheme::Role::Text : EditorTheme::Role::TextDim);
        const float icon = std::floor(ImGui::GetFontSize() * 1.15f);
        const float iy = p.y + (h - icon) * 0.5f;
        if (compactNav)
            EditorIcons::DrawLabeled(dl, ImVec2(p.x + (w - icon) * 0.5f, iy), icon, info.Icon, col, "", col);
        else
            EditorIcons::DrawLabeled(dl, ImVec2(p.x + st.SpacingSM + 4.0f, iy), icon, info.Icon, col, info.Name, col);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, st.SpacingSM);

    // --- Раздел -------------------------------------------------------------
    ImGui::BeginChild("##page", ImVec2(0.0f, -footerH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ui::Ctx ctx;
    ctx.Filter = ui::Lower(m_search);
    ctx.Narrow = ImGui::GetContentRegionAvail().x < 640.0f;
    // В узком окне поиску нет места в колонке значков — он над разделом.
    if (compactNav) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        Sage::UI::SearchField("##search", m_search, sizeof(m_search), T("Search settings"));
        ImGui::Dummy(ImVec2(0.0f, st.SpacingXS));
    }
    if (ctx.Filter.empty()) {
        const CategoryInfo info = Info(m_cat);
        const float big = std::floor(ImGui::GetFontSize() * 1.6f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        EditorIcons::Overlay(ImGui::GetWindowDrawList(), p.x, p.y + 2.0f, big, info.Icon);
        ImGui::Dummy(ImVec2(big + st.SpacingMD, big));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(info.Name);
        ImGui::PopFont();
        // Описание переносится: в узком окне оно обрезалось на полуслове.
        ImGui::PushTextWrapPos(0.0f);
        Sage::UI::TextSecondary("%s", info.About);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0.0f, st.SpacingMD));
        if (m_cat == Category::Controls) m_controls.DrawBody(host);
        else DrawCategory(host, m_cat, ctx);
    } else {
        // Поиск — по всем разделам сразу, с названием раздела над находками.
        for (int i = 0; i < (int)Category::Count; ++i) {
            // Управление — свой редактор со своим поиском, не строки.
            if ((Category)i == Category::Controls) continue;
            const int before = ctx.Shown;
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorPos();
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()));
            DrawCategory(host, (Category)i, ctx);
            if (ctx.Shown > before) {
                const ImVec2 end = ImGui::GetCursorPos();
                ImGui::SetCursorPos(at);
                Sage::UI::TextSecondary("%s", Info((Category)i).Name);
                ImGui::SetCursorPos(end);
            } else {
                ImGui::SetCursorPos(at);
            }
            ImGui::PopID();
        }
        if (ctx.Shown == 0) Sage::UI::EmptyState(T("Nothing found"), T("Try another word: shadows, fog, resolution…"));
    }
    ImGui::EndChild();

    DrawFooter(host);
    ImGui::End();
    if (m_openTrialPopup) {
        ImGui::OpenPopup(T("Keep this display mode?###displaytrial"));
        m_openTrialPopup = false;
    }
}

void SettingsPanel::DrawCategory(EditorHost& host, Category c, ui::Ctx& ctx) {
    switch (c) {
        case Category::Screen: DrawScreen(host, ctx); break;
        case Category::Quality: DrawQuality(host, ctx); break;
        case Category::Lighting: DrawLighting(host, ctx); break;
        case Category::Advanced: DrawAdvanced(host, ctx); break;
        case Category::General: DrawGeneral(host, ctx); break;
        case Category::Appearance: DrawAppearance(host, ctx); break;
        default: break;
    }
}

void SettingsPanel::DrawFooter(EditorHost& host) {
    const Sage::UI::Style& st = Sage::UI::Get();
    ImGui::Dummy(ImVec2(0.0f, st.SpacingSM));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, st.SpacingXS));
    // В поиске низ — про sage.cfg: находки только из разделов игры.
    const Category cat = m_search[0] ? Category::Screen : m_cat;
    if (IsEditorCategory(cat)) {
        ImGui::AlignTextToFramePadding();
        Sage::UI::TextSecondary("%s", T("Applied at once and stored in your editor profile"));
        return;
    }
    const bool controls = cat == Category::Controls;
    if (!controls) {
        if (EditorIcons::Button("refresh", T("Defaults"), T("Engine defaults for this section")))
            ResetCategory(host, cat);
        ImGui::SameLine();
    }
    const bool dirty = controls ? host.ProjectInputDirty() : HasUnsavedChanges(host);
    ImGui::AlignTextToFramePadding();
    if (dirty && controls) Sage::UI::TextSecondary("%s", T("Unsaved changes in the controls"));
    else if (dirty) Sage::UI::TextSecondary("%s", T("Unsaved changes — the scene view already shows them"));
    else Sage::UI::TextSecondary("%s", T("Saved in the project"));
    // Справа: Отмена, Применить.
    const float bw = 110.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - bw * 2.0f - st.SpacingSM));
    ImGui::BeginDisabled(!dirty);
    if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(bw, 0))) {
        if (!controls) RevertUnsaved(host);
        else if (host.ReloadProjectInput()) host.SetStatusMessage(T("Controls reloaded from disk"));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", T("Return to what is saved in the project"));
    ImGui::SameLine(0.0f, st.SpacingSM);
    if (Sage::UI::Button(T("Apply"), Sage::UI::ButtonStyle::Primary, ImVec2(bw, 0))) {
        if (!controls) SaveToProject(host);
        else if (host.SaveProjectInput()) host.SetStatusMessage(T("Controls saved: input.sageinput"));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", controls ? T("Save to the project (input.sageinput)")
                                         : T("Save to the project (sage.cfg): the built game starts with these settings"));
    ImGui::EndDisabled();
}

void SettingsPanel::DrawTrialPopup(EditorHost& host) {
    if (m_openTrialPopup) {
        ImGui::OpenPopup(T("Keep this display mode?###displaytrial"));
        m_openTrialPopup = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(T("Keep this display mode?###displaytrial"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    if (!m_trial.Active()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const int left = (int)std::ceil(m_trial.SecondsLeft(glfwGetTime()));
    ImGui::Text(T("The previous mode comes back in %d s if you do not confirm."), left);
    Sage::UI::TextSecondary("%s", T("If the picture is broken, just wait — or press Esc."));
    ImGui::Dummy(ImVec2(0.0f, Sage::UI::Get().SpacingSM));
    if (Sage::UI::Button(T("Keep"), Sage::UI::ButtonStyle::Primary, ImVec2(120, 0))) {
        m_trial.Confirm();
        host.SetStatusMessage(T("Display mode kept"));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Sage::UI::Button(T("Revert"), Sage::UI::ButtonStyle::Secondary, ImVec2(120, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_trial.Revert(sage::Application::Get().GetWindow());
        host.SetStatusMessage(T("Display mode reverted"));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
