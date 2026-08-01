#include "ScriptEngine.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/physics/Ragdoll.h"
#include "sage/ui/UIShowcase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/core/KeyNames.h"
#include <algorithm>
#include <cctype>
#include <optional>

namespace {
// Навешивает на usertype GameObject единый набор аксессоров к компоненту C:
//   entity:HasX()    -> bool  (есть ли компонент)
//   entity:GetX()    -> C | nil (Unity-семантика: nil, если компонента нет)
//   entity:AddX()    -> C     (создаёт при отсутствии и отдаёт ссылкой для правки)
//   entity:RemoveX()          (снимает компонент)
// Так один шаблон закрывает Light/Camera/RigidBody/Collider/ParticleEmitter/…
// одинаковым, предсказуемым API — скрипт правит ЛЮБОЙ компонент ЛЮБОЙ сущности
// (в т.ч. чужой), то есть общается со всеми системами движка через ECS.
template <typename C>
void BindComponentAccessors(sol::usertype<GameObject>& t, const char* has,
                            const char* get, const char* add, const char* remove) {
    t[has] = [](GameObject& o) {
        return o.Valid() && o.Registry()->all_of<C>(o.Entity());
    };
    t[get] = [](GameObject& o) -> C* {
        return o.Valid() ? o.Registry()->try_get<C>(o.Entity()) : nullptr;
    };
    t[add] = [](GameObject& o) -> C& {
        if (!o.Valid()) throw std::runtime_error("Добавление компонента: невалидная сущность");
        return o.Registry()->get_or_emplace<C>(o.Entity());
    };
    t[remove] = [](GameObject& o) {
        if (o.Valid()) o.Registry()->remove<C>(o.Entity());
    };
}
} // namespace

ScriptEngine::ScriptEngine() {
    // package открыт ради require: игра крупнее одного экрана кода должна
    // раскладываться по модулям, а общий код (воксельное ядро, утилиты)
    // писаться один раз, а не копироваться в каждый скрипт сущности. io/os
    // по-прежнему закрыты — скриптам игры незачем читать диск и звать процессы.
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::coroutine, sol::lib::package);
    // Пути поиска модулей задаёт ХОСТ через AddScriptSearchPath (папка скриптов
    // проекта). Дефолтный package.path указывает в системные каталоги Lua,
    // которых у игрока нет, и только мешает диагностике — обнуляем. cpath
    // обнуляем насовсем: подгрузка нативных .so/.dll из Lua игре не нужна.
    m_lua["package"]["path"] = "";
    m_lua["package"]["cpath"] = "";
    RegisterEngineApi();
}

void ScriptEngine::AddScriptSearchPath(const std::string& dir) {
    if (dir.empty()) return;
    std::string pattern = dir;
    if (pattern.back() != '/' && pattern.back() != '\\') pattern += '/';
    // <dir>/foo.lua и <dir>/foo/init.lua — обе привычные раскладки модуля.
    std::string entry = pattern + "?.lua;" + pattern + "?/init.lua";
    std::string current = m_lua["package"]["path"];
    m_lua["package"]["path"] = current.empty() ? entry : current + ";" + entry;
}

void ScriptEngine::RegisterEngineApi() {
    // Тонкий диспетчер: по одному вызову на связную группу привязок. Порядок —
    // как в прежней монолитной функции (типы прежде их потребителей), поэтому
    // поведение идентично, но каждую область теперь видно/править отдельно.
    RegisterMathTypes();
    RegisterComponentTypes();
    RegisterGameObject();
    RegisterSceneApi();
    RegisterMeshApi();
    RegisterInputApi();
    RegisterCameraApi();
    RegisterParticleApi();
    RegisterBillboardApi();
    RegisterAudioApi();
    RegisterTimerApi();
    RegisterMessagingApi();
    RegisterLaunchArgsApi();
    RegisterMathHelpers();
    RegisterLightingApi();
    RegisterPhysicsApi();
}

