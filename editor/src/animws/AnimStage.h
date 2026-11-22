#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "../AssetPreview.h"
#include "sage/anim/AnimGraph.h"
#include "sage/anim/ClipFile.h"
#include "sage/scene/Scene.h"

class EditorHost;
struct AnimationComponent;
struct Material;

// ---------------------------------------------------------------------------
// СЦЕНА ПРЕДПРОСМОТРА ПРОСТРАНСТВА ANIMATION.
//
// ЗАЧЕМ ОТДЕЛЬНАЯ СЦЕНА. Раньше клип, граф и правка позы играли прямо на
// объекте уровня: окно графа клало свой граф и значения параметров в его
// настоящий AnimationComponent, клип свойств двигал настоящие объекты. Уйти
// из пространства, не вернув всё на место, значило оставить уровень в чужом
// состоянии, — а «вернуть на место» это ровно тот код, который однажды
// забывают.
//
// Здесь у пространства СВОЯ сцена: копия цели (CopySubtree — тот же путь, что у
// префабов и Ctrl+D), шахматный пол и своя камера. Всё, что делает пространство
// — играет клип, граф с параметрами, слои, правит позу кости, — делается с
// копией. Уровень меняют только явные действия: назначить клип, граф, слой,
// маску (они сохраняются с объектом) и ключи клипа (они сохраняются в файл).
//
// Проигрыватель — тот же sage::anim::UpdateAnimators, что в игре: второго
// проигрывателя нет, и то, что видно здесь, — то, что будет в игре.
// ---------------------------------------------------------------------------
class AnimStage {
public:
    enum class Source { Clip, Graph };

    void Shutdown();   // GPU-ресурсы превью — пока контекст жив

    // --- Цель ----------------------------------------------------------------
    // Объект уровня, с которым работает пространство (-1 — нет). Смена цели
    // пересобирает копию.
    void SetTarget(EditorHost& host, int objectId);
    int TargetId() const { return m_targetId; }
    // Копия цели в сцене предпросмотра (невалидна, если цели нет).
    GameObject StageRoot() { return m_scene->Get(m_rootId); }
    Scene& StageScene() { return *m_scene; }
    // Копия объекта уровня с этим номером (тот же путь в иерархии цели).
    entt::entity StageEntityOf(EditorHost& host, int realId);
    // Анимация копии (скелетная цель) и её скелет; nullptr — у цели скелета нет.
    AnimationComponent* StageAnim();
    const sage::anim::Skeleton* Skeleton();
    bool Skeletal() { return Skeleton() != nullptr; }

    // Кадр: сверить копию с целью и продвинуть проигрывание.
    void Tick(EditorHost& host, float dt);

    // --- Выбор ---------------------------------------------------------------
    // Кость скелета (-1 — нет) или объект копии (entt::null — нет); одно из двух.
    int SelectedBone() const { return m_bone; }
    entt::entity SelectedEntity() const { return m_entity; }
    void SelectBone(int bone) { m_bone = bone; m_entity = entt::null; }
    void SelectEntity(entt::entity e) { m_entity = e; m_bone = -1; }
    // Мировая матрица кости (с трансформом копии) — для гизмо и рисования.
    glm::mat4 BoneWorld(int bone);
    // Из пространства скелета в мир: трансформ сущности * настройки импорта.
    glm::mat4 SkeletonToWorld();
    // Локальные TRS кости в показанной позе — то, что ставится ключом.
    bool BoneLocal(int bone, glm::vec3& t, glm::quat& r, glm::vec3& s);
    // Поставить кости новую МИРОВУЮ матрицу (гизмо): правка ложится в
    // переопределения позы копии — клип и модель не трогаются.
    void SetBoneWorld(int bone, const glm::mat4& world, bool t, bool r, bool s);
    // Снять ручную позу (после ключа, при перемотке, смене клипа).
    void ClearPose();
    void ClearBonePose(int bone);
    bool HasPoseEdits() const;
    // Счётчик правок позы гизмо (кость или объект копии): таймлайн с
    // авто-ключом ставит ключ, увидев, что он сдвинулся.
    void NotePoseEdit() { ++m_poseEdits; }
    uint64_t PoseEdits() const { return m_poseEdits; }

    // --- Источник ------------------------------------------------------------
    Source Mode() const { return m_source; }
    void SetMode(Source s) {
        // Из графа назад к клипу: клип показан из памяти (правки таймлайна), а
        // не перечитан из файла — привязать его к копии заново.
        if (s == Source::Clip && m_source != Source::Clip) m_clipDirty = true;
        m_source = s;
    }
    // Клип, который показывает копия: путь (.sageanim) и данные (могут быть
    // правленными в памяти, ещё не сохранёнными). Пусто — клип объекта.
    void ShowClip(const std::string& path, const sage::anim::ClipAsset& clip, uint64_t revision);
    // Свой скелетный клип (editable) в assets/animations: пустой, длиной в
    // секунду. Путь в проекте; пусто — не записался (причина в строке статуса).
    static std::string CreateEditableClip(EditorHost& host, const std::string& baseName);
    void ShowModelClip(int index);
    const std::string& ClipPath() const { return m_clipPath; }
    int ModelClip() const { return m_modelClip; }
    float Time() const { return m_time; }
    void SetTime(float t) { m_time = t; }
    bool Playing() const { return m_playing; }
    void SetPlaying(bool p) { m_playing = p; }
    bool Loop() const { return m_loop; }
    void SetLoop(bool l) { m_loop = l; }
    float Speed() const { return m_speed; }
    void SetSpeed(float s) { m_speed = s; }
    float ClipDuration();
    // Кто ведёт время: таймлайн (бегунок, его воспроизведение) или сама сцена.
    void SetTimeDrivenExternally(bool on) { m_externalTime = on; }

