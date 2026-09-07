#include "sage/scene/SceneReflect.h"

#include <algorithm>
#include <cstring>

#include <glm/glm.hpp>

#include "sage/anim/AnimationComponents.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/render/ParticleComponents.h"
#include "sage/render/ReflectionComponents.h"
#include "sage/gi/GIComponents.h"
#include "sage/net/NetComponents.h"
#include "sage/scene/SceneComponents.h"
#include "sage/scene/Transform.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/ui/scene/UIScene.h"

namespace sage::scene {

const Property* ComponentType::FindProp(std::string_view key) const {
    for (int i = 0; i < PropCount; ++i)
        if (Props[i].Key && key == Props[i].Key) return &Props[i];
    return nullptr;
}

// ============================================================================
//  Доступ к полю
// ============================================================================

int PropertyFloatCount(const Property& p) {
    switch (p.Type) {
        case Property::Kind::Bool:
        case Property::Kind::Int:
        case Property::Kind::Enum:
        case Property::Kind::Float: return 1;
        case Property::Kind::Vec2: return 2;
        case Property::Kind::Vec3:
        case Property::Kind::Color: return 3;
        default: return 0;
    }
}

bool PropertyGetFloat(const void* data, const Property& p, int component, float& out) {
    if (!data) return false;
    switch (p.Type) {
        case Property::Kind::Bool: out = FieldAs<bool>(data, p) ? 1.0f : 0.0f; return true;
        case Property::Kind::Int:
        case Property::Kind::Enum: out = (float)FieldAs<int>(data, p); return true;
        case Property::Kind::Float: out = FieldAs<float>(data, p); return true;
        case Property::Kind::Vec2: {
            const glm::vec2& v = FieldAs<glm::vec2>(data, p);
            if (component < 0 || component > 1) return false;
            out = v[component];
            return true;
        }
        case Property::Kind::Vec3:
        case Property::Kind::Color: {
            const glm::vec3& v = FieldAs<glm::vec3>(data, p);
            if (component < 0 || component > 2) return false;
            out = v[component];
            return true;
        }
        default: return false;
    }
}

bool PropertySetFloat(void* data, const Property& p, int component, float value) {
    if (!data || p.Editor == Property::Widget::ReadOnly) return false;
    // Границы применяются ЗДЕСЬ, а не в редакторе: они описаны у свойства, и
    // скрипт, задающий то же поле, обязан получить те же границы. Иначе
    // «интенсивность −5» приходит из кода и гаснет сцена.
    if (p.HasRange()) value = std::clamp(value, p.Min, p.Max);
    switch (p.Type) {
        case Property::Kind::Bool: FieldAs<bool>(data, p) = value > 0.5f; return true;
        case Property::Kind::Int:
        case Property::Kind::Enum: FieldAs<int>(data, p) = (int)value; return true;
        case Property::Kind::Float: FieldAs<float>(data, p) = value; return true;
        case Property::Kind::Vec2: {
            if (component < 0 || component > 1) return false;
            FieldAs<glm::vec2>(data, p)[component] = value;
            return true;
        }
        case Property::Kind::Vec3:
        case Property::Kind::Color: {
            if (component < 0 || component > 2) return false;
            FieldAs<glm::vec3>(data, p)[component] = value;
            return true;
        }
        default: return false;
    }
}

// ============================================================================
//  Таблицы свойств
// ============================================================================
//
// Таблица — ЕДИНСТВЕННОЕ место, где записано, из чего компонент состоит для
// человека. Границы, подписи и подгруппы стоят здесь же и потому одинаковы
// везде, где поле показывают.
namespace {

using K = Property::Kind;
using W = Property::Widget;

// Перечисления. Имена — те же слова, что в коде: человек, читающий и файл
// сцены, и инспектор, обязан видеть одно и то же.
const char* const kProjectionNames[] = {"Perspective", "Orthographic"};
const char* const kLightNames[] = {"Point", "Spot", "Directional"};
const char* const kMeshNames[] = {"None", "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Model"};
const char* const kBodyNames[] = {"Static", "Dynamic", "Kinematic"};
const char* const kShapeNames[] = {"Box", "Sphere", "Capsule"};
const char* const kJointNames[] = {"Fixed", "Point", "Hinge", "Slider", "Distance", "Cone"};

#define PROP(Type, Field) SAGE_FIELD_OFFSET(Type, Field)

const Property kTransform[] = {
    {"position", SAGE_TEXT("Position"), K::Vec3, PROP(Transform, Position)},
    {"rotation", SAGE_TEXT("Rotation"), K::Vec3, PROP(Transform, Rotation), 0.0f, 0.0f,
     nullptr, 0, W::Angle},
    {"scale", SAGE_TEXT("Scale"), K::Vec3, PROP(Transform, Scale)},
};

const Property kMeshRenderer[] = {
    {"mesh", SAGE_TEXT("Mesh"), K::Enum, PROP(MeshRendererComponent, Ref) +
                                             PROP(MeshRef, type),
     0.0f, 0.0f, kMeshNames, 7},
    {"meshPath", SAGE_TEXT("Model"), K::String,
     PROP(MeshRendererComponent, Ref) + PROP(MeshRef, path), 0.0f, 0.0f, nullptr, 0, W::Asset,
     nullptr, nullptr, ".gltf,.glb,.obj,.fbx"},
    {"material", SAGE_TEXT("Material"), K::String, PROP(MeshRendererComponent, MaterialPath),
     0.0f, 0.0f, nullptr, 0, W::Asset, nullptr, nullptr, ".sagemat"},
    // Поправки экземпляра: они МОДУЛИРУЮТ материал, а не заменяют его, — и
    // подгруппа названа так, чтобы это было видно без чтения README.
    {"color", SAGE_TEXT("Tint"), K::Color, PROP(MeshRendererComponent, Color), 0.0f, 0.0f,
     nullptr, 0, W::Auto, SAGE_TEXT("Instance")},
    {"opacity", SAGE_TEXT("Opacity"), K::Float, PROP(MeshRendererComponent, Opacity), 0.0f,
     1.0f, nullptr, 0, W::Slider, SAGE_TEXT("Instance")},
    {"emissive", SAGE_TEXT("Emissive"), K::Color, PROP(MeshRendererComponent, Emissive), 0.0f,
     0.0f, nullptr, 0, W::Auto, SAGE_TEXT("Instance")},
    {"emissiveStrength", SAGE_TEXT("Emissive Strength"), K::Float,
     PROP(MeshRendererComponent, EmissiveStrength), 0.0f, 20.0f, nullptr, 0, W::Slider,
     SAGE_TEXT("Instance")},
    {"castShadows", SAGE_TEXT("Cast Shadows"), K::Bool,
     PROP(MeshRendererComponent, CastShadows), 0.0f, 0.0f, nullptr, 0, W::Auto,
     SAGE_TEXT("Visibility")},
    {"inReflections", SAGE_TEXT("In Reflections"), K::Bool,
     PROP(MeshRendererComponent, InReflections), 0.0f, 0.0f, nullptr, 0, W::Auto,
     SAGE_TEXT("Visibility")},
};

const Property kCamera[] = {
    {"mode", SAGE_TEXT("Projection"), K::Enum, PROP(CameraComponent, Mode), 0.0f, 0.0f,
     kProjectionNames, 2},
    {"fov", SAGE_TEXT("Field of View"), K::Float, PROP(CameraComponent, Fov), 1.0f, 179.0f,
     nullptr, 0, W::Slider},
    {"orthoHeight", SAGE_TEXT("Ortho Height"), K::Float, PROP(CameraComponent, OrthoHeight),
     0.01f, 500.0f},
    {"near", SAGE_TEXT("Near Clip"), K::Float, PROP(CameraComponent, NearClip), 0.001f, 100.0f},
    {"far", SAGE_TEXT("Far Clip"), K::Float, PROP(CameraComponent, FarClip), 0.1f, 10000.0f},
    {"primary", SAGE_TEXT("Primary"), K::Bool, PROP(CameraComponent, Primary)},
};

const Property kLight[] = {
    {"kind", SAGE_TEXT("Type"), K::Enum, PROP(LightComponent, Kind), 0.0f, 0.0f, kLightNames, 3},
    {"color", SAGE_TEXT("Color"), K::Color, PROP(LightComponent, Color)},
    {"intensity", SAGE_TEXT("Intensity"), K::Float, PROP(LightComponent, Intensity), 0.0f,
     20.0f, nullptr, 0, W::Slider},
    {"range", SAGE_TEXT("Range"), K::Float, PROP(LightComponent, Range), 0.0f, 200.0f},
    {"innerCone", SAGE_TEXT("Inner Cone"), K::Float, PROP(LightComponent, InnerConeDeg), 0.0f,
     89.0f, nullptr, 0, W::Slider, SAGE_TEXT("Spot")},
    {"outerCone", SAGE_TEXT("Outer Cone"), K::Float, PROP(LightComponent, OuterConeDeg), 0.0f,
     90.0f, nullptr, 0, W::Slider, SAGE_TEXT("Spot")},
    {"castShadows", SAGE_TEXT("Cast Shadows"), K::Bool, PROP(LightComponent, CastShadows)},
};

const Property kScript[] = {
    {"path", SAGE_TEXT("Script"), K::String, PROP(ScriptComponent, Path), 0.0f, 0.0f, nullptr,
     0, W::Asset, nullptr, nullptr, ".lua"},
};

const Property kRigidBody[] = {
    {"type", SAGE_TEXT("Body Type"), K::Enum, PROP(RigidBodyComponent, Type), 0.0f, 0.0f,
     kBodyNames, 3},
    {"mass", SAGE_TEXT("Mass"), K::Float, PROP(RigidBodyComponent, Mass), 0.0f, 1000.0f},
    {"friction", SAGE_TEXT("Friction"), K::Float, PROP(RigidBodyComponent, Friction), 0.0f,
     1.0f, nullptr, 0, W::Slider},
    {"restitution", SAGE_TEXT("Restitution"), K::Float, PROP(RigidBodyComponent, Restitution),
     0.0f, 1.0f, nullptr, 0, W::Slider},
    {"sensor", SAGE_TEXT("Sensor"), K::Bool, PROP(RigidBodyComponent, Sensor)},
};

const Property kCollider[] = {
    {"shape", SAGE_TEXT("Shape"), K::Enum, PROP(ColliderComponent, Shape), 0.0f, 0.0f,
     kShapeNames, 3},
    {"halfExtents", SAGE_TEXT("Half Extents"), K::Vec3, PROP(ColliderComponent, HalfExtents)},
    {"radius", SAGE_TEXT("Radius"), K::Float, PROP(ColliderComponent, Radius), 0.0f, 100.0f},
    {"halfHeight", SAGE_TEXT("Half Height"), K::Float, PROP(ColliderComponent, HalfHeight),
     0.0f, 100.0f},
};

const Property kAnimated[] = {
    {"path", SAGE_TEXT("Model"), K::String, PROP(AnimatedModelComponent, Path), 0.0f, 0.0f,
     nullptr, 0, W::Asset, nullptr, nullptr, ".gltf,.glb"},
    {"clip", SAGE_TEXT("Clip"), K::Int, PROP(AnimatedModelComponent, Clip), 0.0f, 64.0f},
    {"speed", SAGE_TEXT("Speed"), K::Float, PROP(AnimatedModelComponent, Speed), -4.0f, 4.0f,
     nullptr, 0, W::Slider},
    {"loop", SAGE_TEXT("Loop"), K::Bool, PROP(AnimatedModelComponent, Loop)},
    {"playing", SAGE_TEXT("Playing"), K::Bool, PROP(AnimatedModelComponent, Playing)},
    {"blendTime", SAGE_TEXT("Blend Time"), K::Float, PROP(AnimatedModelComponent, BlendTime),
     0.0f, 2.0f, nullptr, 0, W::Slider},
    {"rootMotion", SAGE_TEXT("Root Motion"), K::Bool, PROP(AnimatedModelComponent, RootMotion)},
};

const Property kParticles[] = {
    {"active", SAGE_TEXT("Active"), K::Bool, PROP(ParticleEmitterComponent, Active)},
    {"continuous", SAGE_TEXT("Continuous"), K::Bool, PROP(ParticleEmitterComponent, Continuous)},
    {"burstCount", SAGE_TEXT("Burst Count"), K::Int, PROP(ParticleEmitterComponent, BurstCount),
     0.0f, 4096.0f},
    {"burstInterval", SAGE_TEXT("Burst Interval"), K::Float,
     PROP(ParticleEmitterComponent, BurstInterval), 0.0f, 60.0f},
};

const Property kProbe[] = {
    {"resolution", SAGE_TEXT("Resolution"), K::Int, PROP(ReflectionProbeComponent, Resolution),
     16.0f, 1024.0f},
    {"boxHalfExtents", SAGE_TEXT("Box Half Extents"), K::Vec3,
     PROP(ReflectionProbeComponent, BoxHalfExtents)},
    {"intensity", SAGE_TEXT("Intensity"), K::Float, PROP(ReflectionProbeComponent, Intensity),
     0.0f, 4.0f, nullptr, 0, W::Slider},
    {"boxParallax", SAGE_TEXT("Box Parallax"), K::Bool,
     PROP(ReflectionProbeComponent, BoxParallax)},
    {"farClip", SAGE_TEXT("Far Clip"), K::Float, PROP(ReflectionProbeComponent, FarClip), 0.1f,
     10000.0f},
    {"realtime", SAGE_TEXT("Realtime"), K::Bool, PROP(ReflectionProbeComponent, Realtime)},
};

const Property kUIDocument[] = {
    {"document", SAGE_TEXT("Document"), K::String,
     PROP(sage::ui::UIDocumentComponent, Path), 0.0f, 0.0f, nullptr, 0, W::Asset, nullptr,
     nullptr, ".uidoc"},
    {"sortOrder", SAGE_TEXT("Sort Order"), K::Int,
     PROP(sage::ui::UIDocumentComponent, SortOrder), -1000.0f, 1000.0f},
    {"interactive", SAGE_TEXT("Interactive"), K::Bool,
     PROP(sage::ui::UIDocumentComponent, Interactive)},
    {"visible", SAGE_TEXT("Visible"), K::Bool, PROP(sage::ui::UIDocumentComponent, Visible)},
};

const Property kCharacter[] = {
    {"radius", SAGE_TEXT("Radius"), K::Float, PROP(CharacterControllerComponent, Radius), 0.01f,
     10.0f},
    {"height", SAGE_TEXT("Height"), K::Float, PROP(CharacterControllerComponent, Height), 0.1f,
     10.0f},
    {"stepHeight", SAGE_TEXT("Step Height"), K::Float,
     PROP(CharacterControllerComponent, StepHeight), 0.0f, 5.0f},
    {"maxSlope", SAGE_TEXT("Max Slope"), K::Float,
     PROP(CharacterControllerComponent, MaxSlopeDeg), 0.0f, 89.0f, nullptr, 0, W::Slider},
    {"mass", SAGE_TEXT("Mass"), K::Float, PROP(CharacterControllerComponent, Mass), 0.0f,
     1000.0f},
};

const Property kJoint[] = {
    {"type", SAGE_TEXT("Type"), K::Enum, PROP(JointComponent, Type), 0.0f, 0.0f, kJointNames, 6},
    {"targetId", SAGE_TEXT("Target Id"), K::Int, PROP(JointComponent, TargetId), -1.0f,
     1000000.0f},
    {"anchor", SAGE_TEXT("Anchor"), K::Vec3, PROP(JointComponent, Anchor)},
    {"axis", SAGE_TEXT("Axis"), K::Vec3, PROP(JointComponent, Axis)},
    {"useLimits", SAGE_TEXT("Use Limits"), K::Bool, PROP(JointComponent, UseLimits),
     0.0f, 0.0f, nullptr, 0, W::Auto, SAGE_TEXT("Limits")},
    {"minLimit", SAGE_TEXT("Min"), K::Float, PROP(JointComponent, MinLimit), 0.0f, 0.0f,
     nullptr, 0, W::Auto, SAGE_TEXT("Limits")},
    {"maxLimit", SAGE_TEXT("Max"), K::Float, PROP(JointComponent, MaxLimit), 0.0f, 0.0f,
     nullptr, 0, W::Auto, SAGE_TEXT("Limits")},
    {"minDistance", SAGE_TEXT("Min Distance"), K::Float, PROP(JointComponent, MinDistance),
     0.0f, 0.0f, nullptr, 0, W::Auto, SAGE_TEXT("Limits")},
    {"maxDistance", SAGE_TEXT("Max Distance"), K::Float, PROP(JointComponent, MaxDistance),
     0.0f, 0.0f, nullptr, 0, W::Auto, SAGE_TEXT("Limits")},
    {"coneHalfAngle", SAGE_TEXT("Cone Half Angle"), K::Float,
     PROP(JointComponent, ConeHalfAngle), 0.0f, 179.0f, nullptr, 0, W::Slider,
     SAGE_TEXT("Limits")},
};

const Property kDecal[] = {
    {"angleLimit", SAGE_TEXT("Angle Limit"), K::Float, PROP(DecalComponent, AngleLimitDeg),
     0.0f, 179.0f, nullptr, 0, W::Slider},
    {"offset", SAGE_TEXT("Offset"), K::Float, PROP(DecalComponent, Offset), 0.0f, 1.0f},
    {"triangles", SAGE_TEXT("Triangles"), K::Int, PROP(DecalComponent, Triangles), 0.0f, 0.0f,
     nullptr, 0, W::ReadOnly, nullptr,
     SAGE_TEXT("Zero means the decal did not land on anything")},
};

const Property kGIStatic[] = {
    {"lightmapped", SAGE_TEXT("Lightmapped"), K::Bool, PROP(GIStaticComponent, Lightmapped)},
    {"texelScale", SAGE_TEXT("Texel Scale"), K::Float, PROP(GIStaticComponent, TexelScale),
     0.01f, 16.0f},
};

const Property kAnimIK[] = {
    {"enabled", SAGE_TEXT("Enabled"), K::Bool, PROP(IKComponent, Enabled)},
};

const Property kNet[] = {
    {"placeholder", SAGE_TEXT("Replicated"), K::Bool,
     PROP(NetReplicatedComponent, Placeholder)},
};

#undef PROP

// Четыре действия над компонентом, которые нельзя выразить данными. Пишутся
// шаблоном один раз: двадцать одинаковых лямбд руками — двадцать возможностей
// опечататься в имени типа.
template <class T>
ComponentType Make(const char* id, const char* label, const char* icon, const Property* props,
                   int count, bool essential = false) {
    ComponentType t;
    t.Id = id;
    t.Label = label;
    t.Icon = icon;
    t.Props = props;
    t.PropCount = count;
    t.Essential = essential;
    t.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<T>(e); };
    t.Data = [](entt::registry& r, entt::entity e) -> void* {
        return static_cast<void*>(r.try_get<T>(e));
    };
    t.Add = [](entt::registry& r, entt::entity e) { r.emplace_or_replace<T>(e); };
    t.Remove = [](entt::registry& r, entt::entity e) { r.remove<T>(e); };
    return t;
}

} // namespace

