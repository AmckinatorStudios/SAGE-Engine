#include "GIBakeSection.h"

#include <imgui.h>

#include "EditorHost.h"
#include "EditorTheme.h"
#include "../Localization.h"
#include "sage/anim/Tween.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

GIBakeSection::~GIBakeSection() { Wait(); }

sage::gi::GISettings GIBakeSection::SettingsFor(Quality quality, float probeCell) {
    sage::gi::GISettings s;
    switch (quality) {
        case Quality::Draft:  s.TexelsPerUnit = 4;  s.SampleCount = 32;  s.Bounces = 1; break;
        case Quality::Medium: s.TexelsPerUnit = 8;  s.SampleCount = 96;  s.Bounces = 2; break;
        case Quality::High:   s.TexelsPerUnit = 12; s.SampleCount = 256; s.Bounces = 3; break;
    }
    s.ProbeCellSize = probeCell;
    return s;
}

bool GIBakeSection::IsStaticCandidate(Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    if (!reg.all_of<MeshRendererComponent>(e)) return false;
    // Тело, которое движет физика, — не статика; неподвижное (Static) — да:
    // стена с коллайдером остаётся стеной.
    if (const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e))
        if (rb->Type != sage::physics::BodyType::Static) return false;
    // Всё, что может сдвинуть объект во время игры: лайтмапа на нём осталась бы
    // висеть там, где он стоял при запекании.
    if (reg.any_of<CharacterControllerComponent, AnimationComponent, PropertyAnimatorComponent,
                   sage::anim::TweenComponent, ScriptComponent>(e))
        return false;
    return true;
}

int GIBakeSection::MarkStatic(Scene& scene) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> marks;
    for (auto [e, mr] : reg.view<MeshRendererComponent>().each())
        if (!reg.all_of<GIStaticComponent>(e) && IsStaticCandidate(scene, e)) marks.push_back(e);
    for (entt::entity e : marks) reg.emplace<GIStaticComponent>(e);
    return StaticCount(scene);
}

int GIBakeSection::StaticCount(Scene& scene) {
    int n = 0;
    for (auto e : scene.Registry().view<GIStaticComponent, MeshRendererComponent>()) {
        (void)e;
        ++n;
    }
    return n;
}

GIBakeSection::Status GIBakeSection::SceneStatus(Scene& scene) {
    if (!scene.GI || !scene.GI->Baked) return scene.GI && scene.GI->Probes.Valid() ? Status::Stale : Status::None;
    // Устаревает запечённое по ОТПЕЧАТКУ статики: сдвинули стену — свет уже
    // лежит не там, и сказать это надо раньше, чем человек заметит сам.
    return sage::gi::ComputeGeometryHash(scene, scene.GI->Settings) == scene.GI->GeometryHash
               ? Status::Baked
               : Status::Stale;
}

bool GIBakeSection::Start(EditorHost& host) {
    if (m_job) return false;
    Scene& scene = host.CurrentScene();
    if (StaticCount(scene) == 0) return false;
    auto job = std::make_unique<Job>();
    job->Settings = SettingsFor(m_quality, m_probeCell);
    job->Target = &scene;
    job->Hash = sage::gi::ComputeGeometryHash(scene, job->Settings);
    // Вход собирается ЗДЕСЬ, на главном потоке: сцену правят из интерфейса, и
    // читать её из фонового потока нельзя. Дальше — чистая математика.
    auto input = std::make_shared<sage::gi::BakeInput>(sage::gi::CollectBakeInput(scene, job->Settings));
    Job* raw = job.get();
    job->Thread = std::thread([raw, input] {
        raw->Result = sage::gi::Bake(*input, [raw](float p, const char* phase) {
            raw->Progress = p;
            std::lock_guard<std::mutex> lock(raw->PhaseMutex);
            raw->Phase = phase ? phase : "";
        });
        raw->Done = true;
    });
    m_job = std::move(job);
    m_lastMessage.clear();
    return true;
}

