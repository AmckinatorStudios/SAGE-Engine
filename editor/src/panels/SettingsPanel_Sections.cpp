// ---------------------------------------------------------------------------
// «Настройки игры» → разделы качества: профиль, тени, освещение, окружение,
// постобработка, интерфейс игры, дополнительно. Экран — SettingsPanel_Screen.cpp.
//
// Каждая строка — реальное поле EngineConfig (или компонента постобработки
// сцены): того, чего движок не умеет, здесь нет. Изменения в этих разделах
// видны во вьюпорте сразу; «Применить» записывает их в проект.
// ---------------------------------------------------------------------------
#include "SettingsPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "AssetSlot.h"
#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "PostChainUi.h"
#include "panels/settings/SettingsUi.h"
#include "sage/core/Paths.h"
#include "Project.h"
#include "sage/render/DebugView.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
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

// Общая постобработка сцены — компонент НЕ на камере с наибольшим приоритетом
// (то самое правило, по которому его выбирает рендер, ResolvePostChain).
entt::entity ScenePostEntity(Scene& scene) {
    entt::entity best = entt::null;
    int bestPriority = 0;
    auto& reg = scene.Registry();
    for (auto e : reg.view<sage::render::PostProcessComponent>()) {
        if (reg.all_of<CameraComponent>(e)) continue;
        const int p = reg.get<sage::render::PostProcessComponent>(e).Priority;
        if (best == entt::null || p > bestPriority) {
            best = e;
            bestPriority = p;
        }
    }
    return best;
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

// --- Тени ----------------------------------------------------------------------
void SettingsPanel::DrawShadows(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
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

// --- Освещение -----------------------------------------------------------------
void SettingsPanel::DrawLighting(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
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
        ui::Note(T("Ambient occlusion is an effect of the frame — see Post Processing."));
}

// --- Окружение -----------------------------------------------------------------
void SettingsPanel::DrawEnvironment(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    ui::BeginGroup(ctx, T("Sky and fog"),
                   T("Whether they are drawn at all. Their look — colors, time of day — is in the Environment window."));
    {
        ui::RowOpts o;
        o.Keywords = "sky skybox небо";
        ToggleRow(ctx, T("Sky"), c.Skybox, Def().Skybox, T("Procedural sky of the scene"), o);
    }
    {
        ui::RowOpts o;
        o.Keywords = "fog туман";
        ToggleRow(ctx, T("Fog"), c.Fog, Def().Fog, T("Distance fog of the scene"), o);
    }
    if (ctx.GroupOpen && ctx.Filter.empty()) {
        ui::FullWidth(ctx);
        if (EditorIcons::Button("world", T("Open Environment"), T("Sky, air and ambient light of the scene")))
            host.PanelVisible(EditorPanel::Environment) = true;
    }
    ui::EndGroup(ctx);

    ui::BeginGroup(ctx, T("Volumetric light and clouds"), T("The most expensive pass of the frame."));
    {
        ui::RowOpts o;
        o.Keywords = "volumetrics volumetric light clouds объёмный свет облака";
        o.Price = ui::Cost::Heavy;
        ToggleRow(ctx, T("Volumetric light"), c.Volumetrics, Def().Volumetrics, T("Light shafts and clouds"), o);
    }
    const bool vol = c.Volumetrics;
    auto dep = [&](ui::RowOpts& o) {
        o.Disabled = !vol;
        o.DisabledWhy = T("Volumetric light is off");
    };
    {
        ui::RowOpts o;
        dep(o);
        o.Keywords = "shafts rays лучи";
        ToggleRow(ctx, T("Light shafts"), c.VolumetricShafts, Def().VolumetricShafts, T("Rays of light in the air"), o);
    }
    {
        ui::RowOpts o;
        dep(o);
        o.Keywords = "clouds облака";
        ToggleRow(ctx, T("Clouds"), c.VolumetricClouds, Def().VolumetricClouds, T("A layer of volumetric clouds"), o);
    }
    auto slider = [&](const char* label, const char* kw, float& v, float def, float lo, float hi, const char* fmt,
                      const char* tip, bool needsClouds = false) {
        ui::RowOpts o;
        dep(o);
        if (needsClouds && vol && !c.VolumetricClouds) {
            o.Disabled = true;
            o.DisabledWhy = T("Clouds are off");
        }
        o.Keywords = kw;
        o.Tooltip = tip;
        o.Modified = std::abs(v - def) > 1e-5f;
        bool reset = false;
        if (!ui::Row(ctx, label, o, &reset)) return;
        ui::SliderFloat("##v", &v, lo, hi, fmt);
        if (reset) v = def;
        ui::EndRow(ctx, o);
    };
    slider(T("Haze density"), "haze density дымка плотность", c.VolumetricDensity, Def().VolumetricDensity, 0.0f, 0.2f,
           "%.3f", nullptr);
    slider(T("Shaft intensity"), "intensity яркость лучей", c.VolumetricIntensity, Def().VolumetricIntensity, 0.0f,
           4.0f, "%.2f", nullptr);
    slider(T("Haze height falloff"), "height falloff высота", c.VolumetricHeightFalloff, Def().VolumetricHeightFalloff,
           0.0f, 0.5f, "%.3f", T("How fast the haze thins out with height: 0 — the same at the ground and in the sky"));
    slider(T("Max distance"), "distance дальность", c.VolumetricMaxDistance, Def().VolumetricMaxDistance, 10.0f, 500.0f,
           T("%.0f m"), nullptr);
    slider(T("Cloud coverage"), "coverage облачность", c.CloudCoverage, Def().CloudCoverage, 0.0f, 1.0f, "%.2f",
           T("0 — clear sky, 1 — overcast"), true);
    slider(T("Cloud density"), "cloud density плотность облаков", c.CloudDensity, Def().CloudDensity, 0.1f, 4.0f,
           "%.2f", nullptr, true);
    {
        ui::RowOpts o;
        dep(o);
        o.Keywords = "cloud layer height слой облаков высота";
        o.Modified = std::abs(c.CloudBottom - Def().CloudBottom) > 1e-3f || std::abs(c.CloudTop - Def().CloudTop) > 1e-3f;
        bool reset = false;
        if (ui::Row(ctx, T("Cloud layer"), o, &reset)) {
            ImGui::DragFloatRange2("##layer", &c.CloudBottom, &c.CloudTop, 5.0f, 20.0f, 5000.0f, T("from %.0f m"),
                                   T("to %.0f m"), ImGuiSliderFlags_AlwaysClamp);
            if (reset) {
                c.CloudBottom = Def().CloudBottom;
                c.CloudTop = Def().CloudTop;
            }
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        dep(o);
        o.Keywords = "volumetric resolution half full разрешение объёма половина";
        o.Modified = std::abs(c.VolumetricScale - Def().VolumetricScale) > 1e-3f;
        o.Tooltip = T("Half is about four times cheaper; edges stay clean, the haze gets a little softer");
        bool reset = false;
        if (ui::Row(ctx, T("Volumetric resolution"), o, &reset)) {
            const char* items[] = {T("Full"), T("Half")};
            int idx = c.VolumetricScale < 0.75f ? 1 : 0;
            if (ImGui::Combo("##vs", &idx, items, IM_ARRAYSIZE(items))) c.VolumetricScale = idx ? 0.5f : 1.0f;
            if (reset) c.VolumetricScale = Def().VolumetricScale;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);
}

// --- Постобработка --------------------------------------------------------------
//
// ТА ЖЕ СИСТЕМА, НЕ ВТОРАЯ. Постобработка — компонент: на камере он
// обрабатывает её кадр, на другом объекте — кадр всех камер сцены. Раздел
// правит ЭТОТ общий компонент сцены (тот, что рендер и выбирает, см.
// ResolvePostChain) теми же элементами, что инспектор; камеры со своим
// компонентом показаны списком — их кадр решают они.
void SettingsPanel::DrawPost(EditorHost& host, ui::Ctx& ctx) {
    Scene& scene = host.CurrentScene();
    auto& reg = scene.Registry();
    const entt::entity e = ScenePostEntity(scene);
    sage::render::PostProcessComponent* pp =
        e != entt::null ? reg.try_get<sage::render::PostProcessComponent>(e) : nullptr;

    ui::BeginGroup(ctx, T("General"), T("Applies to every camera of the scene that has no post processing of its own."));
    {
        ui::RowOpts o;
        o.Keywords = "post processing enabled постобработка включить";
        o.Wide = true;
        if (ui::Row(ctx, T("Scene post processing"), o)) {
            if (!pp) {
                Sage::UI::TextSecondary("%s", T("The scene has no common post processing yet."));
                if (Sage::UI::Button(T("Create for the whole scene"), Sage::UI::ButtonStyle::Primary)) {
                    host.PushUndoSnapshot();
                    GameObject obj = scene.CreateEmptyObject("Post Processing");
                    reg.emplace<sage::render::PostProcessComponent>(obj.Entity());
                }
            } else {
                bool on = pp->Enabled;
                if (ui::Toggle("##ppon", &on, T("Effects of the frame are on"))) {
                    host.PushUndoSnapshot();
                    pp->Enabled = on;
                }
                GameObject owner(&reg, e);
                Sage::UI::TextSecondary(T("Stored in the scene, on the object «%s»"), owner.Name().c_str());
            }
            ui::EndRow(ctx, o);
        }
    }
    if (pp) {
        ui::RowOpts o;
        o.Keywords = "post profile preset default профиль постобработки стандартный";
        o.Wide = true;
        o.Disabled = !pp->Enabled;
        o.DisabledWhy = T("Post processing is off");
        if (ui::Row(ctx, T("Post processing profile"), o)) {
            // Профили — те, что есть у самого тракта: стандартный набор и
            // голый вывод на экран (кривая и кодирование без эффектов).
            if (Sage::UI::Button(T("Standard"), Sage::UI::ButtonStyle::Secondary)) {
                host.PushUndoSnapshot();
                pp->Chain = sage::render::PostChain::Default();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Tone mapping, bloom, ambient occlusion and the rest at their defaults"));
            ImGui::SameLine();
            if (Sage::UI::Button(T("Display only"), Sage::UI::ButtonStyle::Secondary)) {
                host.PushUndoSnapshot();
                pp->Chain = sage::render::PostChain::Display();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Only the tone curve and output to the screen — no effects"));
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);

    if (pp && ctx.Filter.empty()) {
        // Эффекты — тем же редактором, что в инспекторе: этапы в порядке
        // обработки кадра, у каждого свой выключатель.
        ImGui::BeginDisabled(!pp->Enabled);
        sage::editor::DrawPostChainEditor(&host, pp->Chain, "settingsPost");
        ImGui::EndDisabled();
    }
    if (ctx.Filter.empty()) {
        std::vector<std::string> own;
        for (auto ce : reg.view<CameraComponent, sage::render::PostProcessComponent>()) own.push_back(GameObject(&reg, ce).Name());
        if (!own.empty()) {
            std::string list;
            for (const auto& n : own) list += (list.empty() ? "" : ", ") + n;
            ImGui::Dummy(ImVec2(0.0f, Sage::UI::Get().SpacingSM));
            ui::Note((std::string(T("These cameras have their own post processing and use it instead: ")) + list).c_str());
        }
    }
}

// --- Интерфейс игры --------------------------------------------------------------
void SettingsPanel::DrawInterface(EditorHost& host, ui::Ctx& ctx) {
    sage::EngineConfig& c = host.Settings();
    ui::BeginGroup(ctx, T("Interface font"), T("The font of the game's interface text (not of the editor)."));
    {
        ui::RowOpts o;
        o.Keywords = "font шрифт";
        o.Modified = c.UiFont != Def().UiFont;
        o.Wide = true;
        bool reset = false;
        if (ui::Row(ctx, T("Font"), o, &reset)) {
            const assetslot::Result r =
                assetslot::Draw(host, "uifont", assetslot::Kind::Font, c.UiFont, nullptr, T("Built-in font"));
            if (r.Changed) c.UiFont = r.Path;
            if (reset) c.UiFont = Def().UiFont;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "font size pixel height размер шрифта растр";
        o.Modified = std::abs(c.UiFontPixelHeight - Def().UiFontPixelHeight) > 1e-3f;
        o.Tooltip = T("Size the font is rasterized at: larger — sharper big text, more memory");
        bool reset = false;
        if (ui::Row(ctx, T("Raster size"), o, &reset)) {
            ui::SliderFloat("##px", &c.UiFontPixelHeight, 12.0f, 128.0f, T("%.0f px"), 1.0f);
            if (reset) c.UiFontPixelHeight = Def().UiFontPixelHeight;
            ui::EndRow(ctx, o);
        }
    }
    {
        ui::RowOpts o;
        o.Keywords = "filtering pixel smooth фильтрация пиксельный сглаженный";
        o.Modified = c.UiFontFiltering != Def().UiFontFiltering;
        bool reset = false;
        if (ui::Row(ctx, T("Filtering"), o, &reset)) {
            const char* items[] = {T("Smooth"), T("Pixel (sharp)")};
            int idx = c.UiFontFiltering == sage::TextureFiltering::Nearest ? 1 : 0;
            if (ImGui::Combo("##filt", &idx, items, IM_ARRAYSIZE(items)))
                c.UiFontFiltering = idx ? sage::TextureFiltering::Nearest : sage::TextureFiltering::Smooth;
            if (reset) c.UiFontFiltering = Def().UiFontFiltering;
            ui::EndRow(ctx, o);
        }
    }
    ui::EndGroup(ctx);
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

    ui::BeginGroup(ctx, T("Diagnostics"));
    {
        ui::RowOpts o;
        o.Keywords = "debug view отладочный вид";
        o.When = ui::Apply::Launch;
        o.Modified = c.DebugView != Def().DebugView;
        o.Tooltip = T("The built game starts with this debug view (normals, shadows, depth…)");
        bool reset = false;
        if (ui::Row(ctx, T("Debug view in the game"), o, &reset)) {
            sage::render::DebugView cur = sage::render::DebugView::None;
            sage::render::ParseDebugView(c.DebugView.c_str(), cur);
            if (ImGui::BeginCombo("##dv", sage::render::DebugViewName(cur))) {
                for (int i = 0; i < (int)sage::render::DebugView::Count; ++i) {
                    const auto v = (sage::render::DebugView)i;
                    if (ImGui::Selectable(sage::render::DebugViewName(v), v == cur)) c.DebugView = sage::render::DebugViewId(v);
                }
                ImGui::EndCombo();
            }
            if (reset) c.DebugView = Def().DebugView;
            ui::EndRow(ctx, o);
        }
    }
    if (ctx.GroupOpen && ctx.Filter.empty()) {
        ui::FullWidth(ctx);
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
        case Category::Shadows:
            c.Shadows = d.Shadows; c.ShadowResolution = d.ShadowResolution; c.ShadowCascades = d.ShadowCascades;
            c.ShadowDistance = d.ShadowDistance; c.LocalShadows = d.LocalShadows;
            c.LocalShadowResolution = d.LocalShadowResolution;
            break;
        case Category::Lighting:
            c.ScreenSpaceGI = d.ScreenSpaceGI; c.Reflections = d.Reflections; c.PlanarReflections = d.PlanarReflections;
            break;
        case Category::Environment:
            c.Skybox = d.Skybox; c.Fog = d.Fog; c.Volumetrics = d.Volumetrics; c.VolumetricShafts = d.VolumetricShafts;
            c.VolumetricClouds = d.VolumetricClouds; c.VolumetricDensity = d.VolumetricDensity;
            c.VolumetricIntensity = d.VolumetricIntensity; c.VolumetricHeightFalloff = d.VolumetricHeightFalloff;
            c.VolumetricMaxDistance = d.VolumetricMaxDistance; c.CloudCoverage = d.CloudCoverage;
            c.CloudDensity = d.CloudDensity; c.CloudBottom = d.CloudBottom; c.CloudTop = d.CloudTop;
            c.VolumetricScale = d.VolumetricScale;
            break;
        case Category::Post: {
            Scene& scene = host.CurrentScene();
            const entt::entity e = ScenePostEntity(scene);
            if (e != entt::null) {
                host.PushUndoSnapshot();
                auto& pp = scene.Registry().get<sage::render::PostProcessComponent>(e);
                pp.Enabled = true;
                pp.Chain = sage::render::PostChain::Default();
            }
            break;
        }
        case Category::Interface:
            c.UiFont = d.UiFont; c.UiFontPixelHeight = d.UiFontPixelHeight; c.UiFontFiltering = d.UiFontFiltering;
            break;
        case Category::Advanced:
            c.Backend = d.Backend; c.DebugView = d.DebugView;
            break;
        default: break;
    }
}
