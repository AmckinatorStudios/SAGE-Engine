#include "SettingsPanel.h"
#include "EditorTheme.h"

#include <cfloat>
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>
#include "../Localization.h"
#include "EditorIcons.h"

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
        static const int kMsaaVals[] = {0, 2, 4, 8};
        const char* msaa[] = {T("Off"), "2x", "4x", "8x"};
        int msaaIdx = c.Msaa >= 8 ? 3 : c.Msaa >= 4 ? 2 : c.Msaa >= 2 ? 1 : 0;
        if (ImGui::Combo(T("MSAA"), &msaaIdx, msaa, IM_ARRAYSIZE(msaa)))
            c.Msaa = kMsaaVals[msaaIdx];
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
        ImGui::Checkbox(T("Shadows"), &c.Shadows);
        static const int kShadowVals[] = {512, 1024, 2048, 4096};
        const char* shadowRes[] = {"512", "1024", "2048", "4096"};
        int sr = c.ShadowResolution >= 4096 ? 3 : c.ShadowResolution >= 2048 ? 2 : c.ShadowResolution >= 1024 ? 1 : 0;
        if (ImGui::Combo(T("Shadow Resolution"), &sr, shadowRes, IM_ARRAYSIZE(shadowRes)))
            c.ShadowResolution = kShadowVals[sr];
        ImGui::Checkbox(T("Fog"), &c.Fog);
        ImGui::SameLine();
        ImGui::Checkbox(T("Skybox"), &c.Skybox);
    }

    // ПОСТ-ОБРАБОТКИ В НАСТРОЙКАХ БОЛЬШЕ НЕТ.
    //
    // Здесь стоял тракт проекта: экспозиция, свечение, цвет — на все камеры
    // сцены разом. Обработка принадлежит кадру, а кадр снимает камера, поэтому
    // она уехала компонентом на объект камеры и живёт в сцене вместе с ней.
    // Держать её ещё и здесь значило бы иметь две системы, где половина
    // настроек работает глобально, а половина через компонент.
    if (EditorTheme::SectionHeader("camera", T("Post Processing" "###Post Processing"))) {
        ImGui::TextWrapped("%s", T("Post processing is a camera component: select the camera "
                                   "and add \"Post Processing\" in the Inspector."));
    }

    // ОБЪЁМНЫЙ СВЕТ ПОКА УБРАН ИЗ РЕДАКТОРА.
    //
    // Раздел с десятком ползунков стоял здесь и обещал готовую возможность, а
    // возможность бета: лучи и облака ведут себя предсказуемо не во всякой
    // сцене, и настраивать их приходилось наугад. Обещание в интерфейсе дороже
    // неработающей функции: его видно всем, а работает оно у единиц. Код
    // движка на месте (engine/src/sage/render/Volumetrics.h) и включается
    // ключом в sage.cfg — вернуть раздел значит вернуть эти строки.

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
