#include "LightingPanel.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/gi/GI.h"
#include "sage/physics/PhysicsTypes.h"
#include "sage/scene/Components.h"

LightingPanel::~LightingPanel() {
    // Дожидаемся фонового бейка: его вход самодостаточен, но поток обязан
    // завершиться до разрушения атомиков/мьютекса панели.
    if (m_bakeThread.joinable()) m_bakeThread.join();
}

void LightingPanel::StartBake(EditorHost& host, const sage::gi::GISettings& settings) {
    if (m_bakeRunning) return;
    if (m_bakeThread.joinable()) m_bakeThread.join();

    // Вход собирается ЗДЕСЬ, на главном потоке — фоновой части сцена не нужна.
    sage::gi::BakeInput input = sage::gi::CollectBakeInput(host.CurrentScene(), settings);
    if (input.Items.empty()) {
        host.SetStatusMessage("GI: нет статичных сущностей — добавь компонент GI Static");
        return;
    }

    m_bakeRunning = true;
    m_bakeProgress = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_bakeMutex);
        m_bakeResult.reset();
        m_bakePhase = "Старт";
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

void LightingPanel::DrawGISection(EditorHost& host) {
    if (!ImGui::CollapsingHeader("Global Illumination (baked)", ImGuiTreeNodeFlags_DefaultOpen))
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
                host.SetStatusMessage("GI: бейк завершён — сохрани сцену, чтобы записать лайтмапы");
            } else {
                host.SetStatusMessage("GI: сцена изменилась во время бейка — результат отброшен");
                LOG_WARN("GI") << "Сцена изменилась во время бейка — повтори запекание";
            }
        }
    }

    // Настройки живут в состоянии GI сцены (persist со сценой). Нет — дефолт.
    if (!scene.GI) scene.GI = std::make_shared<sage::gi::GIState>();
    sage::gi::GISettings& s = scene.GI->Settings;

    ImGui::DragInt("Texels / unit", &s.TexelsPerUnit, 0.2f, 1, 64);
    const char* atlasSizes[] = {"512", "1024", "2048"};
    int atlasIdx = s.AtlasSize >= 2048 ? 2 : (s.AtlasSize >= 1024 ? 1 : 0);
    if (ImGui::Combo("Atlas size", &atlasIdx, atlasSizes, 3))
        s.AtlasSize = atlasIdx == 2 ? 2048 : (atlasIdx == 1 ? 1024 : 512);
    ImGui::DragInt("Samples / texel", &s.SampleCount, 1.0f, 8, 1024);
    ImGui::DragInt("Bounces", &s.Bounces, 0.1f, 1, 8);
    ImGui::DragFloat("Probe cell size", &s.ProbeCellSize, 0.1f, 0.5f, 10.0f);

    int staticCount = 0;
    {
        auto view = scene.Registry().view<GIStaticComponent>();
        staticCount = (int)view.size();
    }
    if (scene.GI->Baked)
        ImGui::Text("Baked: %d entities, %d page(s), probes %dx%dx%d",
                    (int)scene.GI->Entities.size(), (int)scene.GI->Pages.size(),
                    scene.GI->Probes.Dims.x, scene.GI->Probes.Dims.y, scene.GI->Probes.Dims.z);
    else
        ImGui::TextDisabled("Not baked (%d static entities)", staticCount);

    // Устарел ли бейк относительно текущей сцены.
    if (scene.GI->Baked &&
        sage::gi::ComputeGeometryHash(scene, s) != scene.GI->GeometryHash) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Scene changed since bake — re-bake");
    }

    if (m_bakeRunning) {
        std::string phase;
        {
            std::lock_guard<std::mutex> lock(m_bakeMutex);
            phase = m_bakePhase;
        }
        ImGui::ProgressBar(m_bakeProgress, ImVec2(-1, 0), phase.c_str());
    } else {
        if (ImGui::Button("Bake GI", ImVec2(-1, 0))) StartBake(host, s);
        if (ImGui::Button("Mark static geometry")) {
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
            host.SetStatusMessage("GI: помечено статичными сущностей: " + std::to_string(added));
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear bake") && scene.GI->Baked) {
            host.PushUndoSnapshot();
            scene.GI = std::make_shared<sage::gi::GIState>();
            scene.GI->Settings = s;
        }
    }
    ImGui::TextDisabled("Bakes indirect light to lightmaps (static) and a probe");
    ImGui::TextDisabled("volume (dynamic); direct light stays realtime");
}

