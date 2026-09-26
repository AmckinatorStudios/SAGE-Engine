#include "../PanelWindows.h"
#include "EnvironmentPanel.h"
#include "EditorTheme.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/render/SkyPresets.h"
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
#include "EditorIcons.h"
#include "ui/ColorPicker.h"

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
            GameObject sun = sage::ecs::CreateSunEntity(scene);
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

// Картинка светила — слотом, как всякий ассет (см. AssetSlot.h). pick —
// номер цели для ответа файлового диалога (см. m_skyPick).
void EnvironmentPanel::DrawDiscTexture(EditorHost& host, const char* id, const char* label,
                                       std::string& path, int pick) {
    ImGui::TextUnformatted(label);
    assetslot::Result r = assetslot::Draw(host, id, assetslot::Kind::Texture, path, nullptr, label);
    if (r.Changed) {
        host.PushUndoSnapshot();
        path = r.Path;
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = label;
        c.Filters = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
        c.FilterLabel = T("Images");
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
        m_skyPick = pick;
    }
}

// --- НЕБО ------------------------------------------------------------------
void EnvironmentPanel::DrawSkySection(EditorHost& host, LightingEnvironment& env) {
    if (!EditorTheme::SectionHeader("sun", T("Sky" "###Sky"), ImGuiTreeNodeFlags_DefaultOpen, nullptr,
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
        Sage::UI::ColorField3(T("Sky colour"), &sky.TopColor.x);
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
        // Готовый вид — отправная точка: собрать узнаваемое небо из трёх
        // десятков полей с нуля — полчаса подбора цветов.
        if (ImGui::BeginCombo(T("Preset"), T("Apply a ready-made look…"))) {
            for (int i = 0; i < (int)sage::render::SkyPreset::Count; ++i) {
                const auto preset = (sage::render::SkyPreset)i;
                if (ImGui::Selectable(T(sage::render::SkyPresetLabel(preset)))) {
                    host.PushUndoSnapshot();
                    sage::render::ApplySkyPreset(sky, preset);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        if (ImGui::Checkbox(T("Day and night"), &sky.DayNight)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Sky colours AND the scene's light follow the sun's height:\n"
                                      "below the horizon it really gets dark, and the moon takes over.\n"
                                      "Turn off for a scene lit at a fixed staged angle."));
        }

        ImGui::TextDisabled("%s", T("Daytime"));
        Sage::UI::ColorField3(T("Zenith"), &sky.TopColor.x); host.TrackLastImGuiItem();
        Sage::UI::ColorField3(T("Horizon"), &sky.HorizonColor.x); host.TrackLastImGuiItem();
        if (sky.DayNight) {
            ImGui::TextDisabled("%s", T("Night"));
            Sage::UI::ColorField3(T("Zenith (night)"), &sky.NightTopColor.x); host.TrackLastImGuiItem();
            Sage::UI::ColorField3(T("Horizon (night)"), &sky.NightHorizonColor.x);
            host.TrackLastImGuiItem();
            Sage::UI::ColorField3(T("Sunset glow"), &sky.DuskColor.x); host.TrackLastImGuiItem();
            Sage::UI::ColorField3(T("Moonlight"), &sky.MoonlightColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Moonlight strength"), &sky.MoonlightIntensity, 0.005f, 0.0f, 1.0f,
                             "%.3f");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("At night the moon becomes the scene's light: it casts\n"
                                          "the shadows and sets how dark the night is."));
            }
        }

        // --- Форма градиента -----------------------------------------------
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("Gradient"));
        ImGui::DragFloat(T("Gradient curve"), &sky.GradientExponent, 0.01f, 0.05f, 8.0f, "%.2f");
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("How fast the zenith colour takes over going up.\n"
                                      "0.5 — a narrow bright band at the horizon; 1 — an even\n"
                                      "gradient; above 1 — a wide hazy horizon."));
        }
        ImGui::DragFloat(T("Horizon softness"), &sky.HorizonSoftness, 0.005f, 0.0f, 1.0f, "%.3f");
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Horizon offset"), &sky.HorizonOffset, 0.002f, -0.5f, 0.5f, "%.3f");
        host.TrackLastImGuiItem();
        if (ImGui::Checkbox(T("Own colour below the horizon"), &sky.Ground)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Off — the horizon colour goes down to the nadir.\n"
                                      "On — its own colour below the horizon (the deep blue\n"
                                      "void of Minecraft)."));
        }
        if (sky.Ground) {
            Sage::UI::ColorField3(T("Below horizon"), &sky.GroundColor.x); host.TrackLastImGuiItem();
            if (sky.DayNight) {
                Sage::UI::ColorField3(T("Below horizon (night)"), &sky.NightGroundColor.x);
                host.TrackLastImGuiItem();
            }
            ImGui::DragFloat(T("Edge width"), &sky.GroundBlend, 0.002f, 0.0f, 0.5f, "%.3f");
            host.TrackLastImGuiItem();
        }

        ImGui::Separator();
        if (ImGui::Checkbox(T("Sun and moon in the sky"), &sky.Celestials)) host.PushUndoSnapshot();
        if (sky.Celestials) {
            // Цвет и направление диска солнца — у объекта-солнца; здесь только
            // то, что принадлежит НЕБУ: вид диска, луна, звёзды.
            const char* kShapes[] = {T("Round"), T("Square")};
            Sage::UI::ColorField3(T("Sun disc colour"), &sky.SunColor.x); host.TrackLastImGuiItem();
            int sunShape = (int)sky.SunShape;
            if (ImGui::Combo(T("Sun shape"), &sunShape, kShapes, 2)) {
                host.PushUndoSnapshot();
                sky.SunShape = (SkyboxSettings::DiscShape)sunShape;
            }
            ImGui::DragFloat(T("Sun size"), &sky.SunSize, 0.002f, 0.005f, 0.6f, "%.3f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Sun brightness"), &sky.SunBrightness, 0.05f, 0.0f, 20.0f, "%.2f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Sun glow"), &sky.SunGlow, 0.02f, 0.0f, 4.0f, "%.2f");
            host.TrackLastImGuiItem();
            DrawDiscTexture(host, "sun_tex", T("Sun picture"), sky.SunTexture, -4);
            if (ImGui::Checkbox(T("Moon"), &sky.Moon)) host.PushUndoSnapshot();
            if (sky.Moon) {
                Sage::UI::ColorField3(T("Moon colour"), &sky.MoonColor.x); host.TrackLastImGuiItem();
                int moonShape = (int)sky.MoonShape;
                if (ImGui::Combo(T("Moon shape"), &moonShape, kShapes, 2)) {
                    host.PushUndoSnapshot();
                    sky.MoonShape = (SkyboxSettings::DiscShape)moonShape;
                }
                ImGui::DragFloat(T("Moon size"), &sky.MoonSize, 0.002f, 0.005f, 0.6f, "%.3f");
                host.TrackLastImGuiItem();
                if (ImGui::Checkbox(T("Moon phase"), &sky.MoonPhase)) host.PushUndoSnapshot();
                DrawDiscTexture(host, "moon_tex", T("Moon picture"), sky.MoonTexture, -5);
            }
            if (ImGui::Checkbox(T("Pixel art"), &sky.PixelArt)) host.PushUndoSnapshot();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Sun and moon pictures without smoothing — crisp pixels."));
            }
            ImGui::DragFloat(T("Stars"), &sky.StarIntensity, 0.02f, 0.0f, 3.0f, "%.2f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Star density"), &sky.StarDensity, 0.02f, 0.0f, 20.0f, "%.2f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Star size"), &sky.StarSize, 0.02f, 0.1f, 10.0f, "%.2f");
            host.TrackLastImGuiItem();
        }

        // --- Облака ---------------------------------------------------------
        ImGui::Separator();
        if (ImGui::Checkbox(T("Clouds"), &sky.Clouds)) host.PushUndoSnapshot();
        if (sky.Clouds) {
            const char* kStyles[] = {T("Blocks"), T("Soft")};
            int style = (int)sky.CloudKind;
            if (ImGui::Combo(T("Cloud style"), &style, kStyles, 2)) {
                host.PushUndoSnapshot();
                sky.CloudKind = (SkyboxSettings::CloudStyle)style;
            }
            Sage::UI::ColorField3(T("Cloud colour"), &sky.CloudColor.x); host.TrackLastImGuiItem();
            if (sky.DayNight) {
                Sage::UI::ColorField3(T("Cloud colour (night)"), &sky.NightCloudColor.x);
                host.TrackLastImGuiItem();
            }
            ImGui::DragFloat(T("Cloud height"), &sky.CloudHeight, 1.0f, -2000.0f, 5000.0f, "%.0f m");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Cloud size"), &sky.CloudScale, 0.1f, 0.1f, 1000.0f, "%.1f m");
            host.TrackLastImGuiItem();
            ImGui::SliderFloat(T("Coverage"), &sky.CloudCoverage, 0.0f, 1.0f, "%.2f");
            host.TrackLastImGuiItem();
            ImGui::SliderFloat(T("Cloud opacity"), &sky.CloudOpacity, 0.0f, 1.0f, "%.2f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat2(T("Wind"), &sky.CloudWind.x, 0.05f, -100.0f, 100.0f, "%.2f m/s");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Fade distance"), &sky.CloudFade, 5.0f, 10.0f, 50000.0f, "%.0f m");
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
            // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
            c.StartDir = c.Root = assetslot::ProjectRoot(host);
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
            // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
            c.StartDir = c.Root = assetslot::ProjectRoot(host);
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
                // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
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
    if (!EditorTheme::SectionHeader("light", T("Ambient light" "###Ambient light"),
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
        Sage::UI::ColorField3(T("Sky (computed)"), &skyC.x, Sage::UI::ColorField_ReadOnly);
        EditorTheme::Hint(T("Taken from the sky, so it darkens with it"));
        Sage::UI::ColorField3(T("Ground (computed)"), &groundC.x, Sage::UI::ColorField_ReadOnly);
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
        Sage::UI::ColorField3(T("Sky"), &none.x, Sage::UI::ColorField_ReadOnly);
        Sage::UI::ColorField3(T("Ground"), &none.x, Sage::UI::ColorField_ReadOnly);
        float zero = 0.0f;
        ImGui::DragFloat(T("Strength"), &zero, 0.01f, 0.0f, 2.0f);
        ImGui::EndDisabled();
        return;
    } else {
        Sage::UI::ColorField3(T("Sky"), &env.SkyColor.x);
        EditorTheme::Hint(T("Sky tints upward faces, Ground — downward"));
        host.TrackLastImGuiItem();
        Sage::UI::ColorField3(T("Ground"), &env.GroundColor.x); host.TrackLastImGuiItem();
    }
    ImGui::DragFloat(T("Strength"), &env.AmbientStrength, 0.01f, 0.0f, 2.0f);
    host.TrackLastImGuiItem();
}

void EnvironmentPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    ImGui::Begin(EditorIcons::WindowTitle("sun", T("Environment"), "Lighting").c_str(), open, panelwindows::WindowFlags("Lighting"));

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
        } else if (m_skyPick == -4) {
            host.PushUndoSnapshot();
            env.Skybox.SunTexture = picked;
        } else if (m_skyPick == -5) {
            host.PushUndoSnapshot();
            env.Skybox.MoonTexture = picked;
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

    if (EditorTheme::SectionHeader("cone", T("Fog" "###Fog"), ImGuiTreeNodeFlags_DefaultOpen, nullptr,
                                   T("Air of the scene: linear distance fog or exponential height fog as in Unreal"))) {
        if (ImGui::Checkbox(T("Enable Fog"), &env.Fog.Enabled)) host.PushUndoSnapshot();
        const char* kinds[] = {T("Linear"), T("Exponential height")};
        int kind = (int)env.Fog.Kind;
        if (ImGui::Combo(T("Fog Type"), &kind, kinds, 2)) {
            env.Fog.Kind = (FogSettings::Mode)kind;
            host.PushUndoSnapshot();
        }
        Sage::UI::ColorField3(T("Fog Color"), &env.Fog.Color.x); host.TrackLastImGuiItem();
        if (env.Fog.Kind == FogSettings::Mode::Linear) {
            ImGui::DragFloat(T("Fog Start"), &env.Fog.Start, 0.2f, 0.0f, 500.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Fog End"), &env.Fog.End, 0.2f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
            if (env.Fog.End < env.Fog.Start) env.Fog.End = env.Fog.Start;
        } else {
            ImGui::DragFloat(T("Density"), &env.Fog.Density, 0.001f, 0.0f, 1.0f, "%.4f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Height Falloff"), &env.Fog.HeightFalloff, 0.005f, 0.0f, 5.0f, "%.3f");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Base Height"), &env.Fog.BaseHeight, 0.1f, -1000.0f, 1000.0f, "%.1f m");
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Start Distance"), &env.Fog.Start, 0.2f, 0.0f, 1000.0f, "%.1f m");
            host.TrackLastImGuiItem();
            ImGui::SliderFloat(T("Max Opacity"), &env.Fog.MaxOpacity, 0.0f, 1.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Sun Glow"), &env.Fog.SunScatter, 0.01f, 0.0f, 4.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Sun Glow Size"), &env.Fog.SunExponent, 0.1f, 1.0f, 64.0f);
            host.TrackLastImGuiItem();
            EditorTheme::Hint(T("Denser near the ground, thinner up high; glows toward the sun."));
        }
    }

    // ВЫПЕЧКА GI ПОКА УБРАНА ИЗ РЕДАКТОРА (см. EnvironmentPanel.h).
    ImGui::End();
}
