#define GLM_ENABLE_EXPERIMENTAL
#include "AnimStage.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "../EditorHost.h"
#include "../Localization.h"
#include "../Project.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/Texture.h"
#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"

using namespace sage::anim;

namespace {

// Объекты поддерева в порядке обхода (корень первым): копия CopySubtree
// повторяет порядок детей, и N-й объект копии — копия N-го объекта уровня.
void Walk(Scene& scene, entt::entity e, std::vector<entt::entity>& out) {
    if (e == entt::null || !scene.Registry().valid(e)) return;
    out.push_back(e);
    for (entt::entity c : scene.ChildrenOf(e)) Walk(scene, c, out);
}

bool Decompose(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    glm::vec3 skew;
    glm::vec4 persp;
    if (!glm::decompose(m, s, r, t, skew, persp)) return false;
    r = glm::normalize(r);
    return true;
}

} // namespace

void AnimStage::Shutdown() {
    m_floorMat.reset();
    m_scene = std::make_unique<Scene>("animation-stage");
    m_floorId = m_rootId = m_animId = -1;
    m_preview.Shutdown();
}

void AnimStage::SetTarget(EditorHost& host, int objectId) {
    if (objectId == m_targetId) return;
    m_targetId = objectId;
    Rebuild(host);
}

void AnimStage::EnsureFloor() {
    if (m_floorId >= 0 && m_scene->Get(m_floorId).Valid()) return;
    // ШАХМАТНЫЙ ПОЛ: позу читают по тому, как стопа стоит на плоскости, а
    // клетка даёт и масштаб, и перспективу. Текстура — две клетки 2×2 в
    // памяти с ближайшим пикселем: чёткие края без файла на диске.
    if (!m_floorMat) {
        const unsigned char a = 118, b = 84;
        const unsigned char px[16] = {a, a, a, 255, b, b, b, 255, b, b, b, 255, a, a, a, 255};
        m_floorMat = std::make_shared<Material>();
        m_floorMat->AlbedoTex = std::make_shared<Texture>(px, 2, 2, TextureFilter::Nearest, false, true);
        m_floorMat->Albedo = glm::vec3(1.0f);
        m_floorMat->Roughness = 0.95f;
        m_floorMat->Metallic = 0.0f;
        m_floorMat->Render.UVScaleX = m_floorMat->Render.UVScaleY = 10.0f;
    }
    GameObject floor = m_scene->CreateObject("##stage-floor");
    MeshRendererComponent& mr = floor.Renderer();
    mr.Ref.type = MeshRef::Type::Plane;
    mr.MeshPtr = ResourceManager::Instance().GetPlane();
    mr.MaterialPtr = m_floorMat;
    mr.MaterialPath = "stage-floor";
    floor.GetTransform().Scale = glm::vec3(20.0f, 1.0f, 20.0f);
    m_floorId = floor.Id();
}

void AnimStage::Rebuild(EditorHost& host) {
    m_scene = std::make_unique<Scene>("animation-stage");
    m_floorId = m_rootId = m_animId = m_realAnimId = -1;
    m_bone = -1;
    m_entity = entt::null;
    m_targetMesh.clear();
    m_clipDirty = !m_clipPath.empty();
    m_graphDirty = m_graph != nullptr;
    EnsureFloor();
    Scene& level = host.LevelScene();
    GameObject src = level.Get(m_targetId);
    if (!src.Valid()) return;
    GameObject root = sage::scene::CopySubtree(level, src.Entity(), *m_scene, entt::null);
    if (!root.Valid()) return;
    m_rootId = root.Id();
    // Копия стоит в начале координат: где цель стоит в уровне — дело уровня.
    root.GetTransform().Position = glm::vec3(0.0f);

    std::vector<entt::entity> real, copy;
    Walk(level, src.Entity(), real);
    Walk(*m_scene, root.Entity(), copy);
    for (size_t i = 0; i < real.size() && i < copy.size(); ++i) {
        if (!level.Registry().all_of<AnimationComponent>(real[i])) continue;
        m_realAnimId = GameObject(&level.Registry(), real[i]).Id();
        m_animId = GameObject(&m_scene->Registry(), copy[i]).Id();
        if (const auto* mr = level.Registry().try_get<MeshRendererComponent>(real[i])) m_targetMesh = mr->Ref.path;
        break;
    }
    if (AnimationComponent* am = StageAnim()) {
        // Копия не ходит: корневое движение увело бы её из кадра.
        am->RootMotion = false;
        am->PoseOverrides.clear();
    }
    sage::anim::UpdateAnimators(*m_scene, 0.0f);
    FrameTarget();
}