// ============================================================================
//  Реестр
// ============================================================================

ComponentRegistry& ComponentRegistry::Instance() {
    static ComponentRegistry* instance = new ComponentRegistry();
    return *instance;
}

ComponentRegistry::ComponentRegistry() { RegisterBuiltinComponents(*this); }

void ComponentRegistry::Register(const ComponentType& type) {
    for (ComponentType& t : m_types)
        if (t.Id && type.Id && std::strcmp(t.Id, type.Id) == 0) {
            t = type;   // повторная регистрация заменяет: так плагин уточняет своё
            return;
        }
    m_types.push_back(type);
}

const ComponentType* ComponentRegistry::Find(std::string_view id) const {
    for (const ComponentType& t : m_types)
        if (t.Id && id == t.Id) return &t;
    return nullptr;
}

std::vector<const ComponentType*> ComponentRegistry::Of(entt::registry& reg,
                                                        entt::entity e) const {
    std::vector<const ComponentType*> out;
    if (!reg.valid(e)) return out;
    for (const ComponentType& t : m_types)
        if (t.Has && t.Has(reg, e)) out.push_back(&t);
    return out;
}

void RegisterBuiltinComponents(ComponentRegistry& r) {
    // Порядок = порядок в инспекторе. Сначала то, что есть у всех и отвечает на
    // вопрос «где объект», потом «как выглядит», потом поведение.
    r.Register(Make<Transform>("Transform", SAGE_TEXT("Transform"), "Move", kTransform,
                               (int)std::size(kTransform), /*essential=*/true));
    r.Register(Make<MeshRendererComponent>("MeshRenderer", SAGE_TEXT("Mesh Renderer"), "Mesh",
                                           kMeshRenderer, (int)std::size(kMeshRenderer)));
    r.Register(Make<CameraComponent>("Camera", SAGE_TEXT("Camera"), "Camera", kCamera,
                                     (int)std::size(kCamera)));
    r.Register(Make<LightComponent>("Light", SAGE_TEXT("Light"), "Light", kLight,
                                    (int)std::size(kLight)));
    r.Register(Make<ScriptComponent>("Script", SAGE_TEXT("Script"), "Script", kScript,
                                     (int)std::size(kScript)));
    r.Register(Make<RigidBodyComponent>("RigidBody", SAGE_TEXT("Rigid Body"), "Physics",
                                        kRigidBody, (int)std::size(kRigidBody)));
    r.Register(Make<ColliderComponent>("Collider", SAGE_TEXT("Collider"), "Cube", kCollider,
                                       (int)std::size(kCollider)));
    r.Register(Make<AnimatedModelComponent>("AnimatedModel", SAGE_TEXT("Animated Model"),
                                            "Bone", kAnimated, (int)std::size(kAnimated)));
    r.Register(Make<ParticleEmitterComponent>("Particles", SAGE_TEXT("Particle Emitter"),
                                              "Particles", kParticles,
                                              (int)std::size(kParticles)));
    r.Register(Make<ReflectionProbeComponent>("ReflectionProbe", SAGE_TEXT("Reflection Probe"),
                                              "Sphere", kProbe, (int)std::size(kProbe)));
    r.Register(Make<CharacterControllerComponent>("CharacterController",
                                                  SAGE_TEXT("Character Controller"), "Hand",
                                                  kCharacter, (int)std::size(kCharacter)));
    r.Register(Make<JointComponent>("Joint", SAGE_TEXT("Joint"), "Link", kJoint,
                                    (int)std::size(kJoint)));
    r.Register(Make<DecalComponent>("Decal", SAGE_TEXT("Decal"), "Pin", kDecal,
                                    (int)std::size(kDecal)));
    r.Register(Make<GIStaticComponent>("GIStatic", SAGE_TEXT("GI Static"), "Light", kGIStatic,
                                       (int)std::size(kGIStatic)));
    r.Register(Make<IKComponent>("IK", SAGE_TEXT("IK"), "Bone", kAnimIK,
                                 (int)std::size(kAnimIK)));
    r.Register(Make<NetReplicatedComponent>("NetReplicated", SAGE_TEXT("Net Replicated"),
                                            "Link", kNet, (int)std::size(kNet)));
    r.Register(Make<sage::ui::UIDocumentComponent>("UIDocument", SAGE_TEXT("UI Document"),
                                                   "Panel", kUIDocument,
                                                   (int)std::size(kUIDocument)));
}

} // namespace sage::scene
