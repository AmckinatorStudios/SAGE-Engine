#include "../PanelWindows.h"
#include "EnvironmentPanel.h"
#include "EditorTheme.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/physics/PhysicsTypes.h"
#include "sage/ecs/LightSystem.h"
#include "sage/scene/Components.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/SkyModel.h"
#include "sage/render/Skybox.h"
#include "../AssetSlot.h"
#include "../Project.h"
#include <cstdio>
#include <cmath>
#include "../Localization.h"

void EnvironmentPanel::DrawSunLink(EditorHost& host, Scene& scene, LightingEnvironment& env) {
    (void)env;
    // СОЛНЦЕ ИЩЕТ ДВИЖОК, а не панель: правило «солнце — направленный свет с
    // наименьшим Id» одно на весь редактор и рантайм, и вторая его копия здесь
    // однажды разошлась бы с первой — панель показывала бы один объект, а
    // светил бы другой.
    const entt::entity sunEntity = sage::ecs::FindSunEntity(scene);

    if (sunEntity == entt::null) {
        ImGui::TextWrapped("%s", T("No directional light — no sun and no time of day."));
        if (ImGui::Button(T("Create a sun"))) {
            host.PushUndoSnapshot();
            GameObject sun = scene.CreateEmptyObject("Sun");
            sun.GetTransform().Position = {0.0f, 10.0f, 0.0f};
            sun.GetTransform().Rotation =
                sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
            LightComponent lc;
            lc.Kind = LightComponent::Type::Directional;
            lc.Color = {1.0f, 0.95f, 0.85f};
            lc.Intensity = 1.0f;
            scene.Registry().emplace<LightComponent>(sun.Entity(), lc);
            host.Selection().SetPrimary(sun.Id());
        }
        return;
    }

    // ЧТО СЕЙЧАС НА НЕБЕ — словами и числом. Без этого «почему у меня темно»
    // проверяется только методом тыка: высота солнца не написана нигде, а по
    // трём числам поворота её в уме не считают.
    //
    // Состояние берётся ЧЕРЕЗ СБОР ОСВЕЩЕНИЯ КАДРА, а не из scene.Lighting.Sun:
    // там лежит поле окружения, которое кадр не трогает, — светит объект-солнце,
    // и его поворот в это поле не попадает. Панель, читающая его напрямую,
    // показывала бы час, не имеющий отношения к картинке.
    const LightingEnvironment frame = sage::ecs::CollectLighting(scene);
    const sage::render::SkyState state = sage::render::EvaluateSky(frame);
    const float elevationDeg = glm::degrees(std::asin(glm::clamp(state.SunDirection.y, -1.0f, 1.0f)));
    const char* phase = state.DayFactor > 0.85f  ? T("day")
                        : state.DayFactor > 0.15f ? T("twilight")
                                                  : T("night");
    ImGui::Text(T("Now: %s (sun %.0f° above the horizon)"), phase, elevationDeg);
    const NameComponent* name = scene.Registry().try_get<NameComponent>(sunEntity);
    const IdComponent* id = scene.Registry().try_get<IdComponent>(sunEntity);
    ImGui::TextDisabled(T("Time of day = rotation of the object \"%s\""),
                        name ? name->Name.c_str() : "?");
    ImGui::SameLine();
    if (ImGui::Button(T("Select"))) host.Selection().SetPrimary(id ? id->Id : 0);
}

