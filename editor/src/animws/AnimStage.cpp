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
#include "sage/render/Mesh.h"
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
    m_floorAligned = false;
    m_floorLift = 0.0f;
    m_alignModel = nullptr;
    m_alignRetries = 0;
    m_followValid = false;
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
    AlignToFloor();
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
    // Источник КОПИИ — по режиму предпросмотра, а не объекта уровня: открыть
    // граф для правки не значит переключить объект на граф, и наоборот. Пути
    // не стираются: копия — одноразовая, а настоящий компонент здесь не трогается.
    am->GraphInMemory = own;
    am->Source = own ? AnimationComponent::SourceKind::Graph : AnimationComponent::SourceKind::Clip;
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
            am->ClipSuspended = false;           // и не «вернулись из графа»: файл не перечитывать
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
    // Другая модель в той же копии или правка настроек импорта (.sageimport
    // перезагрузила модель): посадка и кадр считаются заново. Пока границ не
    // было (модель не успела подгрузиться) — несколько повторов, потом молча
    // остаёмся как есть: цикл на каждый кадр для цели без геометрии не нужен.
    if (am && StageRoot().Valid()) {
        const void* model = am->Model.get();
        const bool changed = model != m_alignModel || (model && am->Model->ImportTransform() != m_alignImport);
        if (changed || (!m_floorAligned && m_alignRetries < 120)) {
            ++m_alignRetries;
            AlignToFloor();
            if (m_floorAligned) FrameTarget();
        }
    }
    if (!am || !am->Model) return;
    if (m_source == Source::Clip && (m_externalTime || !m_playing)) {
        // Время ведёт таймлайн (бегунок) или стоим: поза — ровно в этот момент.
        am->Anim.Seek(m_time);
    } else if (m_source == Source::Clip) {
        m_time = am->Anim.Time();
    }
    UpdateFollow(dt);
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

// Направление от фокуса к глазу — единичный вектор.
static glm::vec3 EyeDir(float yawDeg, float pitchDeg) {
    const float yaw = glm::radians(yawDeg), pitch = glm::radians(pitchDeg);
    return glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
}

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

// Осмотр на месте: глаз не двигается, фокус переезжает на то же расстояние
// вперёд по новому взгляду. Так камера остаётся орбитальной (колесо, средняя
// кнопка, Frame работают как раньше), а ПКМ даёт свободный взгляд.
void AnimStage::Look(float dx, float dy) {
    const glm::vec3 eye = Eye();
    m_yaw -= dx * 0.15f;
    m_pitch = std::clamp(m_pitch + dy * 0.15f, -89.0f, 89.0f);
    m_focus = eye - m_distance * EyeDir(m_yaw, m_pitch);
}

void AnimStage::Fly(float forward, float right, float up, float dt, bool fast) {
    const glm::vec3 fwd = -EyeDir(m_yaw, m_pitch);
    const glm::vec3 side = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    // Скорость — от размера модели: персонаж и спичечный коробок обходятся за
    // одно и то же время.
    const float speed = std::max(1.0f, m_radius * 1.5f) * (fast ? 3.0f : 1.0f) * dt;
    m_focus += (fwd * forward + side * right + glm::vec3(0.0f, 1.0f, 0.0f) * up) * speed;
}

void AnimStage::ResetCamera() {
    m_yaw = 30.0f;
    m_pitch = 15.0f;
    FrameTarget();
}

// Центр костей — точка слежения. Центр, а не корневая кость: у многих
// скелетов над корнем стоит неподвижный «Armature», и следить за ним значило
// бы не следить вовсе.
glm::vec3 AnimStage::TrackedPoint() {
    if (const sage::anim::Skeleton* sk = Skeleton(); sk && sk->Count() > 0) {
        glm::vec3 sum(0.0f);
        for (int i = 0; i < sk->Count(); ++i) sum += glm::vec3(BoneWorld(i)[3]);
        return sum / (float)sk->Count();
    }
    GameObject root = StageRoot();
    return root.Valid() ? glm::vec3(m_scene->WorldMatrix(root.Entity())[3]) : glm::vec3(0.0f);
}

// Follow: фокус получает ТО ЖЕ смещение, что точка слежения. Угол и
// расстояние не трогаются — камера едет, а не переоценивает кадр. Во время
// проигрывания смещение сглажено (шаг бега не качает кадр); при перемотке
// и на стыке цикла — сразу: сглаживание прокатило бы камеру через всю сцену.
void AnimStage::UpdateFollow(float dt) {
    if (!m_follow) return;
    const glm::vec3 cur = TrackedPoint();
    if (!m_followValid) {
        m_followPoint = cur;
        m_followValid = true;
        return;
    }
    const glm::vec3 d = cur - m_followPoint;
    const bool snap = !m_playing || glm::length(d) > std::max(1.0f, m_radius) * 1.5f;
    const float a = snap ? 1.0f : 1.0f - std::exp(-std::max(0.0f, dt) * 10.0f);
    m_focus += d * a;
    m_followPoint += d * a;
}

static glm::vec3 AabbCorner(const glm::vec3& lo, const glm::vec3& hi, int i) {
    return glm::vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
}

