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
    if (!EditorTheme::SectionHeader(T("Global Illumination (baked)" "###Global Illumination (baked)"), ImGuiTreeNodeFlags_DefaultOpen))
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
    ImGui::TextDisabled("%s", T("Bakes indirect light to lightmaps (static) and a probe"));
    ImGui::TextDisabled("%s", T("volume (dynamic); direct light stays realtime"));
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
void EnvironmentPanel::DrawSun(EditorHost& host, Scene& scene, LightingEnvironment& env) {
    if (!EditorTheme::SectionHeader(T("Sun" "###Sun"), ImGuiTreeNodeFlags_DefaultOpen)) return;

    // Ищем то же солнце, что возьмёт рендер: направленный свет с наименьшим id
    // (см. sage::ecs::CollectLighting).
    entt::registry& reg = scene.Registry();
    int sunId = 0;
    entt::entity sunEntity = entt::null;
    for (auto e : reg.view<LightComponent>()) {
        const LightComponent& lc = reg.get<LightComponent>(e);
        if (lc.Kind != LightComponent::Type::Directional) continue;
        const IdComponent* id = reg.try_get<IdComponent>(e);
        const int candidate = id ? id->Id : 0;
        if (sunEntity != entt::null && candidate >= sunId) continue;
        sunEntity = e;
        sunId = candidate;
    }

    if (sunEntity == entt::null) {
        ImGui::TextWrapped("%s", T("No directional light in the scene — no sun. Direction, colour and intensity "
          "live on the object itself."));
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
            reg.emplace<LightComponent>(sun.Entity(), lc);
            host.SetSelectedId(sun.Id());
        }
        return;
    }

    const LightComponent& lc = reg.get<LightComponent>(sunEntity);
    const NameComponent* name = reg.try_get<NameComponent>(sunEntity);
    ImGui::Text(T("Lit by object: %s"), name ? name->Name.c_str() : "?");
    ImGui::SameLine();
    if (ImGui::Button(T("Select"))) host.SetSelectedId(sunId);
    ImGui::Text(T("Intensity %.2f, direction (%.2f, %.2f, %.2f)"), lc.Intensity,
                env.Sun.Direction.x, env.Sun.Direction.y, env.Sun.Direction.z);
    ImGui::TextDisabled("%s", T("Edited in the object inspector; direction is its rotation"));
}

void EnvironmentPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    ImGui::Begin(T("Environment" "###Lighting"), open);

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

    if (EditorTheme::SectionHeader(T("Ambient (hemisphere)" "###Ambient (hemisphere)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3(T("Sky"), &env.SkyColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3(T("Ground"), &env.GroundColor.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Strength"), &env.AmbientStrength, 0.01f, 0.0f, 2.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("%s", T("Sky tints upward faces, Ground — downward"));
    }

    DrawSun(host, scene, env);

    if (EditorTheme::SectionHeader(T("Skybox" "###Skybox"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(T("Enable Skybox"), &env.Skybox.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3(T("Sky Top"), &env.Skybox.TopColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3(T("Sky Horizon"), &env.Skybox.HorizonColor.x); host.TrackLastImGuiItem();

        // Светила. Направление НЕ дублируется: солнце на небе рисуется по тому
        // же DirectionalLight, который освещает сцену, — иначе тени и солнце
        // рано или поздно разъедутся.
        ImGui::Separator();
        if (ImGui::Checkbox(T("Sun and moon in the sky"), &env.Skybox.Celestials))
            host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Sun disc along the sun direction, moon opposite it,\n"
              "stars appear as the sun goes below the horizon."));
        }
        if (env.Skybox.Celestials) {
            // Диска солнца здесь НЕТ намеренно: он принадлежит объекту-солнцу и
            // правится в его инспекторе вместе с направлением, цветом и
            // интенсивностью. Две ручки одного светила в разных окнах — это
            // ровно та путаница, из-за которой окно и переделано.
            ImGui::TextDisabled("%s", T("The sun's disc, direction and colour are on the sun "
                                        "object — select it in the hierarchy."));
            ImGui::Checkbox(T("Moon"), &env.Skybox.Moon);
            if (env.Skybox.Moon) {
                ImGui::ColorEdit3(T("Moon colour"), &env.Skybox.MoonColor.x); host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Moon size"), &env.Skybox.MoonSize, 0.002f, 0.005f, 0.4f, "%.3f");
                host.TrackLastImGuiItem();
            }
            ImGui::DragFloat(T("Stars"), &env.Skybox.StarIntensity, 0.02f, 0.0f, 3.0f, "%.2f");
            host.TrackLastImGuiItem();
        }
        ImGui::TextDisabled("%s", T("Procedural gradient (top -> horizon), no textures"));
    }

    if (EditorTheme::SectionHeader(T("Fog" "###Fog"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(T("Enable Fog"), &env.Fog.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3(T("Fog Color"), &env.Fog.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Fog Start"), &env.Fog.Start, 0.2f, 0.0f, 500.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Fog End"), &env.Fog.End, 0.2f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
        if (env.Fog.End < env.Fog.Start) env.Fog.End = env.Fog.Start;
        ImGui::TextDisabled("%s", T("Linear distance fog (applied in Shaded mode)"));

        // Объёмный свет живёт в настройках ДВИЖКА, а ищут его здесь — рядом с
        // туманом, потому что для человека это одно и то же явление: воздух,
        // который видно. Ссылка дешевле, чем вторая копия десятка ползунков в
        // другом окне.
        ImGui::TextDisabled("%s", T("Volumetric light and clouds are a frame cost, not a scene "
                                    "property — they live in the engine settings."));
        if (ImGui::Button(T("Volumetric Light..."))) host.ShowSettingsWindow();
    }

    DrawGISection(host);

    if (EditorTheme::SectionHeader(T("Scene lights" "###Scene lights"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // Света — сущности сцены (точечные/прожекторы); здесь список для
        // навигации с пометкой типа.
        int count = 0;
        auto view = scene.Registry().view<LightComponent, IdComponent, NameComponent>();
        for (auto e : view) {
            ++count;
            int id = view.get<IdComponent>(e).Id;
            const LightComponent& lc = view.get<LightComponent>(e);
            const char* tag = lc.Kind == LightComponent::Type::Spot ? "[spot] " : "[point] ";
            std::string label = tag + view.get<NameComponent>(e).Name + "##light" + std::to_string(id);
            if (ImGui::Selectable(label.c_str(), host.SelectedId() == id)) host.SetSelectedId(id);
        }
        if (count == 0) ImGui::TextDisabled("%s", T("(no light entities)"));
        ImGui::TextDisabled("%s", T("Add via Entity > Create Light; type/params in Inspector"));
        ImGui::TextDisabled(T("Shader limit: %d point + %d spot lights per frame"),
                            LightingEnvironment::MaxPointLights, LightingEnvironment::MaxSpotLights);
    }

    ImGui::End();
}
