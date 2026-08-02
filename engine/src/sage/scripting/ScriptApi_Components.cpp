#include "ScriptEngine.h"

#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// Перечисления и usertype'ы компонентов ECS
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterComponentTypes() {
    // --- Перечисления компонентов: даём скриптам именованные значения вместо
    // «магических» чисел (light.Kind = LightType.Spot, rb.Type = BodyType.Dynamic).
    // Зарегистрированный enum позволяет sol2 биндить и сами enum-поля компонентов
    // напрямую (&LightComponent::Kind и т.п. ниже). ---
    m_lua.new_enum<LightComponent::Type>("LightType", {
        {"Point", LightComponent::Type::Point},
        {"Spot",  LightComponent::Type::Spot},
    });
    m_lua.new_enum<sage::physics::BodyType>("BodyType", {
        {"Static",    sage::physics::BodyType::Static},
        {"Dynamic",   sage::physics::BodyType::Dynamic},
        {"Kinematic", sage::physics::BodyType::Kinematic},
    });
    m_lua.new_enum<sage::physics::ShapeType>("ColliderShape", {
        {"Box",     sage::physics::ShapeType::Box},
        {"Sphere",  sage::physics::ShapeType::Sphere},
        {"Capsule", sage::physics::ShapeType::Capsule},
    });
    m_lua.new_enum<sage::physics::JointType>("JointType", {
        {"Fixed",    sage::physics::JointType::Fixed},
        {"Point",    sage::physics::JointType::Point},
        {"Hinge",    sage::physics::JointType::Hinge},
        {"Slider",   sage::physics::JointType::Slider},
        {"Distance", sage::physics::JointType::Distance},
        {"Cone",     sage::physics::JointType::Cone},
    });

    // --- Компоненты как usertype'ы: скрипт читает/пишет ЛЮБОЕ поле любого
    // компонента (свет/камера/тело/коллайдер/эмиттер/рендерер/скрипт). Аксессоры
    // GetX/AddX на GameObject (ниже) отдают эти структуры ССЫЛКОЙ — правки идут
    // прямо в компонент сущности, то есть напрямую в состояние системы движка. ---
    m_lua.new_usertype<LightComponent>("LightComponent",
        "Kind", &LightComponent::Kind,
        "Color", &LightComponent::Color,
        "Intensity", &LightComponent::Intensity,
        "Range", &LightComponent::Range,
        "InnerConeDeg", &LightComponent::InnerConeDeg,
        "OuterConeDeg", &LightComponent::OuterConeDeg
    );
    m_lua.new_usertype<CameraComponent>("CameraComponent",
        "Fov", &CameraComponent::Fov,
        "NearClip", &CameraComponent::NearClip,
        "FarClip", &CameraComponent::FarClip,
        "Primary", &CameraComponent::Primary
    );
    m_lua.new_usertype<RigidBodyComponent>("RigidBodyComponent",
        "Type", &RigidBodyComponent::Type,
        "Mass", &RigidBodyComponent::Mass,
        "Friction", &RigidBodyComponent::Friction,
        "Restitution", &RigidBodyComponent::Restitution
    );
    m_lua.new_usertype<ColliderComponent>("ColliderComponent",
        "Shape", &ColliderComponent::Shape,
        "HalfExtents", &ColliderComponent::HalfExtents,
        "Radius", &ColliderComponent::Radius,
        "HalfHeight", &ColliderComponent::HalfHeight
    );
    m_lua.new_usertype<JointComponent>("JointComponent",
        "Type", &JointComponent::Type,
        "TargetId", &JointComponent::TargetId,
        "Anchor", &JointComponent::Anchor,
        "Axis", &JointComponent::Axis,
        "UseLimits", &JointComponent::UseLimits,
        "MinLimit", &JointComponent::MinLimit,
        "MaxLimit", &JointComponent::MaxLimit,
        "MinDistance", &JointComponent::MinDistance,
        "MaxDistance", &JointComponent::MaxDistance,
        "ConeHalfAngle", &JointComponent::ConeHalfAngle
    );
    m_lua.new_usertype<ParticleEmitterComponent>("ParticleEmitterComponent",
        "Config", &ParticleEmitterComponent::Config,
        "Active", &ParticleEmitterComponent::Active,
        "Continuous", &ParticleEmitterComponent::Continuous,
        "BurstCount", &ParticleEmitterComponent::BurstCount,
        "BurstInterval", &ParticleEmitterComponent::BurstInterval
    );
    m_lua.new_usertype<MeshRendererComponent>("MeshRendererComponent",
        "Color", &MeshRendererComponent::Color,
        "Opacity", &MeshRendererComponent::Opacity,
        // Свечение объекта: цвет и сила. Сила больше 1 даёт ореол (bloom) —
        // ровно это отличает светящуюся лампу от просто жёлтого куба.
        "Emissive", &MeshRendererComponent::Emissive,
        "EmissiveStrength", &MeshRendererComponent::EmissiveStrength,
        "MaterialPath", &MeshRendererComponent::MaterialPath
    );
    m_lua.new_usertype<DecalComponent>("DecalComponent",
        "AngleLimit", &DecalComponent::AngleLimitDeg,
        "Offset", &DecalComponent::Offset,
        // Dirty пишут, чтобы пересобрать наклейку после правки; Triangles
        // читают, чтобы отличить «не видно» от «не на что было лечь».
        "Dirty", &DecalComponent::Dirty,
        "Triangles", sol::readonly(&DecalComponent::Triangles)
    );
    m_lua.new_usertype<ScriptComponent>("ScriptComponent",
        "Path", &ScriptComponent::Path
    );

}