void LightingPanel::Draw(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    ImGui::Begin("Lighting");

    if (ImGui::CollapsingHeader("Ambient (hemisphere)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Sky", &env.SkyColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Ground", &env.GroundColor.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Strength", &env.AmbientStrength, 0.01f, 0.0f, 2.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Sky tints upward faces, Ground — downward");
    }

    if (ImGui::CollapsingHeader("Sun (directional)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Direction", &env.Sun.Direction.x, 0.02f, -1.0f, 1.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Color", &env.Sun.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Intensity", &env.Sun.Intensity, 0.02f, 0.0f, 5.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("Direction is where light TRAVELS; casts shadows");
    }

    if (ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Skybox", &env.Skybox.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3("Sky Top", &env.Skybox.TopColor.x); host.TrackLastImGuiItem();
        ImGui::ColorEdit3("Sky Horizon", &env.Skybox.HorizonColor.x); host.TrackLastImGuiItem();

        // Светила. Направление НЕ дублируется: солнце на небе рисуется по тому
        // же DirectionalLight, который освещает сцену, — иначе тени и солнце
        // рано или поздно разъедутся.
        ImGui::Separator();
        if (ImGui::Checkbox("Солнце и луна на небе", &env.Skybox.Celestials))
            host.PushUndoSnapshot();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Диск солнца по направлению Sun Direction, луна противоходом,\n"
                              "звёзды проступают, когда солнце уходит за горизонт.");
        }
        if (env.Skybox.Celestials) {
            ImGui::ColorEdit3("Цвет солнца", &env.Skybox.SunColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat("Размер солнца", &env.Skybox.SunSize, 0.002f, 0.005f, 0.4f, "%.3f");
            host.TrackLastImGuiItem();
            ImGui::Checkbox("Луна", &env.Skybox.Moon);
            if (env.Skybox.Moon) {
                ImGui::ColorEdit3("Цвет луны", &env.Skybox.MoonColor.x); host.TrackLastImGuiItem();
                ImGui::DragFloat("Размер луны", &env.Skybox.MoonSize, 0.002f, 0.005f, 0.4f, "%.3f");
                host.TrackLastImGuiItem();
            }
            ImGui::DragFloat("Звёзды", &env.Skybox.StarIntensity, 0.02f, 0.0f, 3.0f, "%.2f");
            host.TrackLastImGuiItem();
        }
        ImGui::TextDisabled("Procedural gradient (top -> horizon), no textures");
    }

    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable Fog", &env.Fog.Enabled)) host.PushUndoSnapshot();
        ImGui::ColorEdit3("Fog Color", &env.Fog.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat("Fog Start", &env.Fog.Start, 0.2f, 0.0f, 500.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat("Fog End", &env.Fog.End, 0.2f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
        if (env.Fog.End < env.Fog.Start) env.Fog.End = env.Fog.Start;
        ImGui::TextDisabled("Linear distance fog (applied in Shaded mode)");
    }

    DrawGISection(host);

    if (ImGui::CollapsingHeader("Scene lights", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        if (count == 0) ImGui::TextDisabled("(no light entities)");
        ImGui::TextDisabled("Add via Entity > Create Light; type/params in Inspector");
        ImGui::TextDisabled("Shader limit: %d point + %d spot lights per frame",
                            LightingEnvironment::MaxPointLights, LightingEnvironment::MaxSpotLights);
    }

    ImGui::End();
}