// --- НЕБО ------------------------------------------------------------------
void EnvironmentPanel::DrawSkySection(EditorHost& host, LightingEnvironment& env) {
    if (!EditorTheme::SectionHeader(T("Sky" "###Sky"), ImGuiTreeNodeFlags_DefaultOpen, nullptr,
                                    T("Time of day is baked into the images — the day/night model does not touch a textured sky."))) return;
    SkyboxSettings& sky = env.Skybox;

    if (ImGui::Checkbox(T("Enable Sky"), &sky.Enabled)) host.PushUndoSnapshot();
    if (!sky.Enabled) {
        return;
    }

    // РЕЖИМ — первым делом: от него зависит, какие настройки вообще имеют смысл.
    const char* kModes[] = {T("Procedural"), T("One colour"), T("One image (cross or panorama)"),
                            T("Cubemap folder"), T("Six separate files")};
    // Порядок в списке — по частоте, а не по значению перечисления: одной
    // картинкой небо приходит чаще всего, и стоять она должна первой из
    // текстурных. Поэтому индекс списка и Source связаны таблицей, а не равны.
    static const SkyboxSettings::Source kOrder[5] = {
        SkyboxSettings::Source::Procedural, SkyboxSettings::Source::Solid,
        SkyboxSettings::Source::Image, SkyboxSettings::Source::Cubemap,
        SkyboxSettings::Source::Faces};
    constexpr int kModeCount = (int)(sizeof(kOrder) / sizeof(kOrder[0]));
    int mode = 0;
    for (int i = 0; i < kModeCount; ++i)
        if (kOrder[i] == sky.Kind) mode = i;
    if (ImGui::Combo(T("Source"), &mode, kModes, kModeCount)) {
        host.PushUndoSnapshot();
        sky.Kind = kOrder[mode];
        // Пути НЕ стираются при переключении: вернуться к своему набору неба
        // надо уметь без повторного выбора папки.
    }

    // --- Небо одним цветом --------------------------------------------------
    //
    // Настроек ровно одна, и это не упрощение ради красоты: всё остальное
    // (сутки, закат, светила, звёзды) по смыслу режима не существует, и
    // показывать их значило бы обещать то, чего не будет.
    if (sky.Kind == SkyboxSettings::Source::Solid) {
        ImGui::ColorEdit3(T("Sky colour"), &sky.TopColor.x);
        EditorTheme::Hint(T("Flat fill: no gradient, no sun, no time of day. "
                            "Ambient light from the sky takes this colour"));
        host.TrackLastImGuiItem();
        // Окружающий свет при этом ЕСТЬ: режим «от неба» возьмёт этот самый
        // цвет, и предметы будут им подсвечены — в отличие от выключенного
        // неба, которое означает темноту.
        return;
    }

    if (sky.Kind == SkyboxSettings::Source::Procedural) {
        DrawSunLink(host, host.CurrentScene(), env);
        ImGui::Separator();

        if (ImGui::Checkbox(T("Day and night"), &sky.DayNight)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Sky colours AND the scene's light follow the sun's height:\n"
                                      "below the horizon it really gets dark, and the moon takes over.\n"
                                      "Turn off for a scene lit at a fixed staged angle."));
        }

        ImGui::TextDisabled("%s", T("Daytime"));
        ImGui::ColorEdit3(T("Zenith"), &sky.TopColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3(T("Horizon"), &sky.HorizonColor.x); host.TrackLastImGuiItem();
        if (sky.DayNight) {
            ImGui::TextDisabled("%s", T("Night"));
            ImGui::ColorEdit3(T("Zenith (night)"), &sky.NightTopColor.x); host.TrackLastImGuiItem();
            ImGui::ColorEdit3(T("Horizon (night)"), &sky.NightHorizonColor.x);
            host.TrackLastImGuiItem();
            ImGui::ColorEdit3(T("Sunset glow"), &sky.DuskColor.x); host.TrackLastImGuiItem();
            ImGui::ColorEdit3(T("Moonlight"), &sky.MoonlightColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Moonlight strength"), &sky.MoonlightIntensity, 0.005f, 0.0f, 1.0f,
                             "%.3f");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("At night the moon becomes the scene's light: it casts\n"
                                          "the shadows and sets how dark the night is."));
            }
        }

        ImGui::Separator();
        if (ImGui::Checkbox(T("Sun and moon in the sky"), &sky.Celestials)) host.PushUndoSnapshot();
        if (sky.Celestials) {
            // Цвет и направление диска солнца — у объекта-солнца; здесь только
            // то, что принадлежит НЕБУ: размер диска, луна, звёзды.
            ImGui::ColorEdit3(T("Sun disc colour"), &sky.SunColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Sun size"), &sky.SunSize, 0.002f, 0.005f, 0.4f, "%.3f");
            host.TrackLastImGuiItem();
            if (ImGui::Checkbox(T("Moon"), &sky.Moon)) host.PushUndoSnapshot();
            if (sky.Moon) {
                ImGui::ColorEdit3(T("Moon colour"), &sky.MoonColor.x); host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Moon size"), &sky.MoonSize, 0.002f, 0.005f, 0.4f, "%.3f");
                host.TrackLastImGuiItem();
            }
            ImGui::DragFloat(T("Stars"), &sky.StarIntensity, 0.02f, 0.0f, 3.0f, "%.2f");
            host.TrackLastImGuiItem();
        }
        return;
    }

    // --- Текстурное небо ---------------------------------------------------
    if (sky.Kind == SkyboxSettings::Source::Image) {
        // ОДИН ФАЙЛ — обычный случай для скачанного набора: в папке лежат
        // двадцать готовых небес, каждое отдельной картинкой, и выбрать надо
        // именно картинку, а не каталог.
        // СЛОТ, а не поле ввода с путём: картинку неба перетаскивают из панели
        // ассетов, как и всё остальное в редакторе (см. AssetSlot.h).
        ImGui::TextUnformatted(T("Image"));
        assetslot::Result r = assetslot::Draw(host, "sky_image", assetslot::Kind::Texture,
                                              sky.ImagePath, nullptr, T("Sky picture"));
        if (r.Changed) {
            host.PushUndoSnapshot();
            sky.ImagePath = r.Path;
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a sky image");
            c.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr"};
            c.FilterLabel = T("Images");
            c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_skyPick = -3;
        }

        const char* kLayouts[] = {T("Detect automatically"), T("Cross 4:3"), T("Cross 3:4"),
                                  T("Row 6:1"), T("Column 1:6"), T("Panorama 2:1")};
        if (ImGui::Combo(T("Layout"), &sky.ImageLayout, kLayouts, 6)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Usually the aspect ratio is enough to tell. Set it by hand\n"
                                      "if the sky came out scrambled: 4:3 is not always a cross."));
        }
        // ЧТО ИМЕННО РАСПОЗНАНО — словами и сразу, а не «попробуй и посмотри».
        if (!sky.ImagePath.empty()) {
            if (std::shared_ptr<Skybox> loaded = sage::render::SceneSkyCubemap(env)) {
                ImGui::TextDisabled("%s", T("The sky is assembled and in the frame"));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                                   T("The sky did not assemble — the reason is in the console"));
            }
        }
    } else if (sky.Kind == SkyboxSettings::Source::Cubemap) {
        // Слот типа «папка»: каталог перетаскивается из панели ассетов ровно
        // так же, как файл, и слот показывает, что именно выбрано.
        ImGui::TextUnformatted(T("Folder"));
        assetslot::Result r = assetslot::Draw(host, "sky_dir", assetslot::Kind::Folder,
                                              sky.CubemapDir, nullptr, T("Folder with six faces"));
        if (r.Changed) {
            host.PushUndoSnapshot();
            sky.CubemapDir = r.Path;
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a sky folder");
            c.Mode = FileBrowser::PickMode::PickFolder;
            c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_skyPick = -1;
        }
    } else {
        // Шесть отдельных файлов: имена чужого набора трогать не нужно.
        // НЕ static: язык интерфейса переключается на ходу, а статический
        // массив запомнил бы подписи того языка, при котором панель открыли
        // впервые.
        const char* kFaceLabels[6] = {T("Right (+X)"), T("Left (-X)"), T("Up (+Y)"),
                                      T("Down (-Y)"),  T("Front (+Z)"), T("Back (-Z)")};
        for (int i = 0; i < 6; ++i) {
            ImGui::PushID(i);
            ImGui::TextUnformatted(kFaceLabels[i]);
            assetslot::Result r = assetslot::Draw(host, "face", assetslot::Kind::Texture,
                                                  sky.FacePaths[i], nullptr, kFaceLabels[i]);
            if (r.Changed) {
                host.PushUndoSnapshot();
                sky.FacePaths[i] = r.Path;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = kFaceLabels[i];
                c.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
                c.FilterLabel = T("Images");
                c.StartDir = host.CurrentProject().AssetsDir();
                m_browser.Open(c);
                m_skyPick = i;
            }
            ImGui::PopID();
        }
        if (!sky.HasFaces())
            ImGui::TextDisabled("%s", T("Fill all six — the sky needs every face"));
    }

    ImGui::DragFloat(T("Brightness"), &sky.Intensity, 0.01f, 0.0f, 4.0f); host.TrackLastImGuiItem();
    ImGui::DragFloat(T("Rotation"), &sky.RotationDeg, 0.5f, -360.0f, 360.0f, "%.0f°");
    host.TrackLastImGuiItem();
}

