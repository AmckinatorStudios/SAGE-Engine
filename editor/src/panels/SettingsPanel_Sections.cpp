// ---------------------------------------------------------------------------
// «Настройки» → разделы качества: профиль, освещение и тени, дополнительно,
// и оформление редактора. Экран — SettingsPanel_Screen.cpp, управление —
// InputPanel (встраивается как есть).
//
// Каждая строка — реальное поле EngineConfig: того, чего движок не умеет,
// здесь нет. Изменения видны во вьюпорте сразу; «Применить» записывает их в
// проект.
// ---------------------------------------------------------------------------
#include "SettingsPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "panels/settings/SettingsUi.h"
#include "sage/core/Paths.h"
#include "Project.h"
#include "sage/rhi/GraphicsDevice.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"

namespace ui = sage::editor::settingsui;

namespace {
const sage::EngineConfig& Def() {
    static const sage::EngineConfig d;
    return d;
}

const char* PresetName(sage::QualityPreset p) {
    switch (p) {
        case sage::QualityPreset::Minimal: return T("Minimal");
        case sage::QualityPreset::Low: return T("Low quality");
        case sage::QualityPreset::Medium: return T("Medium quality");
        case sage::QualityPreset::High: return T("High quality");
        case sage::QualityPreset::Ultra: return T("Ultra");
    }
    return "";
}

// Строка-переключатель: самая частая форма в этих разделах.
void ToggleRow(ui::Ctx& ctx, const char* label, bool& v, bool def, const char* caption, ui::RowOpts o = {}) {
    o.Modified = v != def;
    bool reset = false;
    if (!ui::Row(ctx, label, o, &reset)) return;
    ui::Toggle("##t", &v, caption);
    if (reset) v = def;
    ui::EndRow(ctx, o);
}

void ResolutionCombo(int& value, int def, bool allow4096) {
    const int sizes[] = {512, 1024, 2048, 4096};
    char cur[32];
    std::snprintf(cur, sizeof(cur), "%d × %d", value, value);
    if (ImGui::BeginCombo("##res", cur)) {
        for (int s : sizes) {
            if (s == 4096 && !allow4096) continue;
            char l[48];
            std::snprintf(l, sizeof(l), "%d × %d%s", s, s, s == def ? T("  (default)") : "");
            if (ImGui::Selectable(l, value == s)) value = s;
        }
        ImGui::EndCombo();
    }
}

} // namespace