void ScriptEngine::RegisterMathTypes() {
    // glm::vec3 — доступен из Lua как обычная таблица с полями x/y/z, плюс
    // арифметика (+, -, унарный минус, умножение/деление на число) и пара
    // геометрических хелперов — без этого любая игровая математика (движение,
    // направления, дистанции) была бы мучением через отдельные x/y/z-поля.
    // ВАЖНО: sol::constructors регистрирует только Vec3.new(...) — вызов
    // Vec3(...) как функции требует ОТДЕЛЬНОЙ регистрации call_constructor
    // (без него Lua падает с «attempt to call a table value»; найдено боевым
    // тестом games/testgame — прежние скрипты векторы не конструировали).
    m_lua.new_usertype<glm::vec3>("Vec3",
        sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
        "x", &glm::vec3::x,
        "y", &glm::vec3::y,
        "z", &glm::vec3::z,
        sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
        sol::meta_function::multiplication, [](const glm::vec3& a, float s) { return a * s; },
        sol::meta_function::division, [](const glm::vec3& a, float s) { return a / s; },
        sol::meta_function::to_string, [](const glm::vec3& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", " + std::to_string(a.z) + ")";
        },
        "Length", [](const glm::vec3& a) { return glm::length(a); },
        "Normalized", [](const glm::vec3& a) {
            float len = glm::length(a);
            return len > 0.0001f ? a / len : a;
        },
        "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); },
        "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
        "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); }
    );

    // Vec2 — размеры билбордов (Size) и прочая 2D-математика (экранные
    // координаты, UV). Минимальный набор — арифметика та же, что у Vec3.
    m_lua.new_usertype<glm::vec2>("Vec2",
        sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        "x", &glm::vec2::x,
        "y", &glm::vec2::y,
        sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
        sol::meta_function::multiplication, [](const glm::vec2& a, float s) { return a * s; },
        sol::meta_function::to_string, [](const glm::vec2& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ")";
        }
    );

    // Vec4 — цвета с альфа-каналом (частицы, тонирование билбордов). Здесь
    // намеренно нет геометрических операций (Length/Normalized/Dot) — Vec4
    // в этом движке используется только как rgba, не как направление/точка.
    m_lua.new_usertype<glm::vec4>("Vec4",
        sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
        sol::call_constructor, sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
        "x", &glm::vec4::x,
        "y", &glm::vec4::y,
        "z", &glm::vec4::z,
        "w", &glm::vec4::w,
        sol::meta_function::to_string, [](const glm::vec4& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", "
                 + std::to_string(a.z) + ", " + std::to_string(a.w) + ")";
        }
    );

    // Transform — позиция/поворот/масштаб объекта, доступны на чтение и запись
    m_lua.new_usertype<::Transform>("Transform",
        "Position", &::Transform::Position,
        "Rotation", &::Transform::Rotation,
        "Scale", &::Transform::Scale
    );
}

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
        "MaterialPath", &MeshRendererComponent::MaterialPath
    );
    m_lua.new_usertype<ScriptComponent>("ScriptComponent",
        "Path", &ScriptComponent::Path
    );

    // --- UI: интерфейс сцены (см. UIElementComponent в Components.h).
    // Скрипты правят текст/значение/видимость прямо в компоненте — так худ
    // (здоровье, счёт, таймеры) обновляется из игровой логики. ---
    m_lua.new_enum<UIElementComponent::Kind>("UIKind", {
        {"Panel", UIElementComponent::Kind::Panel},
        {"Label", UIElementComponent::Kind::Label},
        {"Image", UIElementComponent::Kind::Image},
        {"Bar",   UIElementComponent::Kind::Bar},
    });
    m_lua.new_enum<UIAnchor>("UIAnchor", {
        {"TopLeft", UIAnchor::TopLeft},       {"TopCenter", UIAnchor::TopCenter},
        {"TopRight", UIAnchor::TopRight},     {"CenterLeft", UIAnchor::CenterLeft},
        {"Center", UIAnchor::Center},         {"CenterRight", UIAnchor::CenterRight},
        {"BottomLeft", UIAnchor::BottomLeft}, {"BottomCenter", UIAnchor::BottomCenter},
        {"BottomRight", UIAnchor::BottomRight},
    });
    m_lua.new_usertype<UIElementComponent>("UIElementComponent",
        "Type", &UIElementComponent::Type,
        "Anchor", &UIElementComponent::Anchor,
        "Offset", &UIElementComponent::Offset,
        "Size", &UIElementComponent::Size,
        "Layer", &UIElementComponent::Layer,
        "Visible", &UIElementComponent::Visible,
        "ClipChildren", &UIElementComponent::ClipChildren,
        "Color", &UIElementComponent::Color,
        "Rounding", &UIElementComponent::Rounding,
        "BorderThickness", &UIElementComponent::BorderThickness,
        "BorderColor", &UIElementComponent::BorderColor,
        "Text", &UIElementComponent::Text,
        "TextScale", &UIElementComponent::TextScale,
        "TextColor", &UIElementComponent::TextColor,
        "TextCentered", &UIElementComponent::TextCentered,
        "Value", &UIElementComponent::Value,
        "BarFillColor", &UIElementComponent::BarFillColor
    );

    // Собирает боевой интерфейс (инвентарь + дерево навыков) из UIElementComponent
    // в сцену — плотный реальный экран одним вызовом. Возвращает id корня.
    m_lua.set_function("SpawnUIShowcase", [this]() -> int {
        if (!m_scene) throw std::runtime_error("SpawnUIShowcase: сцена не привязана");
        return sage::ui::BuildShowcase(*m_scene);
    });

    // --- Твины (интерполяция значений) --------------------------------------
    // Плавно ведут поля сущности к цели за duration секунд по кривой ease.
    // Тикают со скриптами (UpdateAll). Возвращают id (для TweenCancel), сеттеры
    // проверяют Valid() — уничтоженная в полёте сущность безопасно пропускается.
    m_lua.new_enum<sage::Easing>("Ease", {
        {"Linear", sage::Easing::Linear},
        {"QuadIn", sage::Easing::QuadIn}, {"QuadOut", sage::Easing::QuadOut}, {"QuadInOut", sage::Easing::QuadInOut},
        {"CubicIn", sage::Easing::CubicIn}, {"CubicOut", sage::Easing::CubicOut}, {"CubicInOut", sage::Easing::CubicInOut},
        {"SineIn", sage::Easing::SineIn}, {"SineOut", sage::Easing::SineOut}, {"SineInOut", sage::Easing::SineInOut},
        {"ExpoOut", sage::Easing::ExpoOut}, {"BackOut", sage::Easing::BackOut},
        {"ElasticOut", sage::Easing::ElasticOut}, {"BounceOut", sage::Easing::BounceOut},
    });

    auto easeOr = [](sol::optional<sage::Easing> e, sage::Easing def) { return e.value_or(def); };

    m_lua.set_function("TweenMove", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                   sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Position, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Position = v; });
    });
    m_lua.set_function("TweenScale", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                    sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Scale, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Scale = v; });
    });
    m_lua.set_function("TweenRotate", [this, easeOr](GameObject obj, glm::vec3 toEulerDeg, float dur,
                                                     sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.GetTransform().Rotation, toEulerDeg, dur, easeOr(ease, sage::Easing::QuadInOut),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.GetTransform().Rotation = v; });
    });
    m_lua.set_function("TweenColor", [this, easeOr](GameObject obj, glm::vec3 to, float dur,
                                                    sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        return m_tweens.To<glm::vec3>(obj.ColorRef(), to, dur, easeOr(ease, sage::Easing::Linear),
            [obj](const glm::vec3& v) mutable { if (obj.Valid()) obj.ColorRef() = v; });
    });
    // Полоса/значение UI (0..1) — например плавная убыль здоровья.
    m_lua.set_function("TweenUIValue", [this, easeOr](GameObject obj, float to, float dur,
                                                      sol::optional<sage::Easing> ease) -> uint64_t {
        if (!obj.Valid()) return 0;
        auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity());
        if (!ui) return 0;
        return m_tweens.To<float>(ui->Value, to, dur, easeOr(ease, sage::Easing::QuadOut),
            [obj](const float& v) mutable {
                if (!obj.Valid()) return;
                if (auto* u = obj.Registry()->try_get<UIElementComponent>(obj.Entity())) u->Value = v;
            });
    });
    m_lua.set_function("TweenCancel", [this](uint64_t id) { m_tweens.Cancel(id); });
    m_lua.set_function("TweenCancelAll", [this]() { m_tweens.CancelAll(); });
    m_lua.set_function("ActiveTweens", [this]() { return m_tweens.ActiveCount(); });

    // Проиграть клип анимации на сущности с AnimatedModelComponent, плавно
    // перейдя за blend секунд (кросс-фейд; система анимации подхватит смену Clip).
    // false, если у сущности нет анимированной модели.
    m_lua.set_function("PlayAnimation", [](GameObject obj, int clip, sol::optional<float> blend) -> bool {
        if (!obj.Valid()) return false;
        auto* am = obj.Registry()->try_get<AnimatedModelComponent>(obj.Entity());
        if (!am) return false;
        am->Clip = clip;
        if (blend) am->BlendTime = *blend;
        am->Playing = true;
        return true;
    });
}