// --- ОКРУЖАЮЩИЙ СВЕТ -------------------------------------------------------
void EnvironmentPanel::DrawAmbientSection(EditorHost& host, LightingEnvironment& env) {
    if (!EditorTheme::SectionHeader(T("Ambient light" "###Ambient light"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const char* kModes[] = {T("From the sky"), T("Custom values")};
    int mode = (int)env.AmbientMode;
    if (ImGui::Combo(T("Source##ambient"), &mode, kModes, 2)) {
        host.PushUndoSnapshot();
        env.AmbientMode = (LightingEnvironment::AmbientSource)mode;
    }

    if (env.AmbientMode == LightingEnvironment::AmbientSource::FromSky && env.Skybox.Enabled) {
        // Показываем РЕЗУЛЬТАТ, а не поля: значения считаются из неба и времени
        // суток, и правка полей ниже на них не влияет. Серые нередактируемые
        // образцы честнее, чем активные ползунки, которые ничего не делают.
        glm::vec3 skyC, groundC;
        env.ResolveAmbient(skyC, groundC);
        ImGui::ColorEdit3(T("Sky (computed)"), &skyC.x, ImGuiColorEditFlags_NoInputs |
                                                            ImGuiColorEditFlags_NoPicker);
        EditorTheme::Hint(T("Taken from the sky, so it darkens with it"));
        ImGui::ColorEdit3(T("Ground (computed)"), &groundC.x, ImGuiColorEditFlags_NoInputs |
                                                                  ImGuiColorEditFlags_NoPicker);
    } else if (env.AmbientMode == LightingEnvironment::AmbientSource::FromSky) {
        // НЕБА НЕТ — И СВЕТА ОТ НЕГО НЕТ, и поля тут ни при чём: они
        // принадлежат другому режиму. Показывать их рабочими значило бы
        // обещать свет, которого не будет, — а именно так и выглядела прошлая
        // подстановка «выключено небо — берём ваши значения»: человек выключал
        // небо, удалял все источники света, и сцена оставалась освещена
        // непонятно чем.
        ImGui::TextDisabled("%s", T("The sky is off — there is no ambient light"));
        ImGui::TextDisabled("%s", T("Switch to Custom values for light without a sky"));
        ImGui::BeginDisabled(true);
        glm::vec3 none(0.0f);
        ImGui::ColorEdit3(T("Sky"), &none.x, ImGuiColorEditFlags_NoInputs |
                                                 ImGuiColorEditFlags_NoPicker);
        ImGui::ColorEdit3(T("Ground"), &none.x, ImGuiColorEditFlags_NoInputs |
                                                    ImGuiColorEditFlags_NoPicker);
        float zero = 0.0f;
        ImGui::DragFloat(T("Strength"), &zero, 0.01f, 0.0f, 2.0f);
        ImGui::EndDisabled();
        return;
    } else {
        ImGui::ColorEdit3(T("Sky"), &env.SkyColor.x);
        EditorTheme::Hint(T("Sky tints upward faces, Ground — downward"));
        host.TrackLastImGuiItem();
        ImGui::ColorEdit3(T("Ground"), &env.GroundColor.x); host.TrackLastImGuiItem();
    }
    ImGui::DragFloat(T("Strength"), &env.AmbientStrength, 0.01f, 0.0f, 2.0f);
    host.TrackLastImGuiItem();
}

void EnvironmentPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    ImGui::Begin(T("Environment" "###Lighting"), open, panelwindows::WindowFlags("Lighting"));

    // Ответ диалога приходит ЧЕРЕЗ КАДР, поэтому цель выбора хранится числом, а
    // не указателем на поле: за этот кадр сцену могли перезагрузить (откат,
    // открытие другой), и указатель повис бы.
    if (m_browser.Draw()) {
        const std::string picked = host.CurrentProject().AssetRef(m_browser.Result());
        if (m_skyPick == -1) {
            host.PushUndoSnapshot();
            env.Skybox.CubemapDir = picked;
        } else if (m_skyPick == -3) {
            host.PushUndoSnapshot();
            env.Skybox.ImagePath = picked;
        } else if (m_skyPick >= 0 && m_skyPick < 6) {
            host.PushUndoSnapshot();
            env.Skybox.FacePaths[m_skyPick] = picked;
        }
        m_skyPick = -2;
    }

    // ГДЕ ЧТО НАСТРАИВАЕТСЯ — первым же абзацем.
    //
    // Раньше окно называлось «Lighting» и содержало вперемешку то, что
    // принадлежит СЦЕНЕ (небо, туман, окружающий свет), и то, что было
    // свойством объекта (солнце). Рядом жило окно «Game Settings» с качеством
    // теней и пост-обработкой — и на вопрос «где настраивается освещение?»
    // честного ответа не было: в двух местах и ещё в третьем, на объекте.
    //
    // Теперь граница проходит по СМЫСЛУ, а не по истории:
    //   • здесь — как выглядит МИР сцены: небо, воздух, окружающий свет. Едет
    //     в .sage вместе со сценой;
    //   • источники света — ОБЪЕКТЫ сцены со своими компонентами (солнце тоже);
    //   • сколько это стоит (разрешение теней, пост-обработка, объём) — Game
    //     Settings, они едут в sage.cfg вместе с проектом.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("The look of the scene's world: sky, air, ambient light. Saved with "
                               "the scene. Light SOURCES are objects — see the hierarchy. Quality "
                               "and cost are in Game Settings."));
    ImGui::PopStyleColor();
    ImGui::Separator();

    DrawSkySection(host, env);
    DrawAmbientSection(host, env);

    if (EditorTheme::SectionHeader(T("Fog" "###Fog"), ImGuiTreeNodeFlags_DefaultOpen, nullptr,
                                   T("Linear distance fog (applied in Shaded mode)"))) {
        if (ImGui::Checkbox(T("Enable Fog"), &env.Fog.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3(T("Fog Color"), &env.Fog.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Fog Start"), &env.Fog.Start, 0.2f, 0.0f, 500.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Fog End"), &env.Fog.End, 0.2f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
        if (env.Fog.End < env.Fog.Start) env.Fog.End = env.Fog.Start;
    }

    // ВЫПЕЧКА GI ПОКА УБРАНА ИЗ РЕДАКТОРА (см. EnvironmentPanel.h).
    ImGui::End();
}