    // --- Граф ----------------------------------------------------------------
    // Граф предпросмотра: из окна графа (в памяти, до сохранения) или файл цели.
    void SetGraph(std::shared_ptr<const sage::anim::AnimGraph> graph, const std::string& path);
    const sage::anim::AnimGraph* Graph() const { return m_graph.get(); }
    const std::string& GraphPath() const { return m_graphPath; }
    // Значения параметров — ТОЛЬКО для предпросмотра: в объект уровня не идут.
    float ParamValue(const std::string& name) const;
    void SetParamValue(const std::string& name, float v) { m_params[name] = v; }

    // --- Пол -----------------------------------------------------------------
    // Копия скелетной цели ставится на пол САМА: нижняя точка настоящей геометрии
    // (все подмеши в позе привязки, с ImportTransform и узлами копии) совмещается
    // с плоскостью Y = 0. Смещение считается ОДИН раз — при сборке копии и при
    // смене модели или её настроек импорта — и дальше не пересчитывается по
    // анимированной позе: иначе прыжок клипа возвращался бы на пол каждый кадр.
    // false — достоверных границ нет (у цели нет вершин): смещения нет, копия
    // осталась в начале координат, и выдавать это за посадку нельзя.
    bool FloorAligned() const { return m_floorAligned; }
    float FloorLift() const { return m_floorLift; }
    // Габарит геометрии копии в МИРЕ (robust — без аномальных частей, для
    // кадрирования). false — геометрии нет.
    bool GeometryBounds(glm::vec3& lo, glm::vec3& hi, bool robust);
    // Точка, за которой едет камера в режиме Follow: центр костей в ТЕКУЩЕЙ
    // позе (у цели без скелета — начало копии).
    glm::vec3 TrackedPoint();

    // --- Кадр ----------------------------------------------------------------
    // Камера: орбита вокруг фокуса, осмотр на месте и полёт.
    void Orbit(float dYaw, float dPitch);
    void Pan(float dx, float dy);
    void Zoom(float wheel);
    // Осмотр на месте (ПКМ): глаз стоит, поворачивается взгляд. Пиксели мыши.
    void Look(float dx, float dy);
    // Полёт (ПКМ + WASDQE): оси -1..1 по взгляду, вбок и вверх по миру; fast —
    // Shift. Глаз и фокус едут вместе, расстояние до фокуса не меняется.
    void Fly(float forward, float right, float up, float dt, bool fast);
    void ResetCamera();
    void FrameTarget();
    // Follow Target: фокус едет за моделью, пока она играет; угол и расстояние
    // остаются пользовательскими. Состояние предпросмотра, в сцену не пишется.
    bool Follow() const { return m_follow; }
    void SetFollow(bool on) { m_follow = on; m_followValid = false; }
    glm::vec3 Focus() const { return m_focus; }
    float Distance() const { return m_distance; }
    float Radius() const { return m_radius; }   // размер кадрированной модели
    glm::mat4 View() const;
    glm::mat4 Proj(float aspect) const;
    glm::vec3 Eye() const;
    uint64_t Render(int width, int height);

private:
    void Rebuild(EditorHost& host);
    void SyncFromTarget(EditorHost& host);
    void ApplySource(float dt);
    void EnsureFloor();
    int RootSkeletalEntity() const;
    void AlignToFloor();
    void UpdateFollow(float dt);

    std::unique_ptr<Scene> m_scene = std::make_unique<Scene>("animation-stage");
    AssetPreview m_preview;
    int m_targetId = -1;
    int m_rootId = -1;
    int m_animId = -1;            // объект копии с AnimationComponent (может быть потомком)
    int m_realAnimId = -1;        // объект уровня, чья анимация скопирована
    std::string m_targetMesh;     // путь модели цели, из которого собрана копия
    int m_floorId = -1;
    std::shared_ptr<Material> m_floorMat;

    uint64_t m_poseEdits = 0;
    int m_bone = -1;
    entt::entity m_entity = entt::null;

    Source m_source = Source::Clip;
    std::string m_clipPath;
    uint64_t m_clipRevision = 0;
    std::string m_pushedPath;     // последний клип, отданный таймлайном
    uint64_t m_pushedRevision = 0;
    bool m_clipDirty = false;
    sage::anim::ClipAsset m_clip;
    int m_modelClip = -1;
    float m_time = 0.0f;
    bool m_playing = false;
    bool m_loop = true;
    float m_speed = 1.0f;
    bool m_externalTime = false;

    std::shared_ptr<const sage::anim::AnimGraph> m_graph;
    std::string m_graphPath;
    bool m_graphDirty = false;
    std::unordered_map<std::string, float> m_params;

    // Посадка на пол: смещение копии и то, от чего оно посчитано. Модель и
    // матрица импорта — чтобы заметить смену модели или настроек, не сравнивая
    // файлы; ReTries — модель могла ещё не подгрузиться (границ нет пока).
    bool m_floorAligned = false;
    float m_floorLift = 0.0f;
    const void* m_alignModel = nullptr;
    glm::mat4 m_alignImport{1.0f};
    int m_alignRetries = 0;
    glm::vec3 m_alignTracked{0.0f};   // TrackedPoint в момент расчёта границ

    glm::vec3 m_focus{0.0f, 1.0f, 0.0f};
    float m_yaw = 30.0f, m_pitch = 15.0f, m_distance = 4.0f;
    float m_radius = 1.0f;
    float m_aspect = 1.6f;            // последний показанный кадр — для кадрирования
    bool m_follow = false;
    bool m_followValid = false;
    glm::vec3 m_followPoint{0.0f};    // сглаженная точка, за которой едет фокус
};
