#include "../PanelWindows.h"
#include "EnvironmentPanel.h"
#include "EditorTheme.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/gi/GI.h"
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

EnvironmentPanel::~EnvironmentPanel() {
    // Дожидаемся фонового бейка: его вход самодостаточен, но поток обязан
    // завершиться до разрушения атомиков/мьютекса панели.
    if (m_bakeThread.joinable()) m_bakeThread.join();
}

void EnvironmentPanel::StartBake(EditorHost& host, const sage::gi::GISettings& settings) {
    if (m_bakeRunning) return;
    if (m_bakeThread.joinable()) m_bakeThread.join();

    // Вход собирается ЗДЕСЬ, на главном потоке — фоновой части сцена не нужна.
    sage::gi::BakeInput input = sage::gi::CollectBakeInput(host.CurrentScene(), settings);
    if (input.Items.empty()) {
        host.SetStatusMessage(T("GI: no static entities — add the GI Static component"));
        return;
    }

    m_bakeRunning = true;
    m_bakeProgress = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_bakeMutex);
        m_bakeResult.reset();
        m_bakePhase = T("Start");
    }
    m_bakeThread = std::thread([this, input = std::move(input)]() {
        auto result = sage::gi::Bake(input, [this](float f, const char* phase) {
            m_bakeProgress = f;
            std::lock_guard<std::mutex> lock(m_bakeMutex);
            m_bakePhase = phase;
        });
        std::lock_guard<std::mutex> lock(m_bakeMutex);
        m_bakeResult = std::move(result);
        m_bakeRunning = false;
    });
}

void EnvironmentPanel::DrawGISection(EditorHost& host) {
    if (!EditorTheme::SectionHeader(T("Global Illumination (baked)" "###Global Illumination (baked)"),
                                    ImGuiTreeNodeFlags_DefaultOpen, nullptr,
                                    T("Bakes indirect light to lightmaps (static) and a probe volume (dynamic); direct light stays realtime")))
        return;

    Scene& scene = host.CurrentScene();

    // Завершившийся фоновый бейк — применяем к сцене (на главном потоке).
    if (!m_bakeRunning) {
        std::shared_ptr<sage::gi::GIState> done;
        {
            std::lock_guard<std::mutex> lock(m_bakeMutex);
            done = std::move(m_bakeResult);
        }
        if (done) {
            // Сцена могла измениться/смениться, пока пёкся свет: применяем
            // только на геометрически ту же сцену, иначе UV не соответствуют.
            uint64_t now = sage::gi::ComputeGeometryHash(scene, done->Settings);
            if (now == done->GeometryHash) {
                scene.GI = std::move(done);
                host.SetStatusMessage(T("GI: bake finished — save the scene to write the lightmaps"));
            } else {
                host.SetStatusMessage(T("GI: the scene changed during the bake — the result was dropped"));
                LOG_WARN("GI") << "Сцена изменилась во время бейка — повтори запекание";
            }
        }
    }

    // Настройки живут в состоянии GI сцены (persist со сценой). Нет — дефолт.
    if (!scene.GI) scene.GI = std::make_shared<sage::gi::GIState>();
    sage::gi::GISettings& s = scene.GI->Settings;

    ImGui::DragInt(T("Texels / unit"), &s.TexelsPerUnit, 0.2f, 1, 64);
    const char* atlasSizes[] = {"512", "1024", "2048"};
    int atlasIdx = s.AtlasSize >= 2048 ? 2 : (s.AtlasSize >= 1024 ? 1 : 0);
    if (ImGui::Combo(T("Atlas size"), &atlasIdx, atlasSizes, 3))
        s.AtlasSize = atlasIdx == 2 ? 2048 : (atlasIdx == 1 ? 1024 : 512);
    ImGui::DragInt(T("Samples / texel"), &s.SampleCount, 1.0f, 8, 1024);
    ImGui::DragInt(T("Bounces"), &s.Bounces, 0.1f, 1, 8);
    ImGui::DragFloat(T("Probe cell size"), &s.ProbeCellSize, 0.1f, 0.5f, 10.0f);

    int staticCount = 0;
    {
        auto view = scene.Registry().view<GIStaticComponent>();
        staticCount = (int)view.size();
    }
    if (scene.GI->Baked)
        ImGui::Text(T("Baked: %d entities, %d page(s), probes %dx%dx%d"),
                    (int)scene.GI->Entities.size(), (int)scene.GI->Pages.size(),
                    scene.GI->Probes.Dims.x, scene.GI->Probes.Dims.y, scene.GI->Probes.Dims.z);
    else
        ImGui::TextDisabled(T("Not baked (%d static entities)"), staticCount);

    // Устарел ли бейк относительно текущей сцены.
    if (scene.GI->Baked &&
        sage::gi::ComputeGeometryHash(scene, s) != scene.GI->GeometryHash) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", T("Scene changed since bake — re-bake"));
    }

    if (m_bakeRunning) {
        std::string phase;
        {
            std::lock_guard<std::mutex> lock(m_bakeMutex);
            phase = m_bakePhase;
        }
        ImGui::ProgressBar(m_bakeProgress, ImVec2(-1, 0), phase.c_str());
    } else {
        if (ImGui::Button(T("Bake GI"), ImVec2(-1, 0))) StartBake(host, s);
        if (ImGui::Button(T("Mark static geometry"))) {
            // Все сущности с мешем и без динамического тела — статичные для GI.
            host.PushUndoSnapshot();
            int added = 0;
            auto view = scene.Registry().view<MeshRendererComponent>();
            for (auto e : view) {
                const auto& mr = view.get<MeshRendererComponent>(e);
                if (mr.Ref.type == MeshRef::Type::None) continue;
                if (scene.Registry().all_of<GIStaticComponent>(e)) continue;
                const auto* rb = scene.Registry().try_get<RigidBodyComponent>(e);
                if (rb && rb->Type != sage::physics::BodyType::Static) continue;
                scene.Registry().emplace<GIStaticComponent>(e);
                ++added;
            }
            host.SetStatusMessage(T("GI: entities marked static: ") + std::to_string(added));
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Clear bake")) && scene.GI->Baked) {
            host.PushUndoSnapshot();
            scene.GI = std::make_shared<sage::gi::GIState>();
            scene.GI->Settings = s;
        }
    }
}