void GIBakeSection::Wait() {
    if (m_job && m_job->Thread.joinable()) m_job->Thread.join();
}

bool GIBakeSection::Poll(EditorHost& host) {
    if (!m_job || !m_job->Done) return false;
    if (m_job->Thread.joinable()) m_job->Thread.join();   // Wait мог дождаться раньше
    std::unique_ptr<Job> job = std::move(m_job);
    Scene& scene = host.CurrentScene();
    // Результат применяется, только если это ТА ЖЕ сцена и её статика за время
    // запекания не изменилась: иначе свет лёг бы на геометрию, которой уже нет.
    if (&scene != job->Target || !job->Result ||
        sage::gi::ComputeGeometryHash(scene, job->Settings) != job->Hash) {
        m_lastMessage = T("The scene changed while baking — bake again");
        return false;
    }
    host.PushUndoSnapshot();
    scene.GI = job->Result;
    m_lastMessage = T("Lighting baked. Save the scene to keep it");
    LOG_INFO("GI") << "Запечённое освещение применено к сцене";
    return true;
}

void GIBakeSection::Draw(EditorHost& host) {
    Poll(host);
    if (!EditorTheme::SectionHeader("sun", T("Baked Lighting" "###BakedLighting"), 0, nullptr,
                                    T("Bounced light baked ahead of time: lightmaps on static "
                                      "objects and light probes for everything that moves")))
        return;
    ImGui::PushID("gibake");
    Scene& scene = host.CurrentScene();
    const int statics = StaticCount(scene);

    switch (SceneStatus(scene)) {
        case Status::None:
            ImGui::TextDisabled("%s", T("Not baked: bounced light comes from the sky and screen-space GI"));
            break;
        case Status::Baked:
            ImGui::TextDisabled(T("Baked: %d objects, %d probes"), (int)scene.GI->Entities.size(),
                                (int)scene.GI->Probes.Probes.size());
            break;
        case Status::Stale:
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("Out of date: static objects changed since baking"));
            break;
    }

    ImGui::TextDisabled(T("Static objects: %d"), statics);
    ImGui::SameLine();
    ImGui::BeginDisabled(Busy());
    if (ImGui::Button(T("Mark static geometry"))) {
        host.PushUndoSnapshot();
        MarkStatic(scene);
    }
    EditorTheme::Hint(T("Marks every object with a mesh that nothing moves: no physics body, "
                        "character, animation, tween or script"));
    ImGui::EndDisabled();

    const char* qualities[] = {T("Draft"), T("Medium"), T("High")};
    int q = (int)m_quality;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(T("Quality"), &q, qualities, IM_ARRAYSIZE(qualities))) m_quality = (Quality)q;
    EditorTheme::Hint(T("Draft — seconds, for checking the look; High — clean light for the final build"));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat(T("Probe Spacing"), &m_probeCell, 1.0f, 6.0f, "%.1f m");
    EditorTheme::Hint(T("Distance between light probes: closer is more precise for moving objects"));

    if (Busy()) {
        std::string phase;
        {
            std::lock_guard<std::mutex> lock(m_job->PhaseMutex);
            phase = m_job->Phase;
        }
        ImGui::ProgressBar(m_job->Progress.load(), ImVec2(-FLT_MIN, 0.0f), phase.c_str());
    } else {
        ImGui::BeginDisabled(statics == 0);
        if (EditorTheme::ColoredButton(T("Bake Lighting"), EditorTheme::Role::Ok)) Start(host);
        ImGui::EndDisabled();
        if (statics == 0) EditorTheme::Hint(T("Mark static geometry first: lightmaps live on static objects"));
        ImGui::SameLine();
        ImGui::BeginDisabled(!scene.GI);
        if (ImGui::Button(T("Clear##gibake"))) {
            host.PushUndoSnapshot();
            scene.GI.reset();
            m_lastMessage.clear();
        }
        ImGui::EndDisabled();
    }
    if (!m_lastMessage.empty()) ImGui::TextDisabled("%s", m_lastMessage.c_str());
    ImGui::PopID();
}
