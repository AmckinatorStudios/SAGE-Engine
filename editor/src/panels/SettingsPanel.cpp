#include "SettingsPanel.h"
#include "EditorTheme.h"

#include <cfloat>
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>
#include "../Localization.h"
#include "EditorIcons.h"
#include "sage/rhi/GraphicsDevice.h"

#include <string>
#include <vector>

// ============================================================================
//  Окно гибких настроек движка (EngineConfig). Редактирует host.Settings() и
//  сохраняет в <проект>/sage.cfg — Build Game кладёт файл в собранную игру, и
//  SagePlayer/игра читают его при запуске. Оконные параметры (размер/режим/
//  vsync) применяются при следующем запуске игры, не в самом редакторе.
// ============================================================================
void SettingsPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    // 640 в ширину, а не 420. Причина не в красоте: у ImGui подпись стоит СПРАВА
    // от поля, поэтому «Shadow Resolution» и «Frame Cap (0=off)» вместе со своим
    // полем требуют места, которого в 420 точках нет — подписи обрезались
    // посередине слова, и настройка превращалась в «Shadow Resolutic».
    ImGui::SetNextWindowSize(ImVec2(640, 620), ImGuiCond_FirstUseEver);
    // И нижняя граница: окно меньше этого показывает половину раздела, а полос
    // прокрутки у разделов нет.
    ImGui::SetNextWindowSizeConstraints(ImVec2(520, 360), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(EditorIcons::WindowTitle("gear", T("Game Settings"), "Game Settings").c_str(), &open)) { ImGui::End(); return; }

    sage::EngineConfig& c = host.Settings();

    // ЧТО ЗДЕСЬ, А ЧТО В ДРУГОМ ОКНЕ. Граница проходит по смыслу: здесь
    // ЦЕНА картинки (что включено и в каком качестве) — она принадлежит
    // проекту и едет в sage.cfg. Как выглядит мир сцены — небо, воздух,
    // окружающий свет — в окне Environment и едет в .sage. Источники света —
    // объекты сцены. Пока это лежало вперемешку, на вопрос «где настраивается
    // освещение» честного ответа не было.
    //

    // Пресеты качества: один клик выставляет display/graphics/post разом
    // (EngineConfig::ApplyPreset); дальше поля можно подстроить вручную.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Preset:"));
    ImGui::SameLine();
    if (ImGui::Button(T("Low"))) c.ApplyPreset(sage::QualityPreset::Low);
    ImGui::SameLine();
    if (ImGui::Button(T("Medium"))) c.ApplyPreset(sage::QualityPreset::Medium);
    ImGui::SameLine();
    if (ImGui::Button(T("High"))) c.ApplyPreset(sage::QualityPreset::High);
    ImGui::SameLine();
    if (ImGui::Button(T("Ultra"))) c.ApplyPreset(sage::QualityPreset::Ultra);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Low — weak/old PCs: no shadows or post-processing, 75%% render scale\n"
          "Medium — 1024 shadows + tone mapping, no Bloom/SSAO\n"
          "High — everything on, 2048 shadows (default)\n"
          "Ultra — 4096 shadows + MSAA 4x\n"
          "In game: environment variable SAGE_QUALITY=low|medium|high|ultra"));
    }
    ImGui::Separator();

    if (EditorTheme::SectionHeader("window", T("Window" "###Window settings"), ImGuiTreeNodeFlags_DefaultOpen,
                                   nullptr, T("Window settings apply when the game starts."))) {
        ImGui::InputInt(T("Width"), &c.Width);
        ImGui::InputInt(T("Height"), &c.Height);
        const char* modes[] = {T("Windowed"), T("Borderless"), T("Fullscreen")};
        int mode = (int)c.Mode;
        if (ImGui::Combo(T("Mode"), &mode, modes, IM_ARRAYSIZE(modes))) c.Mode = (sage::WindowMode)mode;
        ImGui::Checkbox(T("VSync"), &c.VSync);
        ImGui::SameLine();
        ImGui::Checkbox(T("Resizable"), &c.Resizable);
        ImGui::InputInt(T("Frame Cap (0=off)"), &c.FrameCap);
        ImGui::InputInt(T("Background Frame Cap (0=off)"), &c.BackgroundFrameCap);
        EditorTheme::Hint(T("Frames per second while the game window is not in focus: frames nobody sees still load the GPU"));
        static const int kMsaaVals[] = {0, 2, 4, 8};
        const char* msaa[] = {T("Off"), "2x", "4x", "8x"};
        int msaaIdx = c.Msaa >= 8 ? 3 : c.Msaa >= 4 ? 2 : c.Msaa >= 2 ? 1 : 0;
        if (ImGui::Combo(T("MSAA"), &msaaIdx, msaa, IM_ARRAYSIZE(msaa)))
            c.Msaa = kMsaaVals[msaaIdx];
        // Способ захвата курсора — выход для машины, где системный захват не
        // держится (курсор спрятан, но гуляет): см. sage::CursorLockMode.
        const char* locks[] = {T("Auto"), T("System"), T("Software")};
        int lock = (int)c.CursorLock;
        if (ImGui::Combo(T("Cursor Lock"), &lock, locks, IM_ARRAYSIZE(locks)))
            c.CursorLock = (sage::CursorLockMode)lock;
        EditorTheme::Hint(T("How a captured cursor is held. Software: the engine returns it to the window "
                            "centre every frame — use it if the hidden cursor drifts. Auto switches to "
                            "Software by itself when the system keeps dropping the capture"));
    }

    // ГРАФИЧЕСКИЙ API. Список — у RHI (GraphicsDevice::Catalog) вместе с
    // честным статусом: человек ищет «DirectX» и должен найти ответ, а не
    // пустое место. Выбор едет в sage.cfg и применяется при запуске игры —
    // сменить API у уже открытого окна нельзя ни в одном движке.
    if (EditorTheme::SectionHeader("chart", T("Graphics API" "###Graphics API"), ImGuiTreeNodeFlags_DefaultOpen,
                                   nullptr, T("Applies when the game starts."))) {
        using State = sage::rhi::GraphicsDevice::BackendInfo::State;
        const std::vector<sage::rhi::GraphicsDevice::BackendInfo> apis =
            sage::rhi::GraphicsDevice::Catalog();
        auto statusText = [](State s) -> const char* {
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
        const std::string preview = std::string(current->Name) + " — " + statusText(current->Support);
        if (ImGui::BeginCombo(T("API"), preview.c_str())) {
            for (const auto& api : apis) {
                const bool selectable = api.Support == State::Ready || api.Support == State::Experimental;
                const std::string label = std::string(api.Name) + " — " + statusText(api.Support);
                ImGui::BeginDisabled(!selectable);
                if (ImGui::Selectable(label.c_str(), current == &api)) c.Backend = api.Id;
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (current->Support == State::Experimental)
            EditorTheme::Hint(T("Vulkan is not finished: the game starts on OpenGL unless "
                                "SAGE_VULKAN_EXPERIMENTAL=1 is set."));
        EditorTheme::Hint(T("DirectX is not available: the engine's shaders are GLSL and a Direct3D "
                            "backend does not exist yet."));
        // На чём рисует сам редактор — чтобы выбор был не вслепую.
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        ImGui::TextDisabled(T("Editor renders with: %s %s"), device.BackendName(),
                            device.ApiVersion().c_str());
    }

    if (EditorTheme::SectionHeader("window", T("Display" "###Display"), ImGuiTreeNodeFlags_DefaultOpen,
                                       nullptr, T("Render Scale < 1 is faster; > 1 supersamples (sharper)."))) {
        const char* aspects[] = {T("Free"), "16:9", "16:10", "4:3", "21:9"};
        int a = (int)c.Aspect;
        if (ImGui::Combo(T("Aspect Ratio"), &a, aspects, IM_ARRAYSIZE(aspects))) c.Aspect = (sage::AspectMode)a;
        ImGui::SliderFloat(T("Render Scale"), &c.RenderScale, 0.25f, 2.0f, "%.2fx");
    }

    if (EditorTheme::SectionHeader("chart", T("Graphics" "###Graphics"), ImGuiTreeNodeFlags_DefaultOpen,
                                       nullptr, T("Whether a pass runs at all and at what resolution. Which light casts those shadows is a property of the light object."))) {
        static const int kShadowVals[] = {512, 1024, 2048, 4096};
        const char* shadowRes[] = {"512", "1024", "2048", "4096"};
        auto resIndex = [](int v) { return v >= 4096 ? 3 : v >= 2048 ? 2 : v >= 1024 ? 1 : 0; };
        ImGui::Checkbox(T("Shadows"), &c.Shadows);
        ImGui::BeginDisabled(!c.Shadows);
        int sr = resIndex(c.ShadowResolution);
        if (ImGui::Combo(T("Shadow Resolution"), &sr, shadowRes, IM_ARRAYSIZE(shadowRes)))
            c.ShadowResolution = kShadowVals[sr];
        ImGui::SliderInt(T("Shadow Cascades"), &c.ShadowCascades, 1, 3);
        EditorTheme::Hint(T("More cascades keep shadows sharp both near and far"));
        ImGui::SliderFloat(T("Shadow Distance"), &c.ShadowDistance, 20.0f, 500.0f, "%.0f m");
        ImGui::Checkbox(T("Lamp Shadows"), &c.LocalShadows);
        EditorTheme::Hint(T("Shadows of point and spot lights: each lamp costs extra passes"));
        ImGui::BeginDisabled(!c.LocalShadows);
        int lr = resIndex(c.LocalShadowResolution);
        if (ImGui::Combo(T("Lamp Shadow Resolution"), &lr, shadowRes, IM_ARRAYSIZE(shadowRes)))
            c.LocalShadowResolution = kShadowVals[lr];
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::SeparatorText(T("Lighting"));
        // Экранный GI — отражённый свет по кадру (звено «Global Illumination»).
        // Здесь только разрешение: сколько и как — у камеры, в её обработке.
        ImGui::Checkbox(T("Global Illumination (screen-space)"), &c.ScreenSpaceGI);
        EditorTheme::Hint(T("Bounced light: lit surfaces light their neighbours with their own color. "
                            "Strength and quality are set on the camera (Post Processing → Global Illumination)"));
        ImGui::Checkbox(T("Reflections"), &c.Reflections);
        ImGui::SameLine();
        ImGui::BeginDisabled(!c.Reflections);
        ImGui::Checkbox(T("Mirror Reflections"), &c.PlanarReflections);
        ImGui::EndDisabled();
        ImGui::Checkbox(T("Fog"), &c.Fog);
        ImGui::SameLine();
        ImGui::Checkbox(T("Skybox"), &c.Skybox);
        ImGui::Checkbox(T("Occlusion Culling"), &c.OcclusionCulling);
        EditorTheme::Hint(T("Skips objects hidden behind others: faster in dense scenes"));
    }

    // ПОСТ-ОБРАБОТКИ В НАСТРОЙКАХ БОЛЬШЕ НЕТ.
    //
    // Здесь стоял тракт проекта: экспозиция, свечение, цвет — на все камеры
    // сцены разом. Обработка принадлежит кадру, а кадр снимает камера, поэтому
    // она уехала компонентом на объект камеры и живёт в сцене вместе с ней.
    // Держать её ещё и здесь значило бы иметь две системы, где половина
    // настроек работает глобально, а половина через компонент.
    if (EditorTheme::SectionHeader("camera", T("Post Processing" "###Post Processing"))) {
        ImGui::TextWrapped("%s", T("Post processing is a component: on a camera it processes that "
                                   "camera, on any other object — every camera of the scene. Add "
                                   "\"Post Processing\" in the Inspector."));
    }

    // ОБЪЁМНЫЙ СВЕТ — ВЕРНУЛСЯ.
    //
    // Раздел убирали: лучи и облака вели себя предсказуемо не во всякой сцене.
    // Причины найдены и исправлены (render/Volumetrics.cpp): накопление по
    // кадрам сбрасывалось, когда в кадре два окна разного размера; лучи в
    // ортографии шли веером из камеры; облака ночью подсвечивались дневным
    // небом; один NaN навсегда оставался в истории. Ползунков — только те, что
    // меняют картинку понятным образом; остальное — в sage.cfg.
    if (EditorTheme::SectionHeader("sun", T("Volumetric Light" "###Volumetric Light"), 0, nullptr,
                                   T("Light shafts in the air and volumetric clouds. The most expensive pass of the frame"))) {
        ImGui::Checkbox(T("Enabled##volumetrics"), &c.Volumetrics);
        ImGui::BeginDisabled(!c.Volumetrics);
        ImGui::Checkbox(T("Light Shafts"), &c.VolumetricShafts);
        ImGui::SameLine();
        ImGui::Checkbox(T("Clouds"), &c.VolumetricClouds);
        ImGui::BeginDisabled(!c.VolumetricShafts);
        ImGui::SliderFloat(T("Haze Density"), &c.VolumetricDensity, 0.0f, 0.2f, "%.3f");
        ImGui::SliderFloat(T("Shaft Intensity"), &c.VolumetricIntensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat(T("Haze Height Falloff"), &c.VolumetricHeightFalloff, 0.0f, 0.5f, "%.3f");
        EditorTheme::Hint(T("How fast the haze thins out with height: 0 — the same at the ground and in the sky"));
        ImGui::SliderFloat(T("Max Distance"), &c.VolumetricMaxDistance, 10.0f, 500.0f, "%.0f m");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!c.VolumetricClouds);
        ImGui::SliderFloat(T("Cloud Coverage"), &c.CloudCoverage, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat(T("Cloud Density"), &c.CloudDensity, 0.1f, 4.0f, "%.2f");
        ImGui::DragFloatRange2(T("Cloud Layer"), &c.CloudBottom, &c.CloudTop, 5.0f, 20.0f, 5000.0f, "%.0f m", "%.0f m");
        ImGui::EndDisabled();
        const char* scales[] = {T("Full"), T("Half")};
        int scale = c.VolumetricScale < 0.75f ? 1 : 0;
        if (ImGui::Combo(T("Resolution##volumetrics"), &scale, scales, IM_ARRAYSIZE(scales)))
            c.VolumetricScale = scale == 1 ? 0.5f : 1.0f;
        EditorTheme::Hint(T("Half is about four times cheaper; edges stay clean, the haze gets a little softer"));
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    const bool haveProject = true; // проект есть всегда (см. LauncherPanel.h)
    ImGui::BeginDisabled(!haveProject);
    if (ImGui::Button(T("Save to Project"))) {
        std::string path = (host.CurrentProject().Dir() / "sage.cfg").string();
        if (c.SaveFile(path)) host.SetStatusMessage(T("Settings saved: sage.cfg"));
        else host.SetStatusMessage(T("Could not save sage.cfg"));
    }
    ImGui::EndDisabled();
    if (!haveProject) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(open a project to save)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Reset to Defaults"))) c = sage::EngineConfig{};

    ImGui::End();
}
