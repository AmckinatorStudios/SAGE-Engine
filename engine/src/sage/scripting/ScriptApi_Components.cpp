#include "ScriptEngine.h"

#include <algorithm>

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
        // Цвет объекта БЕЗ материала. Свечения и прозрачности здесь больше нет:
        // и то и другое — свойство поверхности, то есть материала (см.
        // RenderComponents.h), и второй их копии на экземпляре не бывает.
        "Color", &MeshRendererComponent::Color,
        // Куда объект НЕ попадает: рука на камере не должна ни бросать тень на
        // палубу, ни отражаться в воде (см. RenderComponents.h).
        "CastShadows", &MeshRendererComponent::CastShadows,
        "InReflections", &MeshRendererComponent::InReflections,
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

    // --- Материал -----------------------------------------------------------
    //
    // ЗАЧЕМ СКРИПТУ МАТЕРИАЛ ЦЕЛИКОМ, А НЕ ПОЛЯ НА РЕНДЕРЕРЕ. Металличность и
    // шероховатость на первый взгляд просятся в MeshRendererComponent рядом с
    // Color — и это была бы ошибка, которую потом не отыграть: это свойства
    // ПОВЕРХНОСТИ, а поверхность в движке одна на всех, кто ей покрашен. Держи
    // их на сущности — и тысяча плиток пола станет тысячей разных материалов,
    // то есть тысячей инстансных групп вместо одной.
    //
    // Поэтому скрипт получает сам материал: заводит его (sage.render.NewMaterial),
    // настраивает поля здесь и назначает нужным объектам. Правка видна всем
    // сразу — тем же способом, каким её видит редактор.
    m_lua.new_usertype<MaterialRender>("MaterialRender",
        // Отсечение граней числом: 0 — задние (обычный случай), 1 — передние,
        // 2 — не отсекать (двусторонний). Именами значений скрипт не пользуется
        // — sol не умеет отдавать enum class без отдельной таблицы, а заводить
        // её ради одного поля дороже, чем написать 2 в скрипте.
        "Cull", sol::property(
            [](const MaterialRender& r) { return (int)r.Cull; },
            [](MaterialRender& r, int v) {
                r.Cull = (CullFaces)std::clamp(v, 0, 2);
            }),
        "PlanarReflectivity", &MaterialRender::PlanarReflectivity,
        // Повтор текстуры по развёртке: без него картинка на большом объекте
        // растягивается, и пол приходится собирать из тысяч плиток-объектов.
        "UVScaleX", &MaterialRender::UVScaleX,
        "UVScaleY", &MaterialRender::UVScaleY
    );
    m_lua.new_usertype<Material>("Material",
        "Albedo", &Material::Albedo,
        "Metallic", &Material::Metallic,
        "Roughness", &Material::Roughness,
        "Opacity", &Material::Opacity,
        "Emissive", &Material::Emissive,
        "EmissiveStrength", &Material::EmissiveStrength,
        // Поведение рендера — вложенной таблицей, как и в C++: список полей
        // здесь растёт, и плоские дубликаты пришлось бы дописывать дважды.
        "Render", &Material::Render,
        // Карты. Присваивание пути ещё НЕ грузит текстуру: её подтягивает
        // sage.render.ResolveMaterialTextures — один вызов после того, как
        // проставлены все пути, вместо загрузки на каждое присваивание.
        "TexturePath", &Material::TexturePath,
        "NormalMapPath", &Material::NormalMapPath,
        "MetallicMapPath", &Material::MetallicMapPath,
        "RoughnessMapPath", &Material::RoughnessMapPath,
        "AOMapPath", &Material::AOMapPath,
        "EmissiveMap", &Material::EmissiveMap
    );
}

