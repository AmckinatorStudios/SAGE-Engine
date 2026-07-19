#include "SettingsPanel.h"
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>

// ============================================================================
//  Окно гибких настроек движка (EngineConfig). Редактирует host.Settings() и
//  сохраняет в <проект>/sage.cfg — Build Game кладёт файл в собранную игру, и
//  SagePlayer/игра читают его при запуске. Оконные параметры (размер/режим/
//  vsync) применяются при следующем запуске игры, не в самом редакторе.
// ============================================================================
void SettingsPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &open)) { ImGui::End(); return; }

    sage::EngineConfig& c = host.Settings();

    ImGui::TextDisabled("Гибкая конфигурация игры (сохраняется в проект как sage.cfg).");
    ImGui::Separator();

    // Пресеты качества: один клик выставляет display/graphics/post разом
    // (EngineConfig::ApplyPreset); дальше поля можно подстроить вручную.
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Preset:");
    ImGui::SameLine();
    if (ImGui::Button("Low")) c.ApplyPreset(sage::QualityPreset::Low);
    ImGui::SameLine();
    if (ImGui::Button("Medium")) c.ApplyPreset(sage::QualityPreset::Medium);
    ImGui::SameLine();
    if (ImGui::Button("High")) c.ApplyPreset(sage::QualityPreset::High);
    ImGui::SameLine();
    if (ImGui::Button("Ultra")) c.ApplyPreset(sage::QualityPreset::Ultra);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Low — слабые/старые ПК: без теней и пост-процесса, рендер 75%%\n"
                          "Medium — тени 1024 + тон-маппинг, без Bloom/SSAO\n"
                          "High — всё включено, тени 2048 (по умолчанию)\n"
                          "Ultra — тени 4096 + MSAA 4x\n"
                          "В игре: переменная окружения SAGE_QUALITY=low|medium|high|ultra");
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Window / Окно", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputInt("Width", &c.Width);
        ImGui::InputInt("Height", &c.Height);
        const char* modes[] = {"Windowed", "Borderless", "Fullscreen"};
        int mode = (int)c.Mode;
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) c.Mode = (sage::WindowMode)mode;
        ImGui::Checkbox("VSync", &c.VSync);
        ImGui::SameLine();
        ImGui::Checkbox("Resizable", &c.Resizable);
        ImGui::InputInt("Frame Cap (0=off)", &c.FrameCap);
        static const int kMsaaVals[] = {0, 2, 4, 8};
        const char* msaa[] = {"Off", "2x", "4x", "8x"};
        int msaaIdx = c.Msaa >= 8 ? 3 : c.Msaa >= 4 ? 2 : c.Msaa >= 2 ? 1 : 0;
        if (ImGui::Combo("MSAA", &msaaIdx, msaa, IM_ARRAYSIZE(msaa)))
            c.Msaa = kMsaaVals[msaaIdx];
        ImGui::TextDisabled("Оконные параметры применяются при запуске игры.");
    }

    if (ImGui::CollapsingHeader("Display / Дисплей", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* aspects[] = {"Free", "16:9", "16:10", "4:3", "21:9"};
        int a = (int)c.Aspect;
        if (ImGui::Combo("Aspect Ratio", &a, aspects, IM_ARRAYSIZE(aspects))) c.Aspect = (sage::AspectMode)a;
        ImGui::SliderFloat("Render Scale", &c.RenderScale, 0.25f, 2.0f, "%.2fx");
        ImGui::TextDisabled("Render Scale < 1 — быстрее; > 1 — суперсэмплинг (чётче).");
    }

    if (ImGui::CollapsingHeader("Graphics / Графика", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Shadows", &c.Shadows);
        static const int kShadowVals[] = {512, 1024, 2048, 4096};
        const char* shadowRes[] = {"512", "1024", "2048", "4096"};
        int sr = c.ShadowResolution >= 4096 ? 3 : c.ShadowResolution >= 2048 ? 2 : c.ShadowResolution >= 1024 ? 1 : 0;
        if (ImGui::Combo("Shadow Resolution", &sr, shadowRes, IM_ARRAYSIZE(shadowRes)))
            c.ShadowResolution = kShadowVals[sr];
        ImGui::Checkbox("Post-Processing", &c.PostProcessing);
        ImGui::Checkbox("Fog", &c.Fog);
        ImGui::SameLine();
        ImGui::Checkbox("Skybox", &c.Skybox);
    }

    if (ImGui::CollapsingHeader("Post-Process / Пост-эффекты")) {
        ImGui::BeginDisabled(!c.PostProcessing);
        ImGui::SliderFloat("Exposure", &c.Exposure, 0.1f, 4.0f);
        ImGui::SliderFloat("Gamma", &c.Gamma, 1.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &c.Saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Contrast", &c.Contrast, 0.5f, 2.0f);
        ImGui::SliderFloat("Vignette", &c.Vignette, 0.0f, 1.0f);

        ImGui::SeparatorText("Bloom");
        ImGui::Checkbox("Bloom", &c.Bloom);
        ImGui::BeginDisabled(!c.Bloom);
        ImGui::SliderFloat("Bloom Threshold", &c.BloomThreshold, 0.0f, 3.0f);
        ImGui::SliderFloat("Bloom Intensity", &c.BloomIntensity, 0.0f, 2.0f);
        ImGui::EndDisabled();

        ImGui::SeparatorText("Ambient Occlusion (SSAO)");
        ImGui::Checkbox("Ambient Occlusion", &c.AmbientOcclusion);
        ImGui::BeginDisabled(!c.AmbientOcclusion);
        ImGui::SliderFloat("AO Strength", &c.AOStrength, 0.0f, 4.0f);
        ImGui::SliderFloat("AO Radius", &c.AORadius, 0.05f, 2.0f);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    bool haveProject = host.CurrentProject().Loaded();
    ImGui::BeginDisabled(!haveProject);
    if (ImGui::Button("Save to Project")) {
        std::string path = (host.CurrentProject().Dir() / "sage.cfg").string();
        if (c.SaveFile(path)) host.SetStatusMessage("Настройки сохранены: sage.cfg");
        else host.SetStatusMessage("Не удалось сохранить sage.cfg");
    }
    ImGui::EndDisabled();
    if (!haveProject) {
        ImGui::SameLine();
        ImGui::TextDisabled("(откройте проект, чтобы сохранить)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) c = sage::EngineConfig{};

    ImGui::End();
}