void ScriptEngine::RegisterGameObject() {
    // GameObject — то, что скрипт получает как "entity" в OnUpdate/OnStart,
    // или что возвращают SpawnObject/FindObject. Теперь это дескриптор поверх
    // ECS (см. Scene.h), поэтому поля выставлены через property-аксессоры к
    // компонентам сущности. Transform/Color отдаются ССЫЛКОЙ — чтобы прежний
    // код вида `entity.Transform.Rotation.y = ...` и `cube.Color.x = ...`
    // писал прямо в компонент; для Color есть и сеттер целого значения
    // (`cube.Color = Vec3.new(...)`), чтобы поведение совпало со старым
    // data-member-биндингом один в один.
    sol::usertype<GameObject> goType = m_lua.new_usertype<GameObject>("GameObject",
        "Id", sol::property([](GameObject& o) { return o.Id(); }),
        "Name", sol::property(
            [](GameObject& o) { return o.Name(); },
            [](GameObject& o, const std::string& n) { o.SetName(n); }),
        "Transform", sol::property([](GameObject& o) -> ::Transform& { return o.GetTransform(); }),
        "Color", sol::property(
            [](GameObject& o) -> glm::vec3& { return o.ColorRef(); },
            [](GameObject& o, const glm::vec3& c) { o.ColorRef() = c; }),
        "Valid", [](GameObject& o) { return o.Valid(); }
    );

    // Аксессоры компонентов (Has/Get/Add/Remove) — единый шаблон на каждый тип.
    BindComponentAccessors<LightComponent>(goType, "HasLight", "GetLight", "AddLight", "RemoveLight");
    BindComponentAccessors<CameraComponent>(goType, "HasCamera", "GetCamera", "AddCamera", "RemoveCamera");
    BindComponentAccessors<RigidBodyComponent>(goType, "HasRigidBody", "GetRigidBody", "AddRigidBody", "RemoveRigidBody");
    BindComponentAccessors<ColliderComponent>(goType, "HasCollider", "GetCollider", "AddCollider", "RemoveCollider");
    BindComponentAccessors<JointComponent>(goType, "HasJoint", "GetJoint", "AddJoint", "RemoveJoint");
    BindComponentAccessors<ParticleEmitterComponent>(goType, "HasEmitter", "GetEmitter", "AddEmitter", "RemoveEmitter");
    BindComponentAccessors<MeshRendererComponent>(goType, "HasRenderer", "GetRenderer", "AddRenderer", "RemoveRenderer");
    BindComponentAccessors<ScriptComponent>(goType, "HasScript", "GetScript", "AddScript", "RemoveScript");
    BindComponentAccessors<UIElementComponent>(goType, "HasUI", "GetUI", "AddUI", "RemoveUI");

    // --- Иерархия прямо на объекте: e:SetParent(p) / e:Parent() / e:Children() /
    // e:WorldPosition() / e:Destroy(). Всё маршрутизируется через Scene (циклы
    // предотвращаются, мировая матрица считается по цепочке родителей). ---
    goType["SetParent"] = [this](GameObject& child, GameObject& parent) {
        if (!m_scene) throw std::runtime_error("SetParent: сцена не привязана (BindScene не вызван)");
        if (child.Valid())
            m_scene->SetParent(child.Entity(), parent.Valid() ? parent.Entity() : entt::null);
    };
    goType["Unparent"] = [this](GameObject& child) {
        if (m_scene && child.Valid()) m_scene->SetParent(child.Entity(), entt::null);
    };
    goType["Parent"] = [this](GameObject& o) -> sol::optional<GameObject> {
        if (!m_scene || !o.Valid()) return sol::nullopt;
        entt::entity p = m_scene->ParentOf(o.Entity());
        if (p == entt::null) return sol::nullopt;
        return GameObject(&m_scene->Registry(), p);
    };
    goType["Children"] = [this](GameObject& o) -> sol::table {
        sol::table list = m_lua.create_table();
        if (m_scene && o.Valid()) {
            if (auto* h = m_scene->Registry().try_get<HierarchyComponent>(o.Entity())) {
                int i = 1;
                for (auto c : h->Children)
                    if (m_scene->Registry().valid(c)) list[i++] = GameObject(&m_scene->Registry(), c);
            }
        }
        return list;
    };
    goType["WorldPosition"] = [this](GameObject& o) -> glm::vec3 {
        if (!m_scene || !o.Valid()) return glm::vec3(0.0f);
        return glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    };
    goType["Destroy"] = [this](GameObject& o) {
        if (m_scene && o.Valid()) m_scene->RemoveObject(o.Id());
    };
}