entt::entity AnimStage::StageEntityOf(EditorHost& host, int realId) {
    Scene& level = host.LevelScene();
    GameObject src = level.Get(m_targetId);
    GameObject root = StageRoot();
    if (!src.Valid() || !root.Valid()) return entt::null;
    std::vector<entt::entity> real, copy;
    Walk(level, src.Entity(), real);
    Walk(*m_scene, root.Entity(), copy);
    for (size_t i = 0; i < real.size() && i < copy.size(); ++i)
        if (GameObject(&level.Registry(), real[i]).Id() == realId) return copy[i];
    return entt::null;
}

AnimationComponent* AnimStage::StageAnim() {
    GameObject o = m_scene->Get(m_animId);
    return o.Valid() ? o.Registry()->try_get<AnimationComponent>(o.Entity()) : nullptr;
}

const sage::anim::Skeleton* AnimStage::Skeleton() {
    AnimationComponent* am = StageAnim();
    return am && am->Model ? &am->Model->GetSkeleton() : nullptr;
}

// Описание анимации цели, которое пространство НЕ ведёт само: слои с масками
// и граф объекта. Правят их в инспекторе на объекте уровня (они сохраняются со
// сценой) — копия догоняет. Сравнение, а не присваивание: вектор слоёв не
// копируется каждый кадр.
void AnimStage::SyncFromTarget(EditorHost& host) {
    Scene& level = host.LevelScene();
    GameObject target = level.Get(m_targetId);
    if (m_targetId >= 0 && !target.Valid()) {   // цель удалили
        m_targetId = -1;
        Rebuild(host);
        return;
    }
    GameObject real = level.Get(m_realAnimId);
    if (!real.Valid()) return;
    const AnimationComponent* src = real.Registry()->try_get<AnimationComponent>(real.Entity());
    AnimationComponent* dst = StageAnim();
    const auto* mr = real.Registry()->try_get<MeshRendererComponent>(real.Entity());
    if (!src || !dst || (mr && mr->Ref.path != m_targetMesh)) {
        Rebuild(host);   // сменили модель или сняли анимацию — копию заново
        return;
    }
    bool same = dst->Layers.size() == src->Layers.size();
    for (size_t i = 0; same && i < src->Layers.size(); ++i) {
        const AnimationLayer& a = src->Layers[i];
        const AnimationLayer& b = dst->Layers[i];
        same = a.Name == b.Name && a.ClipPath == b.ClipPath && a.Weight == b.Weight && a.Mask == b.Mask;
    }
    if (!same) dst->Layers = src->Layers;
}

void AnimStage::ShowClip(const std::string& path, const ClipAsset& clip, uint64_t revision) {
    // Тот же клип той же правки — ничего: иначе выбор клипа модели в
    // инспекторе тут же перебивался бы клипом, открытым в таймлайне.
    if (path == m_pushedPath && revision == m_pushedRevision) return;
    m_pushedPath = path;
    m_pushedRevision = revision;
    m_clipPath = path;
    m_clipRevision = revision;
    m_clip = clip;
    m_modelClip = -1;
    m_clipDirty = true;
}

void AnimStage::ShowModelClip(int index) {
    if (m_modelClip == index && m_clipPath.empty()) return;
    m_modelClip = index;
    m_clipPath.clear();
    m_clip = ClipAsset{};
    m_clipDirty = true;
}

