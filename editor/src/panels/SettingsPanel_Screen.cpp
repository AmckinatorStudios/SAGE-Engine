// ---------------------------------------------------------------------------
// «Настройки игры» → Экран: режим окна, монитор, разрешение, частота, кадры.
//
// Параметры экрана принадлежат ОКНУ ИГРЫ и применяются при её запуске (окно
// создаётся с ними). Чтобы не гадать, заработает ли выбранный режим на этом
// мониторе, его можно проверить прямо здесь — на окне редактора, с откатом.
// ---------------------------------------------------------------------------
#include "SettingsPanel.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "EditorHost.h"
#include "EditorTheme.h"
#include "panels/settings/SettingsUi.h"
#include "sage/app/Application.h"
#include "../Localization.h"
#include "../ui/UI.h"

namespace ui = sage::editor::settingsui;

namespace {
const sage::EngineConfig& Def() {
    static const sage::EngineConfig d;
    return d;
}

std::string ResLabel(int w, int h) {
    char b[64];
    std::snprintf(b, sizeof(b), "%d × %d  (%s)", w, h, sage::display::AspectName(w, h).c_str());
    return b;
}
} // namespace

void SettingsPanel::DrawScreen(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    const auto& monitors = Monitors();
    // Монитор, о котором идёт речь: выбранный или главный.
    const sage::display::Monitor* mon = nullptr;
    if (c.Monitor >= 0 && c.Monitor < (int)monitors.size()) mon = &monitors[(size_t)c.Monitor];
    if (!mon || !mon->ModeKnown)
        for (const auto& m : monitors)
            if (m.Primary && m.ModeKnown) mon = &m;
    if (!mon)
        for (const auto& m : monitors)
            if (m.ModeKnown) mon = &m;

    ui::BeginGroup(ctx, T("Display mode"), nullptr, ui::Apply::Launch);
    {
        ui::RowOpts o;
        o.Keywords = "window mode fullscreen borderless окно полноэкранный безрамочный";
        o.When = ui::Apply::Launch;
        o.Modified = c.Mode != Def().Mode;
        o.Tooltip = T("Windowed — a normal window; Borderless — a window covering the monitor (instant switching); "
                      "Fullscreen — the monitor switches to the chosen resolution");
        bool reset = false;
        if (ui::Row(ctx, T("Window mode"), o, &reset)) {
            const char* modes[] = {T("Windowed"), T("Borderless fullscreen"), T("Fullscreen")};
            int m = (int)c.Mode;
            if (ImGui::Combo("##mode", &m, modes, IM_ARRAYSIZE(modes))) c.Mode = (sage::WindowMode)m;
            if (reset) c.Mode = Def().Mode;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "monitor display монитор экран";
        o.When = ui::Apply::Launch;
        o.Modified = c.Monitor != Def().Monitor;
        o.Tooltip = T("Which monitor the game opens on. A monitor that does not report its mode cannot be chosen.");
        bool reset = false;
        if (ui::Row(ctx, T("Monitor"), o, &reset)) {
            auto label = [&](int idx) -> std::string {
                if (idx < 0) return T("Primary (automatic)");
                if (idx >= (int)monitors.size()) return T("Disconnected monitor — the primary is used");
                const auto& m = monitors[(size_t)idx];
                char b[160];
                std::snprintf(b, sizeof(b), "%d. %s%s", idx + 1, m.Name.c_str(),
                              m.Primary ? T(" (primary)") : "");
                return b;
            };
            if (ImGui::BeginCombo("##monitor", label(c.Monitor).c_str())) {
                if (ImGui::Selectable(label(-1).c_str(), c.Monitor < 0)) c.Monitor = -1;
                for (const auto& m : monitors) {
                    std::string l = label(m.Index);
                    if (m.ModeKnown) {
                        char extra[64];
                        std::snprintf(extra, sizeof(extra), "  —  %d × %d, %d%%", m.Current.Width, m.Current.Height,
                                      (int)std::lround(m.Scale * 100.0f));
                        l += extra;
                    } else {
                        l += T("  —  does not respond");
                    }
                    ImGui::BeginDisabled(!m.ModeKnown);
                    if (ImGui::Selectable(l.c_str(), c.Monitor == m.Index)) c.Monitor = m.Index;
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (reset) c.Monitor = Def().Monitor;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "resolution size width height разрешение размер ширина высота";
        o.When = ui::Apply::Launch;
        o.Modified = c.Width != Def().Width || c.Height != Def().Height;
        const bool borderless = c.Mode == sage::WindowMode::Borderless;
        o.Disabled = borderless;
        o.DisabledWhy = T("A borderless window always takes the size of its monitor");
        bool reset = false;
        if (ui::Row(ctx, T("Resolution"), o, &reset)) {
            if (borderless && mon) {
                std::string l = std::string(T("Monitor size: ")) + ResLabel(mon->Current.Width, mon->Current.Height);
                ImGui::InputText("##res", l.data(), l.size() + 1, ImGuiInputTextFlags_ReadOnly);
            } else {
                // Список — из режимов монитора (полный экран умеет только их);
                // окну можно и своё — полями ниже.
                std::vector<std::pair<int, int>> sizes;
                if (mon)
                    for (const auto& vm : mon->Modes) {
                        const std::pair<int, int> s{vm.Width, vm.Height};
                        if (std::find(sizes.begin(), sizes.end(), s) == sizes.end()) sizes.push_back(s);
                    }
                const bool known = std::find(sizes.begin(), sizes.end(), std::make_pair(c.Width, c.Height)) != sizes.end();
                const std::string cur = known ? ResLabel(c.Width, c.Height)
                                              : ResLabel(c.Width, c.Height) + T("  — custom");
                if (ImGui::BeginCombo("##res", cur.c_str())) {
                    for (const auto& s : sizes)
                        if (ImGui::Selectable(ResLabel(s.first, s.second).c_str(), s.first == c.Width && s.second == c.Height)) {
                            c.Width = s.first;
                            c.Height = s.second;
                        }
                    ImGui::EndCombo();
                }
                if (c.Mode == sage::WindowMode::Windowed) {
                    int wh[2] = {c.Width, c.Height};
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputInt2("##custom", wh)) {
                        c.Width = std::clamp(wh[0], 320, 16384);
                        c.Height = std::clamp(wh[1], 240, 16384);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Your own window size, in pixels"));
                } else if (!known && mon) {
                    ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                       T("The monitor does not support this resolution in fullscreen — pick one from the list"));
                }
            }
            if (reset) {
                c.Width = Def().Width;
                c.Height = Def().Height;
            }
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "aspect ratio letterbox соотношение сторон полосы";
        o.Modified = c.Aspect != Def().Aspect;
        o.Tooltip = T("Fixed proportions of the frame: the rest of the window is filled with black bars");
        bool reset = false;
        if (ui::Row(ctx, T("Aspect ratio"), o, &reset)) {
            const char* aspects[] = {T("Free (by window)"), "16:9", "16:10", "4:3", "21:9"};
            int a = (int)c.Aspect;
            if (ImGui::Combo("##aspect", &a, aspects, IM_ARRAYSIZE(aspects))) c.Aspect = (sage::AspectMode)a;
            if (reset) c.Aspect = Def().Aspect;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "refresh rate hz hertz частота обновления гц";
        o.When = ui::Apply::Launch;
        o.Modified = c.RefreshRate != Def().RefreshRate;
        o.Disabled = c.Mode != sage::WindowMode::Fullscreen;
        o.DisabledWhy = T("Only fullscreen switches the monitor's refresh rate; a window uses the monitor's own");
        bool reset = false;
        if (ui::Row(ctx, T("Refresh rate"), o, &reset)) {
            std::set<int> rates;
            if (mon)
                for (const auto& vm : mon->Modes)
                    if (vm.Width == c.Width && vm.Height == c.Height && vm.RefreshHz > 0) rates.insert(vm.RefreshHz);
            char cur[48];
            if (c.RefreshRate <= 0) std::snprintf(cur, sizeof(cur), "%s", T("As the monitor"));
            else std::snprintf(cur, sizeof(cur), T("%d Hz"), c.RefreshRate);
            if (ImGui::BeginCombo("##hz", cur)) {
                if (ImGui::Selectable(T("As the monitor"), c.RefreshRate <= 0)) c.RefreshRate = 0;
                for (auto it = rates.rbegin(); it != rates.rend(); ++it) {
                    char l[32];
                    std::snprintf(l, sizeof(l), T("%d Hz"), *it);
                    if (ImGui::Selectable(l, c.RefreshRate == *it)) c.RefreshRate = *it;
                }
                ImGui::EndCombo();
            }
            if (reset) c.RefreshRate = Def().RefreshRate;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "resizable window размер окна растягивать";
        o.When = ui::Apply::Launch;
        o.Modified = c.Resizable != Def().Resizable;
        o.Disabled = c.Mode != sage::WindowMode::Windowed;
        o.DisabledWhy = T("Only a window has borders to drag");
        bool reset = false;
        if (ui::Row(ctx, T("Resizable window"), o, &reset)) {
            ui::Toggle("##resizable", &c.Resizable, T("The player can drag the window's edges"));
            if (reset) c.Resizable = Def().Resizable;
            ui::EndRow(ctx, o);
        }
    }
    if (ctx.GroupOpen && ctx.Filter.empty()) {
        ui::FullWidth(ctx);
        ui::Note(T("Screen settings apply when the game starts. Test the mode on this editor window first: "
                   "it comes back by itself if you do not confirm."));
        if (Sage::UI::Button(T("Test on this screen"), Sage::UI::ButtonStyle::Secondary)) {
            sage::display::DisplayRequest r;
            r.Mode = c.Mode;
            r.Monitor = c.Monitor;
            r.Width = c.Width;
            r.Height = c.Height;
            r.RefreshRate = c.RefreshRate;
            m_trialError.clear();
            if (m_trial.Begin(sage::Application::Get().GetWindow(), r, glfwGetTime(), 15.0, &m_trialError))
                m_openTrialPopup = true;
        }
        if (!m_trialError.empty()) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s", m_trialError.c_str());
        }
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("Frame rate"), nullptr, ui::Apply::Launch);
    {
        ui::RowOpts o;
        o.Keywords = "vsync vertical sync tearing вертикальная синхронизация разрывы";
        o.When = ui::Apply::Launch;
        o.Modified = c.VSync != Def().VSync;
        bool reset = false;
        if (ui::Row(ctx, T("Vertical sync (VSync)"), o, &reset)) {
            ui::Toggle("##vsync", &c.VSync, T("Removes tearing, limits frames to the monitor's rate"));
            if (reset) c.VSync = Def().VSync;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "fps frame cap limit ограничение кадров частота кадров";
        o.When = ui::Apply::Launch;
        o.Modified = c.FrameCap != Def().FrameCap;
        o.Tooltip = T("Frames per second at most: less heat and noise from the GPU");
        bool reset = false;
        if (ui::Row(ctx, T("Frame rate limit"), o, &reset)) {
            bool on = c.FrameCap > 0;
            if (ui::Toggle("##capon", &on)) c.FrameCap = on ? 144 : 0;
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::BeginDisabled(!on);
            ImGui::SetNextItemWidth(-FLT_MIN);
            int cap = on ? c.FrameCap : 144;
            if (ui::SliderInt("##cap", &cap, 20, 360, T("%d FPS")) && on) c.FrameCap = cap;
            ImGui::EndDisabled();
            if (reset) c.FrameCap = Def().FrameCap;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "background unfocused фон не в фокусе";
        o.When = ui::Apply::Launch;
        o.Modified = c.BackgroundFrameCap != Def().BackgroundFrameCap;
        o.Tooltip = T("Frames per second while the game window is not in focus: frames nobody sees still load the GPU");
        bool reset = false;
        if (ui::Row(ctx, T("Frame rate in background"), o, &reset)) {
            ui::SliderInt("##bgcap", &c.BackgroundFrameCap, 0, 120,
                          c.BackgroundFrameCap == 0 ? T("no limit") : T("%d FPS"));
            if (reset) c.BackgroundFrameCap = Def().BackgroundFrameCap;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);
}