void ScriptEngine::RegisterSceneApi() {
    // Простая функция логирования, чтобы скрипты могли печатать отладочную информацию
    m_lua.set_function("log", [](const std::string& message) {
        LOG_INFO("Lua") << message;
    });

    // --- Сцена: спавн/поиск/удаление объектов из Lua (доступно после BindScene) ---
    m_lua.set_function("SpawnObject", [this](const std::string& name) -> GameObject {
        if (!m_scene) throw std::runtime_error("SpawnObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        GameObject obj = m_scene->CreateObject(name);
        // Уровень Trace, а не Info, НАМЕРЕННО. Скрипт, порождающий мир (воксельный
        // ландшафт — тысячи блоков за кадр загрузки), давал по строке лога на
        // каждый блок: консоль редактора забивалась, а сам вывод стоил дороже
        // создания сущности. Разовые спавны по-прежнему видны — достаточно
        // опустить порог лога (см. Log.h).
        LOG_TRACE("Lua") << "Создан объект: " << name << " (id " << obj.Id() << ")";
        return obj;
    });

    m_lua.set_function("FindObject", [this](const std::string& name) -> sol::optional<GameObject> {
        if (!m_scene) return sol::nullopt;
        GameObject obj = m_scene->FindByName(name);
        if (!obj.Valid()) return sol::nullopt;
        return obj;
    });

    // Верхний видимый UI-элемент под экранной точкой (учитывает слои/маски):
    // GameObject или nil. Экранный размер передаётся явно — скрипт берёт его
    // из своего контекста (окно игры / панель Game).
    m_lua.set_function("GetUIElementAt",
        [this](float x, float y, int screenW, int screenH) -> sol::optional<GameObject> {
            if (!m_scene) return sol::nullopt;
            int id = sage::ui::HitTest(*m_scene, x, y, screenW, screenH);
            if (id < 0) return sol::nullopt;
            GameObject obj = m_scene->Get(id);
            if (!obj.Valid()) return sol::nullopt;
            return obj;
        });

    m_lua.set_function("DestroyObject", [this](int id) {
        if (!m_scene) throw std::runtime_error("DestroyObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        // RemoveObject уничтожает сущность в ECS-registry. Зависимые
        // ScriptInstance держат дескриптор {registry, entity}, а не сырой
        // указатель, поэтому после уничтожения их Object.Valid() станет false —
        // UpdateAll() безопасно пропустит и уберёт их после текущего прохода
        // (никакого use-after-free, даже если скрипт уничтожает сам себя).
        m_scene->RemoveObject(id);
    });
}

void ScriptEngine::RegisterMeshApi() {
    // Удобный хелпер: даёт заспавненному объекту видимый куб-меш, чтобы его
    // сразу было видно на сцене без ручной возни с MeshRef/ResourceManager
    m_lua.set_function("SetMeshCube", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCube();
    });

    // То же самое, но грузит .obj модель (кэшируется ResourceManager'ом —
    // одна и та же модель, запрошенная из нескольких скриптов, не
    // перечитывается с диска). Бросает ошибку, если файл не найден/битый.
    m_lua.set_function("SetMeshModel", [](GameObject& obj, const std::string& path) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Model, path};
        mr.MeshPtr = ResourceManager::Instance().GetModel(path);
        // GetModel при ошибке возвращает nullptr (чтобы загрузка СЦЕНЫ с битой
        // моделью не обрывалась) — но скрипту отдаём прежний контракт: ошибка.
        if (!mr.MeshPtr) throw std::runtime_error("SetMeshModel: модель не загрузилась: " + path);
    });

    // Остальные встроенные примитивы — как SetMeshCube, но для сферы/плоскости/
    // цилиндра/конуса. SetMeshNone убирает геометрию (пустышка/маркер/держатель
    // компонентов).
    m_lua.set_function("SetMeshSphere", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Sphere, ""};
        mr.MeshPtr = ResourceManager::Instance().GetSphere();
    });
    m_lua.set_function("SetMeshPlane", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Plane, ""};
        mr.MeshPtr = ResourceManager::Instance().GetPlane();
    });
    m_lua.set_function("SetMeshCylinder", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cylinder, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCylinder();
    });
    m_lua.set_function("SetMeshCone", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cone, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCone();
    });
    m_lua.set_function("SetMeshNone", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::None, ""};
        mr.MeshPtr.reset();
    });
}

void ScriptEngine::RegisterInputApi() {
    // --- Ввод: именованные действия движка (см. core/InputMap.h), доступно
    // после BindInput. Скрипты читают тот же ввод, что и C++-код игры —
    // никакого параллельного дублирования раскладки клавиш. ---
    m_lua.set_function("IsActionDown", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->IsDown(name);
    });
    m_lua.set_function("WasActionPressed", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasPressed(name);
    });
    m_lua.set_function("WasActionReleased", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasReleased(name);
    });
    m_lua.set_function("HasAction", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name);
    });

    // --- Раскладка ИЗ ИГРЫ: BindAction("Jump", "SPACE") / BindAction("Move
    // Forward", {"W", "UP"}). Раньше действия мог объявить только C++-код
    // хоста, поэтому игра целиком на скриптах не могла завести НИ ОДНОЙ своей
    // клавиши — весь ввод приходилось согласовывать через правку движка.
    // Имена клавиш — те же, что в файлах настроек (см. core/KeyNames.h):
    // "W", "SPACE", "LEFT_SHIFT", "MOUSE_LEFT", "MOUSE_RIGHT", ... Возвращает
    // число реально распознанных привязок (0 — имя клавиши не понято). ---
    auto bindOne = [this](const std::string& action, const std::string& key) -> bool {
        std::optional<InputBinding> binding = KeyNames::Parse(key);
        if (!binding) {
            LOG_WARN("Lua") << "BindAction('" << action << "'): неизвестная клавиша '" << key << "'";
            return false;
        }
        m_input->Register(action).Bind(*binding);
        return true;
    };
    m_lua.set_function("BindAction", [this, bindOne](const std::string& action, sol::object keys) -> int {
        if (!m_input)
            throw std::runtime_error("BindAction: ввод не привязан (ScriptEngine::BindInput не вызван)");
        int bound = 0;
        if (keys.is<std::string>()) {
            if (bindOne(action, keys.as<std::string>())) ++bound;
        } else if (keys.is<sol::table>()) {
            sol::table list = keys.as<sol::table>();
            for (size_t i = 1; i <= list.size(); ++i) {
                sol::optional<std::string> key = list[i];
                if (key && bindOne(action, *key)) ++bound;
            }
        } else {
            throw std::runtime_error("BindAction: вторым аргументом ожидается имя клавиши "
                                     "или таблица имён");
        }
        return bound;
    });

    // --- «Сырой» ввод: обзор от первого лица. Доступно после BindRawInput. ---
    m_lua.set_function("GetMouseDelta", [this]() -> glm::vec2 {
        if (!m_rawInput)
            throw std::runtime_error("GetMouseDelta: сырой ввод не привязан (BindRawInput не вызван)");
        return m_rawInput->MouseDelta();
    });
    m_lua.set_function("GetMousePosition", [this]() -> glm::vec2 {
        if (!m_rawInput)
            throw std::runtime_error("GetMousePosition: сырой ввод не привязан (BindRawInput не вызван)");
        return m_rawInput->MousePosition();
    });
    m_lua.set_function("GetScrollDelta", [this]() -> int {
        return m_rawInput ? m_rawInput->ScrollDelta() : 0;
    });
    m_lua.set_function("SetMouseCaptured", [this](bool captured) {
        if (!m_rawInput)
            throw std::runtime_error("SetMouseCaptured: сырой ввод не привязан (BindRawInput не вызван)");
        m_rawInput->SetMouseCaptured(captured);
    });
    m_lua.set_function("IsMouseCaptured", [this]() -> bool {
        return m_rawInput && m_rawInput->MouseCaptured();
    });
}

void ScriptEngine::RegisterCameraApi() {
    // --- Камера: доступно после BindCamera. Position — обычное поле, но
    // Yaw/Pitch выставлены через property-функции, которые сразу пересчитывают
    // Front/Right/Up вызовом ProcessMouse(0,0) — тем же приёмом, что уже
    // использует ApplyDebugEnvOverrides() в main.cpp после ручной правки угла.
    // Без этого камера "смотрела" бы в старом направлении до следующего
    // движения мыши игроком. ---
    m_lua.new_usertype<Camera>("Camera",
        "Position", &Camera::Position,
        "Front", sol::readonly(&Camera::Front),
        "Right", sol::readonly(&Camera::Right),
        "Up", sol::readonly(&Camera::Up),
        "Fov", &Camera::Fov,
        "MovementSpeed", &Camera::MovementSpeed,
        "NearClip", &Camera::NearClip,
        "FarClip", &Camera::FarClip,
        "Yaw", sol::property(
            [](const Camera& c) { return c.Yaw; },
            [](Camera& c, float yaw) { c.Yaw = yaw; c.ProcessMouse(0.0f, 0.0f); }),
        "Pitch", sol::property(
            [](const Camera& c) { return c.Pitch; },
            [](Camera& c, float pitch) { c.Pitch = pitch; c.ProcessMouse(0.0f, 0.0f); })
    );

    m_lua.set_function("GetCamera", [this]() -> Camera& {
        if (!m_camera) throw std::runtime_error("GetCamera: камера не привязана (ScriptEngine::BindCamera не вызван)");
        return *m_camera;
    });
}