void AnimStage::SetGraph(std::shared_ptr<const AnimGraph> graph, const std::string& path) {
    if (graph == m_graph && path == m_graphPath) return;
    m_graph = std::move(graph);
    m_graphPath = path;
    m_graphDirty = true;
}

float AnimStage::ParamValue(const std::string& name) const {
    auto it = m_params.find(name);
    if (it != m_params.end()) return it->second;
    const AnimGraphParam* p = m_graph ? m_graph->FindParam(name) : nullptr;
    return p ? p->Default : 0.0f;
}

float AnimStage::ClipDuration() {
    AnimationComponent* am = StageAnim();
    if (!am || !am->Model) return 0.0f;
    if (!m_clipPath.empty()) return am->OwnClips.empty() ? m_clip.Duration : am->OwnClips.front().Duration;
    const auto& clips = am->Model->Clips();
    return (m_modelClip >= 0 && m_modelClip < (int)clips.size()) ? clips[(size_t)m_modelClip].Duration : 0.0f;
}

// Источник позы копии — тем же компонентом и тем же проигрывателем, что в
// игре: клип файлом (в том числе правленный в памяти), клип модели или граф.
void AnimStage::ApplySource(float dt) {
    AnimationComponent* am = StageAnim();
    if (!am || !am->Model) return;
    const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
    am->Loop = m_loop;
    am->Speed = m_speed;
    am->BlendTime = 0.0f;   // смена клипа в предпросмотре — сразу, без перехода
    const bool own = m_source == Source::Graph && m_graph;
    if (own) {
        // Граф — из памяти: правка видна до сохранения. Путь и ревизия
        // выставлены так, чтобы система не перечитала его из файла.
        am->GraphPath = m_graphPath;
        am->GraphResolvedFrom = m_graphPath;
        am->GraphRevision = AnimGraphRevision();
        if (m_graphDirty || am->GraphAsset != m_graph) {
            am->GraphAsset = m_graph;
            m_graphDirty = false;
        }
        // Параметры — значения ПРЕДПРОСМОТРА: в объект уровня они не идут.
        for (const AnimGraphParam& p : m_graph->Params) am->Params.Floats[p.Name] = ParamValue(p.Name);
    } else {
        am->GraphPath.clear();
        am->GraphResolvedFrom.clear();
        am->GraphAsset = nullptr;
    }
    if (m_clipDirty) {
        m_clipDirty = false;
        if (!m_clipPath.empty()) {
            am->OwnClips.clear();
            am->OwnClips.push_back(Bind(m_clip, sk, &am->MissingBones));
            am->ClipPath = m_clipPath;
            am->ClipResolvedFrom = m_clipPath;   // из памяти, не из файла
            am->Anim.SetRig(&sk, &am->OwnClips);
            am->Anim.Play(0, m_loop);
        } else {
            am->ClipPath.clear();   // система вернёт клипы модели
            if (m_modelClip >= 0) am->Clip = m_modelClip;
        }
    }
    // Время ведёт таймлайн только у клипа: граф идёт своим временем.
    am->Playing = m_source == Source::Graph ? m_playing : (m_playing && !m_externalTime);
    (void)dt;
}

void AnimStage::Tick(EditorHost& host, float dt) {
    SyncFromTarget(host);
    ApplySource(dt);
    sage::anim::UpdateAnimators(*m_scene, dt);
    AnimationComponent* am = StageAnim();
    if (!am || !am->Model) return;
    if (m_source == Source::Clip && (m_externalTime || !m_playing)) {
        // Время ведёт таймлайн (бегунок) или стоим: поза — ровно в этот момент.
        am->Anim.Seek(m_time);
    } else if (m_source == Source::Clip) {
        m_time = am->Anim.Time();
    }
}

int AnimStage::RootSkeletalEntity() const { return m_animId; }