// Границы ОТОБРАЖАЕМОЙ геометрии копии в мире: скелетная модель — габарит всех
// её подмешей в позе привязки (не положения костей: кость не граница меша)
// через те же матрицы, что при рисовании, «сущность * ImportTransform»;
// обычные меши потомков — габарит меша через мировую матрицу.
bool AnimStage::GeometryBounds(glm::vec3& lo, glm::vec3& hi, bool robust) {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    bool any = false;
    GameObject root = StageRoot();
    if (!root.Valid()) return false;
    auto addBox = [&](const glm::mat4& m, const glm::vec3& a, const glm::vec3& b) {
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 p = glm::vec3(m * glm::vec4(AabbCorner(a, b, i), 1.0f));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        any = true;
    };
    std::vector<entt::entity> all;
    Walk(*m_scene, root.Entity(), all);
    for (entt::entity e : all) {
        const glm::mat4 world = m_scene->WorldMatrix(e);
        const AnimationComponent* am = m_scene->Registry().try_get<AnimationComponent>(e);
        if (am && am->Model) {
            glm::vec3 a, b;
            if (robust ? am->Model->RobustBindBounds(a, b) : am->Model->BindBounds(a, b))
                addBox(world * am->Model->ImportTransform(), a, b);
            continue;
        }
        const auto* mr = m_scene->Registry().try_get<MeshRendererComponent>(e);
        if (mr && mr->MeshPtr && mr->MeshPtr->BoundsRadius() > 0.0f)
            addBox(world, mr->MeshPtr->BoundsMin(), mr->MeshPtr->BoundsMax());
    }
    return any;
}

// Посадка копии на пол — по границам геометрии, один раз (см. AnimStage.h).
// Только скелетная цель: у объекта без скелета положение корня — то, что
// анимирует клип свойств, и сдвиг его бы перебил.
void AnimStage::AlignToFloor() {
    m_floorAligned = false;
    m_floorLift = 0.0f;
    GameObject root = StageRoot();
    AnimationComponent* am = StageAnim();
    m_alignModel = am ? am->Model.get() : nullptr;
    if (am && am->Model) m_alignImport = am->Model->ImportTransform();
    if (!root.Valid() || !am || !am->Model) return;
    root.GetTransform().Position = glm::vec3(0.0f);
    glm::vec3 lo, hi;
    if (!GeometryBounds(lo, hi, false) || !std::isfinite(lo.y)) return;
    m_floorLift = -lo.y;
    root.GetTransform().Position.y = m_floorLift;
    m_floorAligned = true;
    m_alignTracked = TrackedPoint();
}

// Кадрирование: по настоящей геометрии (без аномальных частей), запасной
// вариант — кости без дальних выбросов, затем объекты копии.
void AnimStage::FrameTarget() {
    glm::vec3 lo(1e9f), hi(-1e9f);
    bool any = GeometryBounds(lo, hi, true);
    if (any) {
        // Модель в движении — рамка едет вместе с ней: границы посчитаны по
        // позе привязки, а точка слежения уже в текущей позе.
        if (m_floorAligned && StageAnim()) {
            const glm::vec3 shift = TrackedPoint() - m_alignTracked;
            lo += shift;
            hi += shift;
        }
    } else if (const sage::anim::Skeleton* sk = Skeleton(); sk && sk->Count() > 0) {
        // Кости: центр по медиане, границы — без костей дальше удвоенного
        // 90-го перцентиля расстояния от центра (одна кость на краю света
        // не должна отодвигать камеру).
        std::vector<glm::vec3> pts((size_t)sk->Count());
        for (int i = 0; i < sk->Count(); ++i) pts[(size_t)i] = glm::vec3(BoneWorld(i)[3]);
        std::vector<float> xs, ys, zs;
        for (const glm::vec3& p : pts) { xs.push_back(p.x); ys.push_back(p.y); zs.push_back(p.z); }
        auto median = [](std::vector<float>& v) {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        const glm::vec3 mid(median(xs), median(ys), median(zs));
        std::vector<float> dist;
        for (const glm::vec3& p : pts) dist.push_back(glm::length(p - mid));
        std::vector<float> sorted = dist;
        std::sort(sorted.begin(), sorted.end());
        const float limit = std::max(0.05f, sorted[(size_t)((sorted.size() - 1) * 0.9f)] * 2.0f);
        for (size_t i = 0; i < pts.size(); ++i) {
            if (dist[i] > limit) continue;
            lo = glm::min(lo, pts[i]);
            hi = glm::max(hi, pts[i]);
            any = true;
        }
    }
    if (!any && StageRoot().Valid()) {
        std::vector<entt::entity> all;
        Walk(*m_scene, StageRoot().Entity(), all);
        // Объект — точкой и размером: у одного куба «габарит» из одних
        // центров нулевой, и камера вставала вплотную.
        for (entt::entity e : all) {
            const glm::mat4 w = m_scene->WorldMatrix(e);
            const glm::vec3 c(w[3]);
            const float r = 0.5f * std::max({glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])),
                                             glm::length(glm::vec3(w[2]))});
            lo = glm::min(lo, c - glm::vec3(r));
            hi = glm::max(hi, c + glm::vec3(r));
            any = true;
        }
    }
    if (!any) {
        m_focus = glm::vec3(0.0f, 1.0f, 0.0f);
        m_radius = 1.0f;
    } else {
        m_focus = (lo + hi) * 0.5f;
        m_radius = std::max(0.25f, glm::length(hi - lo) * 0.5f);
    }
    // Расстояние — чтобы шар вокруг модели вписался в МЕНЬШИЙ из углов обзора
    // (по высоте и по ширине кадра); запас 5 %.
    const float halfV = glm::radians(20.0f);
    const float halfH = std::atan(std::tan(halfV) * std::max(0.1f, m_aspect));
    m_distance = std::clamp(m_radius / std::sin(std::min(halfV, halfH)) * 1.05f, 0.2f, 200.0f);
    m_followValid = false;   // слежение начинается с этого кадра
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
    m_aspect = (float)width / (float)std::max(1, height);
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