void ScriptEngine::RegisterParticleApi() {
    // --- Частицы: доступно после BindParticles. ParticleConfig — те же поля,
    // что и ParticleEmitterConfig в C++ (см. render/Particle.h), плюс готовые
    // пресеты ParticlePresets.* (готовые визуальные рецепты — всплеск/дым/осколки) —
    // Lua-скрипт может взять пресет как основу и подправить пару полей,
    // вместо того чтобы описывать весь конфиг с нуля. ---
    m_lua.new_usertype<ParticleEmitterConfig>("ParticleConfig",
        sol::constructors<ParticleEmitterConfig()>(),
        "DirectionMin", &ParticleEmitterConfig::DirectionMin,
        "DirectionMax", &ParticleEmitterConfig::DirectionMax,
        "SpeedMin", &ParticleEmitterConfig::SpeedMin,
        "SpeedMax", &ParticleEmitterConfig::SpeedMax,
        "Gravity", &ParticleEmitterConfig::Gravity,
        "LifetimeMin", &ParticleEmitterConfig::LifetimeMin,
        "LifetimeMax", &ParticleEmitterConfig::LifetimeMax,
        "StartSizeMin", &ParticleEmitterConfig::StartSizeMin,
        "StartSizeMax", &ParticleEmitterConfig::StartSizeMax,
        "EndSizeMin", &ParticleEmitterConfig::EndSizeMin,
        "EndSizeMax", &ParticleEmitterConfig::EndSizeMax,
        "StartColor", &ParticleEmitterConfig::StartColor,
        "EndColor", &ParticleEmitterConfig::EndColor,
        "AngularVelocityMax", &ParticleEmitterConfig::AngularVelocityMax,
        "EmissionRate", &ParticleEmitterConfig::EmissionRate
    );

    sol::table presets = m_lua.create_table();
    presets.set_function("WaterSplash", &ParticlePresets::WaterSplash);
    presets.set_function("Smoke", &ParticlePresets::Smoke);
    presets.set_function("BlockBreak", &ParticlePresets::BlockBreak);
    presets.set_function("StoveEmbers", &ParticlePresets::StoveEmbers);
    m_lua["ParticlePresets"] = presets;

    // Разовый залп частиц в мировой точке — см. ParticleSystem::Burst
    m_lua.set_function("EmitParticles", [this](const ParticleEmitterConfig& config, glm::vec3 pos, int count) {
        if (!m_particles) throw std::runtime_error("EmitParticles: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->Burst(config, pos, count);
    });
    // Непрерывная струя (дым, искры) — создаётся выключенной, включай
    // SetParticleStreamActive(id, true) отдельно (см. ParticleSystem::CreateStream)
    m_lua.set_function("CreateParticleStream", [this](const std::string& id, const ParticleEmitterConfig& config, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("CreateParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->CreateStream(id, config, pos);
    });
    m_lua.set_function("SetParticleStreamActive", [this](const std::string& id, bool active) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamActive: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamActive(id, active);
    });
    m_lua.set_function("SetParticleStreamPosition", [this](const std::string& id, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamPosition: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamPosition(id, pos);
    });
    m_lua.set_function("RemoveParticleStream", [this](const std::string& id) {
        if (!m_particles) throw std::runtime_error("RemoveParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->RemoveStream(id);
    });
}

void ScriptEngine::RegisterBillboardApi() {
    // --- Билборды: доступно после BindBillboards. Спрайт без texturePath
    // рисуется сплошным цветом Tint (см. BillboardSprite) — удобно для
    // маркеров/индикаторов, для которых не хочется готовить текстуру. ---
    m_lua.set_function("AddBillboard", [this](glm::vec3 pos, glm::vec2 size, sol::optional<std::string> texturePath) -> int {
        if (!m_billboards) throw std::runtime_error("AddBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        BillboardSprite sprite;
        sprite.WorldPos = pos;
        sprite.Size = size;
        if (texturePath) sprite.SpriteTexture = GetOrLoadBillboardTexture(*texturePath);
        return m_billboards->Add(sprite);
    });
    m_lua.set_function("RemoveBillboard", [this](int id) {
        if (!m_billboards) throw std::runtime_error("RemoveBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->Remove(id);
    });
    m_lua.set_function("SetBillboardPosition", [this](int id, glm::vec3 pos) {
        if (!m_billboards) throw std::runtime_error("SetBillboardPosition: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetPosition(id, pos);
    });
    m_lua.set_function("SetBillboardVisible", [this](int id, bool visible) {
        if (!m_billboards) throw std::runtime_error("SetBillboardVisible: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetVisible(id, visible);
    });
    m_lua.set_function("SetBillboardTint", [this](int id, glm::vec4 tint) {
        if (!m_billboards) throw std::runtime_error("SetBillboardTint: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetTint(id, tint);
    });
}

void ScriptEngine::RegisterAudioApi() {
    // --- Звук: доступно после BindAudio. Скрипты (катсцены, события уровня)
    // могут проигрывать эффекты/музыку и рулить громкостью. Если аудио-
    // устройство недоступно (headless), вызовы безопасно ничего не делают. ---
    m_lua.set_function("PlaySound", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound2D(path, volume.value_or(1.0f));
    });
    m_lua.set_function("PlaySound3D", [this](const std::string& path, glm::vec3 pos, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound3D: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound3D(path, pos, volume.value_or(1.0f));
    });
    m_lua.set_function("PlayMusic", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlayMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlayMusic(path, volume.value_or(1.0f), true);
    });
    m_lua.set_function("StopMusic", [this]() {
        if (!m_audio) throw std::runtime_error("StopMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->StopMusic();
    });
    m_lua.set_function("SetMasterVolume", [this](float volume) {
        if (!m_audio) throw std::runtime_error("SetMasterVolume: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->SetMasterVolume(volume);
    });
}

void ScriptEngine::RegisterTimerApi() {
    // --- Таймеры: отложенные/повторяющиеся вызовы без ручного хранения
    // "сколько осталось" в самом скрипте. Возвращают id для CancelTimer. ---
    m_lua.set_function("Schedule", [this](float seconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, seconds, 0.0f, false, false, std::move(fn)});
        return id;
    });
    m_lua.set_function("Repeat", [this](float intervalSeconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, intervalSeconds, intervalSeconds, true, false, std::move(fn)});
        return id;
    });
    m_lua.set_function("CancelTimer", [this](int id) {
        for (auto& call : m_scheduled) {
            if (call.Id == id) { call.Cancelled = true; break; }
        }
    });

    // --- Корутины: последовательности во времени как линейный код —
    // StartCoroutine(function() ... wait(1.0) ... end) вместо ручного
    // стейт-машины из Schedule-вызовов. wait() определён Lua-обвязкой ниже,
    // это просто именованная обёртка над coroutine.yield для читаемости.
    //
    // ВАЖНО: sol::coroutine должен строиться НАПРЯМУЮ из sol::function —
    // sol2 сам создаёт для неё новый Lua-поток (thread) с правильной
    // внутренней настройкой стека вызова. Если вместо этого передать сюда
    // результат ручного coroutine.create(fn), первый же resume падает с
    // "attempt to call a thread value" — sol2 ожидает управлять созданием
    // потока сам, а не оборачивать уже готовый Lua-thread.
    m_lua.set_function("StartCoroutine", [this](sol::function fn) {
        // sol::coroutine, построенный НАПРЯМУЮ из функции главного Lua-состояния
        // (sol::coroutine co = fn;), не создаёт для неё отдельный Lua-поток —
        // lua_resume() в его реализации вызывается на lua_state() САМОЙ fn,
        // то есть на главном состоянии. Пока активна только ОДНА такая
        // "корутина", это незаметно работает случайно; как только их две
        // одновременно, вторая резюмится на ТОМ ЖЕ lua_State*, что и первая,
        // и по факту продолжает выполнение первой вместо своей функции.
        // Поэтому явно создаём отдельный Lua-поток (sol::thread) и переносим
        // на его стек функцию через lua_xmove перед тем, как обернуть в
        // sol::coroutine — так каждая корутина резюмится на СВОЁМ потоке.
        sol::thread runner = sol::thread::create(m_lua.lua_state());
        lua_State* runnerState = runner.state().lua_state();
        fn.push();
        lua_xmove(fn.lua_state(), runnerState, 1);
        sol::coroutine co(runnerState, -1);
        m_coroutines.push_back({std::move(co), 0.0f, std::move(runner)});
    });

    // wait(seconds) — именованная обёртка над coroutine.yield для читаемости тела
    // корутины (см. StartCoroutine выше). Регистрируется здесь же, рядом со своим
    // единственным потребителем.
    sol::protected_function_result bootstrap = m_lua.script(
        "function wait(seconds) return coroutine.yield(seconds or 0) end",
        sol::script_pass_on_error);
    if (!bootstrap.valid()) {
        sol::error err = bootstrap;
        LOG_ERROR("ScriptEngine") << "Не удалось зарегистрировать встроенную функцию wait(): " << err.what();
    }
}

void ScriptEngine::RegisterMessagingApi() {
    // --- Сообщения между скриптами: событийная модель для «компоненты общаются
    // друг с другом». Скрипт объявляет хук OnMessage(entity, name, data) (как
    // OnStart/OnUpdate); другой скрипт шлёт ему SendMessage(target, name, data)
    // (target — GameObject или его Id) или всем сразу Broadcast(name, data).
    // data — любое значение Lua (число/строка/таблица) или отсутствует (nil).
    // Так поведения связываются без глобальных переменных и жёстких ссылок. ---
    m_lua.set_function("SendMessage", [this](sol::object target, const std::string& name, sol::object data) {
        int id = -1;
        if (target.is<GameObject>()) {
            GameObject o = target.as<GameObject>();
            if (o.Valid()) id = o.Id();
        } else if (target.is<int>()) {
            id = target.as<int>();
        }
        if (id >= 0) DispatchMessage(id, name, data);
    });
    m_lua.set_function("Broadcast", [this](const std::string& name, sol::object data) {
        DispatchMessage(-1, name, data);
    });
}

void ScriptEngine::SetLaunchArg(const std::string& key, const std::string& value) {
    if (!key.empty()) m_launchArgs[key] = value;
}

void ScriptEngine::SetLaunchArgsFromString(const std::string& args) {
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && std::isspace((unsigned char)args[i])) ++i;
        size_t start = i;
        while (i < args.size() && !std::isspace((unsigned char)args[i])) ++i;
        if (start == i) break;
        std::string token = args.substr(start, i - start);
        // Ведущие дефисы («--autopilot») отбрасываем: и такая запись, и голое
        // «autopilot» — привычные способы задать флаг, различать их незачем.
        size_t dash = token.find_first_not_of('-');
        if (dash == std::string::npos) continue;
        token = token.substr(dash);
        size_t eq = token.find('=');
        if (eq == std::string::npos) SetLaunchArg(token, "1");
        else SetLaunchArg(token.substr(0, eq), token.substr(eq + 1));
    }
}

void ScriptEngine::RegisterLaunchArgsApi() {
    // LaunchArg("seed") -> строка или nil; LaunchFlag("autopilot") -> bool
    // (истина для "1"/"true"/"yes"). Скрипты игры так узнают о режиме запуска,
    // не имея доступа ни к ОС, ни к командной строке.
    m_lua.set_function("LaunchArg", [this](const std::string& key) -> sol::optional<std::string> {
        auto it = m_launchArgs.find(key);
        if (it == m_launchArgs.end()) return sol::nullopt;
        return it->second;
    });
    m_lua.set_function("LaunchFlag", [this](const std::string& key) -> bool {
        auto it = m_launchArgs.find(key);
        if (it == m_launchArgs.end()) return false;
        const std::string& v = it->second;
        return v == "1" || v == "true" || v == "yes" || v == "on";
    });
}

void ScriptEngine::RegisterMathHelpers() {
    // --- Математические хелперы сверх Vec-арифметики: то, чего не выразить
    // операторами. Lerp работает и для чисел, и для Vec3 (перегрузка). ---
    m_lua.set_function("Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); });
    m_lua.set_function("Radians", [](float deg) { return glm::radians(deg); });
    m_lua.set_function("Degrees", [](float rad) { return glm::degrees(rad); });
    m_lua.set_function("Clamp", [](float x, float lo, float hi) { return glm::clamp(x, lo, hi); });
    m_lua.set_function("Lerp", sol::overload(
        [](float a, float b, float t) { return a + (b - a) * t; },
        [](const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * t; }
    ));
}

void ScriptEngine::RegisterLightingApi() {
    // --- Освещение сцены: солнце, ambient, туман — доступны после BindScene.
    // Скрипт-дирижёр катсцены/цикла дня меняет их прямо: GetLighting().Sun.
    // Intensity = 0.2, GetLighting().Fog.Enabled = true и т.п. ---
    m_lua.new_usertype<DirectionalLight>("DirectionalLight",
        "Direction", &DirectionalLight::Direction,
        "Color", &DirectionalLight::Color,
        "Intensity", &DirectionalLight::Intensity
    );
    m_lua.new_usertype<FogSettings>("FogSettings",
        "Enabled", &FogSettings::Enabled,
        "Color", &FogSettings::Color,
        "Start", &FogSettings::Start,
        "End", &FogSettings::End
    );
    m_lua.new_usertype<LightingEnvironment>("LightingEnvironment",
        "SkyColor", &LightingEnvironment::SkyColor,
        "GroundColor", &LightingEnvironment::GroundColor,
        "AmbientStrength", &LightingEnvironment::AmbientStrength,
        "Sun", &LightingEnvironment::Sun,
        "Fog", &LightingEnvironment::Fog
    );
    m_lua.set_function("GetLighting", [this]() -> LightingEnvironment& {
        if (!m_scene) throw std::runtime_error("GetLighting: сцена не привязана (BindScene не вызван)");
        return m_scene->Lighting;
    });
}

void ScriptEngine::RegisterPhysicsApi() {
    // --- Физика времени выполнения: доступна после BindPhysics. Управляет
    // линейной скоростью тела сущности (у неё должен быть RigidBodyComponent,
    // и симуляция должна идти) и гравитацией всего мира. Если тела нет/оно ещё
    // не построено — тихий no-op / нулевая скорость (не ошибка). ---
    m_lua.set_function("SetVelocity", [this](GameObject& obj, const glm::vec3& v) {
        if (!m_physics) throw std::runtime_error("SetVelocity: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return;
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return;
        m_physics->SetLinearVelocity(rb->RuntimeBody, v);
    });
    m_lua.set_function("GetVelocity", [this](GameObject& obj) -> glm::vec3 {
        if (!m_physics) throw std::runtime_error("GetVelocity: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return glm::vec3(0.0f);
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return glm::vec3(0.0f);
        return m_physics->GetLinearVelocity(rb->RuntimeBody);
    });
    m_lua.set_function("SetGravity", [this](const glm::vec3& g) {
        if (!m_physics) throw std::runtime_error("SetGravity: физика не привязана (BindPhysics не вызван)");
        m_physics->SetGravity(g);
    });
    // Мгновенный импульс в тело сущности (толчок/пинок/выстрел). No-op, если у
    // сущности нет тела или симуляция не идёт.
    m_lua.set_function("AddImpulse", [this](GameObject& obj, const glm::vec3& impulse) {
        if (!m_physics) throw std::runtime_error("AddImpulse: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return;
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return;
        m_physics->AddImpulse(rb->RuntimeBody, impulse);
    });
    // Собирает тряпичную куклу (кости-капсулы + суставы) в сцене на месте pos.
    // Возвращает id корневой сущности (таз). Полноценно симулируется на Jolt;
    // на Simple кости просто падают по отдельности (joints не поддержаны).
    m_lua.set_function("SpawnRagdoll", [this](const glm::vec3& pos, sol::optional<float> scale) -> int {
        if (!m_scene) throw std::runtime_error("SpawnRagdoll: сцена не привязана");
        return sage::physics::BuildRagdoll(*m_scene, pos, scale.value_or(1.0f));
    });
}

void ScriptEngine::AttachScript(GameObject object, const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];
    // OnUpdate необязателен для объектных скриптов, которые всё делают в
    // OnStart или через StartCoroutine/Schedule — раньше это было required,
    // что мешало писать скрипты "разово настроил и забыл".

    // OnMessage(entity, name, data) — необязательный хук: если объявлен, скрипт
    // может принимать сообщения от других скриптов (SendMessage/Broadcast).
    sol::protected_function messageFn = env["OnMessage"];

    // Userdata сущности создаём один раз — все дальнейшие вызовы хуков
    // передают его же (см. комментарий у ScriptInstance::EntityRef).
    sol::object entityRef = sol::make_object(m_lua, object);

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn(entityRef);
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    m_instances.push_back({ object, /*HasObject=*/true, std::move(env), std::move(updateFn),
                            std::move(messageFn), std::move(entityRef), scriptPath });
}

void ScriptEngine::RunScript(const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn();
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    m_instances.push_back({ GameObject{}, /*HasObject=*/false, std::move(env), std::move(updateFn),
                            sol::protected_function{}, sol::object{}, scriptPath });
}

void ScriptEngine::DispatchMessage(int targetId, const std::string& name, sol::object data) {
    // Ограничение глубины: OnMessage может сам слать сообщения — цикл из двух
    // обработчиков, пересылающих друг другу, дал бы неограниченную рекурсию и
    // переполнение C++-стека (краш всего движка из-за ошибки в скрипте).
    constexpr int kMaxMessageDepth = 16;
    if (m_messageDepth >= kMaxMessageDepth) {
        LOG_ERROR("ScriptEngine") << "SendMessage/Broadcast: превышена глубина вложенной рассылки ("
                                  << kMaxMessageDepth << ") на сообщении '" << name
                                  << "' — вероятен цикл OnMessage-обработчиков, рассылка оборвана";
        return;
    }
    ++m_messageDepth;

    // Собираем цели ДО вызова обработчиков: обработчик может сам слать сообщения,
    // спавнить или уничтожать объекты — любое из этого способно реаллоцировать
    // m_instances и оборвать итератор. Снимок (объект + КОПИЯ обработчика, дешёвая
    // ссылка на тот же Lua-объект) к такой мутации иммунен. targetId < 0 — всем.
    struct Target { GameObject Object; sol::object Ref; sol::protected_function Fn; };
    std::vector<Target> targets;
    for (auto& inst : m_instances) {
        if (!inst.HasObject || !inst.Object.Valid()) continue;
        if (!inst.MessageFn.valid()) continue;
        if (targetId >= 0 && inst.Object.Id() != targetId) continue;
        targets.push_back({ inst.Object, inst.EntityRef, inst.MessageFn });
    }
    for (auto& t : targets) {
        if (!t.Object.Valid()) continue; // мог быть уничтожен предыдущим обработчиком в этой рассылке
        auto result = t.Fn(t.Ref, name, data);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = t.Object.Valid() ? t.Object.Name() : std::string("?");
            LOG_ERROR("ScriptEngine") << "Ошибка в OnMessage (" << who << "): " << err.what();
        }
    }

    --m_messageDepth;
}

void ScriptEngine::UpdateAll(float deltaTime) {
    for (auto& instance : m_instances) {
        // Объект уничтожен через DestroyObject() — сущность больше не валидна.
        if (instance.HasObject && !instance.Object.Valid()) continue;
        if (!instance.UpdateFn.valid()) continue; // скрипт без OnUpdate — легитимно (см. AttachScript)

        // Объектные скрипты получают entity первым аргументом, уровневые — нет.
        // EntityRef — кэшированный userdata (см. ScriptInstance): без него sol2
        // аллоцировал бы новый объект на каждый скрипт каждый кадр.
        auto result = instance.HasObject ? instance.UpdateFn(instance.EntityRef, deltaTime)
                                         : instance.UpdateFn(deltaTime);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = (instance.HasObject && instance.Object.Valid()) ? instance.Object.Name() : instance.Path;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnUpdate (" << who << "): " << err.what();
        }
    }

    // Убираем экземпляры с уничтоженным Object одним проходом ПОСЛЕ основного
    // цикла — как и ниже для таймеров/корутин, чтобы не мутировать m_instances
    // во время его же итерации (в т.ч. если DestroyObject был вызван изнутри
    // самого OnUpdate этого же экземпляра).
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [](const ScriptInstance& i) { return i.HasObject && !i.Object.Valid(); }),
        m_instances.end());

    UpdateTimers(deltaTime);
    UpdateCoroutines(deltaTime);
    m_tweens.Update(deltaTime); // твины геймплея — в такт со скриптами
}