glm::mat4 AnimStage::BoneWorld(int bone) {
    AnimationComponent* am = StageAnim();
    if (!am || bone < 0 || bone >= (int)am->Anim.GlobalMatrices().size()) return glm::mat4(1.0f);
    return SkeletonToWorld() * am->Anim.GlobalMatrices()[(size_t)bone];
}

// Матрицы аниматора — в пространстве МОДЕЛИ до настроек импорта; меш рисуется
// как «сущность * ImportTransform * кость». Без второго множителя скелет и
// гизмо оставались в исходных единицах и не совпадали с геометрией при любом
// масштабе, повороте и сдвиге импорта.
glm::mat4 AnimStage::SkeletonToWorld() {
    AnimationComponent* am = StageAnim();
    GameObject o = m_scene->Get(m_animId);
    glm::mat4 m = m_scene->WorldMatrix(o.Entity());
    if (am && am->Model) m *= am->Model->ImportTransform();
    return m;
}

bool AnimStage::BoneLocal(int bone, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    AnimationComponent* am = StageAnim();
    const sage::anim::Skeleton* sk = Skeleton();
    if (!am || !sk || bone < 0 || bone >= sk->Count()) return false;
    const auto& g = am->Anim.GlobalMatrices();
    if ((int)g.size() != sk->Count()) return false;
    const int parent = sk->Joints[(size_t)bone].Parent;
    const glm::mat4 pg = parent >= 0 ? g[(size_t)parent] : sk->Root;
    return Decompose(glm::inverse(pg) * g[(size_t)bone], t, r, s);
}

void AnimStage::SetBoneWorld(int bone, const glm::mat4& world, bool t, bool r, bool s) {
    AnimationComponent* am = StageAnim();
    const sage::anim::Skeleton* sk = Skeleton();
    if (!am || !sk || bone < 0 || bone >= sk->Count()) return;
    const auto& g = am->Anim.GlobalMatrices();
    if ((int)g.size() != sk->Count()) return;
    const glm::mat4 model = glm::inverse(SkeletonToWorld()) * world;
    const int parent = sk->Joints[(size_t)bone].Parent;
    const glm::mat4 pg = parent >= 0 ? g[(size_t)parent] : sk->Root;
    glm::vec3 lt, ls;
    glm::quat lr;
    if (!Decompose(glm::inverse(pg) * model, lt, lr, ls)) return;
    if ((int)am->PoseOverrides.size() != sk->Count()) am->PoseOverrides.resize((size_t)sk->Count());
    JointPose& p = am->PoseOverrides[(size_t)bone];
    if (t) { p.HasTranslation = true; p.Translation = lt; }
    if (r) { p.HasRotation = true; p.Rotation = lr; }
    if (s) { p.HasScale = true; p.Scale = ls; }
    am->Anim.SetPoseOverride(&am->PoseOverrides);
    am->Anim.RefreshPose();
    NotePoseEdit();
}

void AnimStage::ClearPose() {
    if (AnimationComponent* am = StageAnim()) {
        am->PoseOverrides.clear();
        am->Anim.SetPoseOverride(nullptr);
        am->Anim.RefreshPose();
    }
}

void AnimStage::ClearBonePose(int bone) {
    AnimationComponent* am = StageAnim();
    if (!am || bone < 0 || bone >= (int)am->PoseOverrides.size()) return;
    am->PoseOverrides[(size_t)bone].Clear();
    am->Anim.RefreshPose();
}

bool AnimStage::HasPoseEdits() const {
    GameObject o = m_scene->Get(m_animId);
    const AnimationComponent* am = o.Valid() ? o.Registry()->try_get<AnimationComponent>(o.Entity()) : nullptr;
    if (!am) return false;
    for (const JointPose& p : am->PoseOverrides)
        if (p.Any()) return true;
    return false;
}

// --- Камера -------------------------------------------------------------------

void AnimStage::Orbit(float dYaw, float dPitch) {
    m_yaw += dYaw;
    m_pitch = std::clamp(m_pitch + dPitch, -89.0f, 89.0f);
}