// Солнце сцены. Раньше здесь стояли три поля прямо в настройках освещения —
// направление, цвет, яркость. Теперь солнце это ОБЪЕКТ (сущность с
// LightComponent{Directional}), и панель не редактирует его копию, а показывает,
// какой именно объект светит, и уводит к нему.
//
// Почему не оставить дубль полей здесь «для удобства». Потому что дубля не
// бывает: сущность можно повернуть гизмо, привязать к родителю и анимировать, а
// три поля в панели про это не знают. Панель, которая правит одно, а показывает
// другое, — худший вид удобства.
// Солнце — ОБЪЕКТ СЦЕНЫ, и время суток задаётся его поворотом.
//
// Здесь не настройки солнца, а дорога к нему: полей солнца в этом окне нет и
// быть не должно (их правит инспектор объекта). Но процедурное небо целиком
// зависит от того, где солнце стоит, и человек, пришедший «сделать ночь»,
// должен из этого места попасть к нужной ручке за одно нажатие, а не искать
// объект в иерархии по названию.
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
            GameObject sun = scene.CreateObject("Sun");
            sun.GetTransform().Position = {0.0f, 10.0f, 0.0f};
            sun.GetTransform().Rotation =
                sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
            LightComponent lc;
            lc.Kind = LightComponent::Type::Directional;
            lc.Color = {1.0f, 0.95f, 0.85f};
            lc.Intensity = 1.0f;
            scene.Registry().emplace<LightComponent>(sun.Entity(), lc);
            host.SetSelectedId(sun.Id());
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
    if (ImGui::Button(T("Select"))) host.SetSelectedId(id ? id->Id : 0);
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

        // Объёмный свет живёт в настройках ДВИЖКА, а ищут его здесь — рядом с
        // туманом, потому что для человека это одно и то же явление: воздух,
        // который видно. Ссылка дешевле, чем вторая копия десятка ползунков в
        // другом окне.
        ImGui::TextDisabled("%s", T("Volumetric light and clouds are a frame cost, not a scene "
                                    "property — they live in the engine settings."));
        if (ImGui::Button(T("Volumetric Light..."))) host.ShowSettingsWindow();
    }

    DrawGISection(host);

    ImGui::End();
}
