#include "sage/physics/Ragdoll.h"

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/render/ResourceManager.h"

namespace sage::physics {

namespace {

// Описание одной кости: имя, локальный центр (× scale, + origin), капсула
// (r, hh), индекс родителя (−1 — корень) и параметры сустава с родителем.
struct BoneDef {
    const char* Name;
    glm::vec3 Center;        // относительно origin, до масштаба
    float Radius;
    float HalfHeight;
    int Parent;              // индекс в массиве костей; -1 — корень (без сустава)
    JointType Joint;
    glm::vec3 JointAnchor;   // точка сустава относительно origin, до масштаба
    glm::vec3 Axis;          // ось (для Hinge)
    float ConeAngle;         // Cone: полу-угол °
    float HingeMin, HingeMax; // Hinge: пределы °
};

// Гуманоид: таз-корень, торс/шея/плечи/бёдра на конусах, локти/колени на петлях.
const BoneDef kBones[] = {
    // name          center            r     hh    par  joint            anchor            axis        cone  hmin  hmax
    {"Pelvis",   {0.0f,  0.00f, 0}, 0.15f, 0.10f, -1, JointType::Point,{0,0,0},          {0,1,0},     0,    0,    0},
    {"Torso",    {0.0f,  0.45f, 0}, 0.18f, 0.18f,  0, JointType::Cone, {0.0f, 0.24f, 0}, {0,1,0},     30,   0,    0},
    {"Head",     {0.0f,  0.92f, 0}, 0.14f, 0.05f,  1, JointType::Cone, {0.0f, 0.74f, 0}, {0,1,0},     25,   0,    0},
    {"UpperArmL",{-0.34f,0.60f, 0}, 0.07f, 0.16f,  1, JointType::Cone, {-0.20f,0.64f, 0},{0,1,0},     60,   0,    0},
    {"UpperArmR",{ 0.34f,0.60f, 0}, 0.07f, 0.16f,  1, JointType::Cone, { 0.20f,0.64f, 0},{0,1,0},     60,   0,    0},
    {"ForearmL", {-0.34f,0.22f, 0}, 0.06f, 0.15f,  3, JointType::Hinge,{-0.34f,0.40f, 0},{0,0,1},     0,  -140,   0},
    {"ForearmR", { 0.34f,0.22f, 0}, 0.06f, 0.15f,  4, JointType::Hinge,{ 0.34f,0.40f, 0},{0,0,1},     0,    0,  140},
    {"ThighL",   {-0.15f,-0.50f,0}, 0.09f, 0.20f,  0, JointType::Cone, {-0.13f,-0.18f,0},{0,1,0},     45,   0,    0},
    {"ThighR",   { 0.15f,-0.50f,0}, 0.09f, 0.20f,  0, JointType::Cone, { 0.13f,-0.18f,0},{0,1,0},     45,   0,    0},
    {"ShinL",    {-0.15f,-1.00f,0}, 0.08f, 0.20f,  7, JointType::Hinge,{-0.15f,-0.76f,0},{1,0,0},     0,    0,  140},
    {"ShinR",    { 0.15f,-1.00f,0}, 0.08f, 0.20f,  8, JointType::Hinge,{ 0.15f,-0.76f,0},{1,0,0},     0,    0,  140},
};
constexpr int kBoneCount = (int)(sizeof(kBones) / sizeof(kBones[0]));

} // namespace

int BuildRagdoll(Scene& scene, const glm::vec3& origin, float scale) {
    float s = scale > 0.01f ? scale : 1.0f;
    int ids[kBoneCount] = {0};

    auto cylinder = ResourceManager::Instance().GetCylinder();

    for (int i = 0; i < kBoneCount; ++i) {
        const BoneDef& b = kBones[i];
        GameObject bone = scene.CreateObject(b.Name);
        glm::vec3 pos = origin + b.Center * s;
        bone.GetTransform().Position = pos;
        // Визуализация капсулы цилиндром: диаметр 2r, высота 2(hh+r).
        bone.GetTransform().Scale = {2.0f * b.Radius * s,
                                     (b.HalfHeight + b.Radius) * s,
                                     2.0f * b.Radius * s};

        MeshRendererComponent& mr = bone.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cylinder, ""};
        mr.MeshPtr = cylinder;
        mr.Color = {0.75f, 0.65f, 0.55f};

        // Тело + капсульный коллайдер (масштаб сущности учтётся в PhysicsScene,
        // поэтому здесь локальные размеры делим на scale сущности по осям).
        scene.Registry().emplace<RigidBodyComponent>(
            bone.Entity(), RigidBodyComponent{BodyType::Dynamic, 1.0f, 0.6f, 0.05f});
        ColliderComponent col;
        col.Shape = ShapeType::Capsule;
        // Transform.Scale выше не единичный — компенсируем, чтобы коллайдер вышел
        // радиусом b.Radius*s и полу-высотой b.HalfHeight*s в мире.
        glm::vec3 es = bone.GetTransform().Scale;
        float uniform = glm::max(es.x, glm::max(es.y, es.z));
        col.Radius = (b.Radius * s) / glm::max(uniform, 0.001f);
        col.HalfHeight = (b.HalfHeight * s) / glm::max(es.y, 0.001f);
        scene.Registry().emplace<ColliderComponent>(bone.Entity(), col);

        ids[i] = bone.Id();

        // Сустав с родителем (у корня — нет).
        if (b.Parent >= 0) {
            JointComponent jc;
            jc.Type = b.Joint;
            jc.TargetId = ids[b.Parent];
            // Anchor хранится как смещение от позиции ЭТОЙ сущности (мир).
            jc.Anchor = (origin + b.JointAnchor * s) - pos;
            jc.Axis = b.Axis;
            if (b.Joint == JointType::Hinge) {
                jc.UseLimits = true;
                jc.MinLimit = b.HingeMin;
                jc.MaxLimit = b.HingeMax;
            }
            jc.ConeHalfAngle = b.ConeAngle;
            scene.Registry().emplace<JointComponent>(bone.Entity(), jc);
        }
    }

    return ids[0]; // корень — таз
}

} // namespace sage::physics