void AnimStage::Pan(float dx, float dy) {
    const glm::mat4 v = View();
    const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
    const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
    const float k = m_distance * 0.0015f;
    m_focus += (-right * dx + up * dy) * k;
}

void AnimStage::Zoom(float wheel) {
    m_distance = std::clamp(m_distance * (wheel > 0 ? 0.88f : 1.14f), 0.2f, 200.0f);
}

void AnimStage::ResetCamera() {
    m_yaw = 30.0f;
    m_pitch = 15.0f;
    FrameTarget();
}

// Кадрирование цели: по костям (скелет) или по объектам копии.
void AnimStage::FrameTarget() {
    glm::vec3 lo(1e9f), hi(-1e9f);
    bool any = false;
    auto grow = [&](const glm::vec3& p) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        any = true;
    };
    if (AnimationComponent* am = StageAnim()) {
        for (size_t i = 0; i < am->Anim.GlobalMatrices().size(); ++i) grow(glm::vec3(BoneWorld((int)i)[3]));
    }
    if (!any) {
        std::vector<entt::entity> all;
        Walk(*m_scene, StageRoot().Entity(), all);
        // Объект — точкой и размером: у одного куба «габарит» из одних
        // центров нулевой, и камера вставала вплотную.
        for (entt::entity e : all) {
            const glm::mat4 w = m_scene->WorldMatrix(e);
            const glm::vec3 c(w[3]);
            const float r = 0.5f * std::max({glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])),
                                             glm::length(glm::vec3(w[2]))});
            grow(c - glm::vec3(r));
            grow(c + glm::vec3(r));
        }
    }
    if (!any) {
        m_focus = glm::vec3(0.0f, 1.0f, 0.0f);
        m_radius = 1.0f;
    } else {
        m_focus = (lo + hi) * 0.5f;
        m_radius = std::max(0.5f, glm::length(hi - lo) * 0.5f);
    }
    m_distance = m_radius * 3.2f;
}

glm::vec3 AnimStage::Eye() const {
    const float yaw = glm::radians(m_yaw), pitch = glm::radians(m_pitch);
    return m_focus + m_distance * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                            std::cos(pitch) * std::cos(yaw));
}

glm::mat4 AnimStage::View() const { return glm::lookAt(Eye(), m_focus, glm::vec3(0.0f, 1.0f, 0.0f)); }

glm::mat4 AnimStage::Proj(float aspect) const {
    return glm::perspective(glm::radians(40.0f), std::max(0.1f, aspect), std::max(0.01f, m_distance * 0.01f),
                            m_distance * 20.0f + 50.0f);
}

uint64_t AnimStage::Render(int width, int height) {
    EnsureFloor();
    // Светло-серый фон: тёмная модель на нём читается силуэтом, светлая —
    // тенью пола и светом студии.
    return m_preview.RenderSceneView(*m_scene, width, height, View(), Proj((float)width / (float)height), Eye(),
                                     glm::vec3(0.55f, 0.56f, 0.58f), "anim-stage");
}

std::string AnimStage::CreateEditableClip(EditorHost& host, const std::string& baseName) {
    namespace fs = std::filesystem;
    const fs::path dir = host.CurrentProject().AssetsDir() / "animations";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = baseName.empty() ? std::string("Clip") : baseName;
    fs::path file;
    for (int n = 1; n < 1000; ++n) {
        file = dir / ClipFileName(base, "Clip" + std::to_string(n));
        if (!fs::exists(file, ec)) break;
    }
    ClipAsset clip;
    clip.Name = file.stem().string();
    clip.Duration = 1.0f;
    clip.Editable = true;
    try {
        SaveClip(clip, file.string());
        sage::AssetDatabase::Instance().Register(file.string(), "animation");
    } catch (const std::exception& e) {
        host.SetStatusMessage(std::string(T("Could not create the clip: ")) + e.what());
        return {};
    }
    return host.CurrentProject().AssetRef(file);
}