// --- Качество графики --------------------------------------------------------
void SettingsPanel::DrawQuality(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    sage::QualityPreset current;
    const bool matches = c.CurrentPreset(current);
    sage::QualityPreset chosen = sage::QualityPreset::High;
    sage::QualityPresetFromKey(c.QualityProfile, chosen);

    ui::BeginGroup(ctx, T("Quality profile"),
                   T("A profile sets shadows, anti-aliasing, render resolution, reflections, GI and the cost of "
                     "volumetrics at once. Fine-tune any of them afterwards."));
    {
        ui::RowOpts o;
        o.Keywords = "profile preset quality low medium high ultra minimal профиль качество низкое среднее высокое ультра";
        o.Wide = true;
        if (ui::Row(ctx, T("Profile"), o)) {
            const float gap = Sage::UI::Get().SpacingSM;
            const float w = (ImGui::GetContentRegionAvail().x - gap * (sage::kQualityPresetCount - 1)) /
                            (float)sage::kQualityPresetCount;
            for (int i = 0; i < sage::kQualityPresetCount; ++i) {
                const auto p = (sage::QualityPreset)i;
                if (i) ImGui::SameLine(0.0f, gap);
                const bool on = matches && current == p;
                if (Sage::UI::Button(PresetName(p), on ? Sage::UI::ButtonStyle::Primary : Sage::UI::ButtonStyle::Secondary,
                                     ImVec2(w, 0)))
                    c.ApplyPreset(p);
            }
            // Индикатор: какой профиль сейчас — или что настройки свои.
            ImGui::Dummy(ImVec2(0.0f, gap * 0.5f));
            ImGui::AlignTextToFramePadding();
            if (matches) {
                Sage::UI::TextSecondary(T("Current profile: %s"), PresetName(current));
            } else {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), T("Custom — changed from «%s»"),
                                   PresetName(chosen));
                ImGui::SameLine();
                if (EditorIcons::Button("refresh", T("Back to the profile"),
                                        T("Return every quality setting to the chosen profile")))
                    c.ApplyPreset(chosen);
            }
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("Resolution and anti-aliasing"));
    {
        ui::RowOpts o;
        o.Keywords = "render scale resolution supersampling масштаб разрешение рендера";
        o.Modified = std::abs(c.RenderScale - Def().RenderScale) > 1e-4f;
        o.Price = c.RenderScale > 1.25f ? ui::Cost::Heavy : ui::Cost::Normal;
        o.Tooltip = T("A smaller frame is stretched to the window — faster; a larger one is sharper and slower");
        bool reset = false;
        if (ui::Row(ctx, T("Render scale"), o, &reset)) {
            float pct = c.RenderScale * 100.0f;
            if (ui::SliderFloat("##scale", &pct, 25.0f, 200.0f, "%.0f%%", 5.0f)) c.RenderScale = pct / 100.0f;
            int w = 0, h = 0;
            c.ScaledResolution(c.Width, c.Height, w, h);
            Sage::UI::TextSecondary(T("%d × %d window → frame %d × %d"), c.Width, c.Height, w, h);
            if (reset) c.RenderScale = Def().RenderScale;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "msaa anti-aliasing antialiasing сглаживание лесенка";
        o.Modified = c.Msaa != Def().Msaa;
        o.Price = c.Msaa >= 8 ? ui::Cost::Heavy : ui::Cost::Normal;
        o.Tooltip = T("Smooths the edges of geometry. Each step doubles the memory of the frame");
        bool reset = false;
        if (ui::Row(ctx, T("Anti-aliasing (MSAA)"), o, &reset)) {
            const char* items[] = {T("Off"), "2×", "4×", "8×"};
            const int vals[] = {0, 2, 4, 8};
            int idx = c.Msaa >= 8 ? 3 : c.Msaa >= 4 ? 2 : c.Msaa >= 2 ? 1 : 0;
            if (ImGui::Combo("##msaa", &idx, items, IM_ARRAYSIZE(items))) c.Msaa = vals[idx];
            if (reset) c.Msaa = Def().Msaa;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("Performance"));
    {
        ui::RowOpts o;
        o.Keywords = "occlusion culling hidden отсечение невидимых";
        ToggleRow(ctx, T("Occlusion culling"), c.OcclusionCulling, Def().OcclusionCulling,
                  T("Skips objects hidden behind others: faster in dense scenes"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "threads multithreaded потоки многопоточный";
        o.When = ui::Apply::Launch;
        ToggleRow(ctx, T("Multithreaded frame preparation"), c.MultithreadedRender, Def().MultithreadedRender,
                  T("Culling and batching on worker threads"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "worker threads cores потоки ядра";
        o.When = ui::Apply::Launch;
        o.Modified = c.WorkerThreads != Def().WorkerThreads;
        o.Disabled = !c.MultithreadedRender;
        o.DisabledWhy = T("Needs multithreaded frame preparation");
        bool reset = false;
        if (ui::Row(ctx, T("Worker threads"), o, &reset)) {
            ui::SliderInt("##threads", &c.WorkerThreads, 0, 32, c.WorkerThreads == 0 ? T("automatic") : "%d");
            if (reset) c.WorkerThreads = Def().WorkerThreads;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);
}

// --- Освещение и тени -----------------------------------------------------------
// Тени — первыми: их настраивают чаще всего, и цена у них самая заметная.
static void DrawShadowGroups(sage::EngineConfig& c, ui::Ctx& ctx) {
    ui::BeginGroup(ctx, T("Sun shadows"),
                   T("Which light casts shadows is a property of the light object; here — their quality."));
    {
        ui::RowOpts o;
        o.Keywords = "shadows sun тени солнце";
        ToggleRow(ctx, T("Shadows"), c.Shadows, Def().Shadows, T("Shadow pass of the sun"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "shadow resolution map разрешение карты теней";
        o.Modified = c.ShadowResolution != Def().ShadowResolution;
        o.Disabled = !c.Shadows;
        o.DisabledWhy = T("Shadows are off");
        o.Price = c.ShadowResolution >= 4096 ? ui::Cost::Heavy : ui::Cost::Normal;
        bool reset = false;
        if (ui::Row(ctx, T("Shadow map resolution"), o, &reset)) {
            ResolutionCombo(c.ShadowResolution, Def().ShadowResolution, true);
            if (reset) c.ShadowResolution = Def().ShadowResolution;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "cascades каскады";
        o.Modified = c.ShadowCascades != Def().ShadowCascades;
        o.Disabled = !c.Shadows;
        o.DisabledWhy = T("Shadows are off");
        o.Tooltip = T("More cascades keep shadows sharp both near and far");
        bool reset = false;
        if (ui::Row(ctx, T("Cascades"), o, &reset)) {
            ui::SliderInt("##casc", &c.ShadowCascades, 1, 3, "%d");
            if (reset) c.ShadowCascades = Def().ShadowCascades;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "shadow distance дальность теней";
        o.Modified = std::abs(c.ShadowDistance - Def().ShadowDistance) > 1e-3f;
        o.Disabled = !c.Shadows;
        o.DisabledWhy = T("Shadows are off");
        o.Tooltip = T("Further away there are no shadows; the shorter, the sharper the near ones");
        bool reset = false;
        if (ui::Row(ctx, T("Shadow distance"), o, &reset)) {
            ui::SliderFloat("##dist", &c.ShadowDistance, 20.0f, 500.0f, T("%.0f m"), 5.0f);
            if (reset) c.ShadowDistance = Def().ShadowDistance;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("Lamp shadows"));
    {
        ui::RowOpts o;
        o.Keywords = "lamp point spot shadows тени ламп точечных прожекторов";
        o.Price = ui::Cost::Heavy;
        ToggleRow(ctx, T("Shadows of lamps"), c.LocalShadows, Def().LocalShadows,
                  T("Point and spot lights: each lamp costs extra passes"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "lamp shadow resolution разрешение теней ламп";
        o.Modified = c.LocalShadowResolution != Def().LocalShadowResolution;
        o.Disabled = !c.LocalShadows;
        o.DisabledWhy = T("Shadows of lamps are off");
        bool reset = false;
        if (ui::Row(ctx, T("Lamp shadow resolution"), o, &reset)) {
            ResolutionCombo(c.LocalShadowResolution, Def().LocalShadowResolution, true);
            if (reset) c.LocalShadowResolution = Def().LocalShadowResolution;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);
}

void SettingsPanel::DrawLighting(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    DrawShadowGroups(c, ctx);
    ui::BeginGroup(ctx, T("Global illumination"));
    {
        ui::RowOpts o;
        o.Keywords = "gi global illumination bounce ssgi глобальное освещение отражённый свет";
        o.Price = ui::Cost::Heavy;
        o.Tooltip = T("Bounced light: lit surfaces light their neighbours with their own color. Works with what is "
                      "on screen; baked probes and lightmaps are set in the Environment window");
        ToggleRow(ctx, T("Screen-space GI"), c.ScreenSpaceGI, Def().ScreenSpaceGI, T("Bounced light from the screen"), o);
    }
    ui::EndGroup(ctx);
    ui::BeginGroup(ctx, T("Reflections"));
    {
        ui::RowOpts o;
        o.Keywords = "reflections probes environment отражения зонды";
        ToggleRow(ctx, T("Reflections"), c.Reflections, Def().Reflections,
                  T("Environment map and reflection probes"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "mirror planar reflections зеркальные отражения вода";
        o.Price = ui::Cost::Heavy;
        o.Disabled = !c.Reflections;
        o.DisabledWhy = T("Reflections are off");
        ToggleRow(ctx, T("Mirror reflections"), c.PlanarReflections, Def().PlanarReflections,
                  T("Mirrors and water: the scene is drawn a second time"), o);
    }
    ui::EndGroup(ctx);
    if (ctx.Filter.empty())
        ui::Note(T("Ambient occlusion, bloom and other effects of the frame belong to the Post Processing "
                   "component of the camera; sky and fog — to the Environment window."));
}

// --- Дополнительно ---------------------------------------------------------------
void SettingsPanel::DrawAdvanced(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    using State = sage::rhi::GraphicsDevice::BackendInfo::State;
    ui::BeginGroup(ctx, T("Graphics API"), T("Changing the API of an open window is impossible in any engine."));
    {
        ui::RowOpts o;
        o.Keywords = "api backend opengl vulkan directx графический";
        o.When = ui::Apply::Launch;
        o.Modified = c.Backend != Def().Backend;
        bool reset = false;
        if (ui::Row(ctx, T("Graphics API"), o, &reset)) {
            const auto apis = sage::rhi::GraphicsDevice::Catalog();
            auto status = [](State s) -> const char* {
                switch (s) {
                    case State::Ready: return T("ready");
                    case State::Experimental: return T("experimental");
                    case State::Unavailable: return T("no driver on this PC");
                    case State::NotBuilt: return T("not in this build");
                    case State::NotImplemented: return T("not implemented yet");
                }
                return "";
            };
            const sage::rhi::GraphicsDevice::BackendInfo* current = &apis.front();
            for (const auto& api : apis)
                if (c.Backend == api.Id) current = &api;
            const std::string preview = std::string(current->Name) + " — " + status(current->Support);
            if (ImGui::BeginCombo("##api", preview.c_str())) {
                for (const auto& api : apis) {
                    const bool usable = api.Support == State::Ready || api.Support == State::Experimental;
                    ImGui::BeginDisabled(!usable);
                    if (ImGui::Selectable((std::string(api.Name) + " — " + status(api.Support)).c_str(), current == &api))
                        c.Backend = api.Id;
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (current->Support == State::Experimental)
                Sage::UI::TextSecondary("%s", T("Vulkan is not finished: the game starts on OpenGL unless "
                                                "SAGE_VULKAN_EXPERIMENTAL=1 is set."));
            if (reset) c.Backend = Def().Backend;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("This machine"), T("For a bug report: what the editor runs on."));
    if (ui::FreeBlock(ctx, T("This machine"), "diagnostics monitor renderer диагностика монитор видеокарта")) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        Sage::UI::TextSecondary(T("Editor renders with: %s %s"), device.BackendName(), device.ApiVersion().c_str());
        Sage::UI::TextSecondary(T("Interface scale: %d%% (system %d%%)"),
                                (int)std::lround(EditorTheme::EffectiveScale() * 100.0f),
                                (int)std::lround(EditorTheme::SystemScale() * 100.0f));
        for (const auto& m : Monitors()) {
            if (m.ModeKnown)
                Sage::UI::TextSecondary(T("Monitor %d «%s»: %d × %d, %d Hz, scale %d%%%s"), m.Index + 1, m.Name.c_str(),
                                        m.Current.Width, m.Current.Height, m.Current.RefreshHz,
                                        (int)std::lround(m.Scale * 100.0f), m.ScaleKnown ? "" : T(" (not reported)"));
            else
                Sage::UI::TextSecondary(T("Monitor %d «%s»: does not report its mode — skipped"), m.Index + 1,
                                        m.Name.c_str());
        }
        Sage::UI::TextSecondary(T("Settings file: %s"),
                                (host.CurrentProject().Dir() / "sage.cfg").string().c_str());
    }
    ui::EndGroup(ctx);
}

// --- Оформление редактора --------------------------------------------------------
// Настройка РЕДАКТОРА, а не игры: живёт в профиле человека и применяется
// сразу. Проект, навязывающий свою тему каждому, кто его открыл, — не то, чего
// ждут от проекта, поэтому в sage.cfg это не попадает.
void SettingsPanel::DrawAppearance(EditorHost& host, ui::Ctx& ctx) {
    ui::BeginGroup(ctx, T("Editor appearance"));
    {
        ui::RowOpts o;
        o.Keywords = "theme color dark light тема цвет тёмная светлая оформление";
        if (ui::Row(ctx, T("Color theme"), o)) {
            // Название темы не переводится: по нему её находят в themes/*.json.
            if (ImGui::BeginCombo("##theme", EditorTheme::Current().Name.c_str())) {
                for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
                    const bool active = theme.Id == EditorTheme::CurrentId();
                    if (ImGui::Selectable(theme.Name.c_str(), active) && !active) {
                        EditorTheme::SetTheme(theme.Id);
                        host.SetStatusMessage(T("Theme changed"));
                    }
                }
                ImGui::EndCombo();
            }
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "scale size dpi zoom масштаб размер крупнее";
        float scale = EditorTheme::UiScale();
        o.Modified = std::abs(scale - 1.0f) > 1e-3f;
        o.Tooltip = T("On a 4K screen a 16 px font is read with a magnifier — make the whole editor larger");
        bool reset = false;
        if (ui::Row(ctx, T("Interface scale"), o, &reset)) {
            float pct = scale * 100.0f;
            if (ui::SliderFloat("##uiscale", &pct, 75.0f, 200.0f, "%.0f%%", 5.0f)) EditorTheme::SetUiScale(pct / 100.0f);
            if (EditorTheme::SystemScale() > 1.001f)
                Sage::UI::TextSecondary(T("System scale %d%% is applied on top"),
                                        (int)std::lround(EditorTheme::SystemScale() * 100.0f));
            if (reset) EditorTheme::SetUiScale(1.0f);
            ui::EndRow(ctx, o);
        }
    }
    // Выгрузка темы файлом — ради неё вся система тем: взять текущую за
    // основу, поправить цвета в текстовом редакторе и положить в themes/.
    if (ui::FreeBlock(ctx, T("Export theme to themes/..."), "export theme file выгрузить тему файл")) {
        if (EditorIcons::Button("save", T("Export theme to themes/..."),
                                T("Copy of the current theme as a file: edit its colors and put it back"))) {
            const std::string path = EditorTheme::ThemesDir() + "/" + EditorTheme::Current().Id + "-copy.json";
            if (EditorTheme::ExportTheme(EditorTheme::Current(), path))
                host.SetStatusMessage(std::string(T("Theme saved:")) + " " + path);
            else
                host.SetStatusMessage(T("Could not write the theme file"));
        }
    }
    ui::EndGroup(ctx);
}

// --- По умолчанию для раздела ------------------------------------------------------
void SettingsPanel::ResetCategory(EditorHost& host, Category cat) {
    sage::EngineConfig& c = host.Settings();
    const sage::EngineConfig& d = Def();
    switch (cat) {
        case Category::Screen:
            c.Width = d.Width; c.Height = d.Height; c.Mode = d.Mode; c.Monitor = d.Monitor;
            c.RefreshRate = d.RefreshRate; c.Resizable = d.Resizable; c.VSync = d.VSync;
            c.FrameCap = d.FrameCap; c.BackgroundFrameCap = d.BackgroundFrameCap; c.Aspect = d.Aspect;
            break;
        case Category::Quality:
            c.ApplyPreset(sage::QualityPreset::High);
            c.OcclusionCulling = d.OcclusionCulling;
            c.MultithreadedRender = d.MultithreadedRender;
            c.WorkerThreads = d.WorkerThreads;
            break;
        case Category::Lighting:
            c.Shadows = d.Shadows; c.ShadowResolution = d.ShadowResolution; c.ShadowCascades = d.ShadowCascades;
            c.ShadowDistance = d.ShadowDistance; c.LocalShadows = d.LocalShadows;
            c.LocalShadowResolution = d.LocalShadowResolution;
            c.ScreenSpaceGI = d.ScreenSpaceGI; c.Reflections = d.Reflections; c.PlanarReflections = d.PlanarReflections;
            break;
        case Category::Advanced:
            c.Backend = d.Backend;
            break;
        default: break; // управление и оформление — со своими кнопками
    }
}