void ScriptEngine::UpdateTimers(float dt) {
    // Индекс, а не диапазон/итератор: колбэк таймера может сам вызвать
    // Schedule/Repeat (см. ниже), чей push_back способен реаллоцировать
    // m_scheduled — любой ранее взятый итератор/ссылка внутрь вектора после
    // этого висячий (undefined behavior). Индекс остаётся корректным для уже
    // пройденных и текущего элемента (до конца этой функции ничего не
    // удаляется поэлементно, только один проход erase-remove в самом конце).
    //
    // Сам колбэк ПЕРЕД вызовом копируем в локальную переменную, а не вызываем
    // прямо "m_scheduled[i].Fn()": sol2 после resume пишет результат обратно
    // в себя (в "this"), и если бы этот "this" жил внутри вектора, реаллокация
    // ВНУТРИ самого вызова (из-за вложенного Schedule/Repeat) оборвала бы его
    // раньше, чем вызов успеет завершиться — copy sol::protected_function
    // дешёвый (просто ссылка на тот же Lua-объект), поэтому это не накладно.
    //
    // Границу цикла ФИКСИРУЕМ до прохода: таймер, созданный колбэком ВНУТРИ
    // этого же прохода (Schedule из тела таймера), не должен тикать в текущем
    // кадре — иначе он теряет один dt и срабатывает на кадр раньше срока.
    const size_t scheduledAtFrameStart = m_scheduled.size();
    for (size_t i = 0; i < scheduledAtFrameStart; ++i) {
        if (m_scheduled[i].Cancelled) continue;
        m_scheduled[i].TimeLeft -= dt;
        if (m_scheduled[i].TimeLeft > 0.0f) continue;

        int id = m_scheduled[i].Id;
        sol::protected_function fn = m_scheduled[i].Fn;

        auto result = fn();
        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в таймере (id " << id << "): " << err.what();
        }

        // m_scheduled могла реаллоцироваться внутри fn() — обращаемся к
        // элементу заново по тому же индексу, не через старую ссылку.
        if (m_scheduled[i].Repeating) {
            m_scheduled[i].TimeLeft += m_scheduled[i].Interval; // "+=", не "=", чтобы не копить дрейф при просадках FPS
        } else {
            m_scheduled[i].Cancelled = true; // одноразовый — гасим после первого срабатывания
        }
    }

    // Чистим отменённые/сработавшие одноразовые вызовы одним проходом за кадр
    m_scheduled.erase(
        std::remove_if(m_scheduled.begin(), m_scheduled.end(), [](const ScheduledCall& c) { return c.Cancelled; }),
        m_scheduled.end());
}

void ScriptEngine::UpdateCoroutines(float dt) {
    // Индекс, а не итератор — по той же причине, что и в UpdateTimers: тело
    // корутины может само вызвать StartCoroutine, чей push_back способен
    // реаллоцировать m_coroutines. Резюмируем ЛОКАЛЬНУЮ КОПИЮ sol::coroutine
    // (дешёвая операция — копирует лишь ссылку на тот же Lua-поток), а не
    // объект, живущий прямо в векторе: sol2 после lua_resume пишет статус
    // обратно в себя (в "this"), и если бы "this" был указателем внутрь
    // вектора, реаллокация ВНУТРИ самого вызова оборвала бы его раньше, чем
    // вызов успеет завершиться. Индекс остаётся корректным после реаллокации
    // (erase здесь — только для текущего/уже пройденных элементов).
    for (size_t i = 0; i < m_coroutines.size(); ) {
        m_coroutines[i].WaitTime -= dt;
        if (m_coroutines[i].WaitTime > 0.0f) { ++i; continue; }

        sol::coroutine co = m_coroutines[i].Co;
        auto result = co();

        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в корутине: " << err.what();
            m_coroutines.erase(m_coroutines.begin() + i);
            continue;
        }

        if (co.status() == sol::call_status::yielded) {
            // wait(seconds) вернул через yield время следующей паузы.
            // m_coroutines могла реаллоцироваться внутри co() — обращаемся к
            // элементу заново по тому же индексу, не через старую ссылку.
            float nextWait = result.get<sol::optional<float>>().value_or(0.0f);
            m_coroutines[i].Co = co;
            m_coroutines[i].WaitTime = nextWait;
            ++i;
        } else {
            // Корутина дошла до конца функции — больше резюмировать нечего
            m_coroutines.erase(m_coroutines.begin() + i);
        }
    }
}

const Texture* ScriptEngine::GetOrLoadBillboardTexture(const std::string& path) {
    auto it = m_billboardTextures.find(path);
    if (it != m_billboardTextures.end()) return it->second.get();

    // Bilinear — разумный дефолт для одиночных спрайтов (не атлас, в
    // отличие от блочного атласа вокселей, которому нужен Nearest)
    auto texture = std::make_unique<Texture>(path, TextureFilter::Bilinear);
    const Texture* raw = texture.get();
    m_billboardTextures[path] = std::move(texture);
    return raw;
}
