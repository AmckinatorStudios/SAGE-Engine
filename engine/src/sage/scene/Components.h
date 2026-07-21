#pragma once
#include <string>
#include <memory>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include "sage/scene/Transform.h"
#include "sage/render/Mesh.h"
#include "sage/render/Material.h"
#include "sage/physics/PhysicsTypes.h"
#include "sage/anim/Animator.h"
#include "sage/render/Particle.h"
#include "sage/ui/UIAnchor.h"

// ---------------------------------------------------------------------------
// Компоненты ECS — простые data-структуры, навешиваемые на сущности (entity)
// в sage::Scene (поверх entt). Системы (см. sage/ecs/*) итерируют сущности по
// набору компонентов. Движок не зашивает «толстый» объект с фиксированными
// полями — состав сущности собирается из компонентов, что и делает архитектуру
// расширяемой: своя игра добавляет свои компоненты, не трогая ядро.
// ---------------------------------------------------------------------------

// Человекочитаемое имя сущности (для поиска, иерархии, инспектора редактора).
struct NameComponent {
    std::string Name;
};

// Стабильный целочисленный id сущности в пределах сцены. entt::entity несёт в
// себе биты версии и не годится как «простой id» для Lua/сериализации, поэтому
// движок раздаёт свои последовательные id (1,2,3...) и держит карту id->entity
// в Scene. Так сохраняется прежний контракт GameObject.Id / DestroyObject(id).
struct IdComponent {
    int Id = 0;
};

// Иерархия сцены: родитель + список детей. Мировая матрица сущности =
// worldMatrix(Parent) * Transform.GetMatrix() — так дети наследуют перенос/
// поворот/масштаб родителя (см. sage::ecs::WorldMatrix). Связи двусторонние
// (Parent у ребёнка и Children у родителя) и поддерживаются Scene::SetParent.
// Отсутствие компонента = корневая сущность без родителя и детей.
struct HierarchyComponent {
    entt::entity Parent = entt::null;
    std::vector<entt::entity> Children;
};

// Описание того, ИЗ ЧЕГО сделан меш — то, что реально сохраняется в файл сцены.
// Сам GPU-меш (Mesh) не сериализуется, он пересоздаётся при загрузке через
// ResourceManager на основе этого описания.
struct MeshRef {
    // Примитивы генерируются процедурно (Mesh::Create*), Model грузится из файла
    // по path. None — сущность без меша (камера/свет/пустышка), не рисуется.
    enum class Type { None, Cube, Sphere, Plane, Cylinder, Cone, Model };
    Type type = Type::None;
    std::string path; // используется только при Type::Model

    bool operator==(const MeshRef& other) const {
        return type == other.type && path == other.path;
    }
};

// Визуальное представление сущности: чем она нарисована (Ref + runtime-меш на
// GPU) и каким цветом тонируется. Сущность без MeshPtr не рисуется (см.
// RenderSystem) — так же, как раньше пропускался GameObject без MeshComponent.
struct MeshRendererComponent {
    MeshRef Ref;
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    // Runtime-указатель на GPU-меш. Не сериализуется — заполняется
    // ResourceManager'ом на основе Ref при загрузке сцены или назначении меша.
    std::shared_ptr<Mesh> MeshPtr;

    // Материал (.sagemat) — переиспользуемое описание внешнего вида, общее
    // для всех сущностей с этим путём. Path сериализуется; Ptr — runtime,
    // восстанавливается ResourceManager::GetMaterial при загрузке сцены.
    // Назначенный материал ЗАМЕНЯЕТ Color (см. EffectiveColor ниже).
    std::string MaterialPath;
    std::shared_ptr<Material> MaterialPtr;
};

// Итоговый базовый цвет сущности для рендера: albedo материала, если материал
// назначен, иначе — прямой Color компонента. Единая точка выбора для
// редактора и игр (см. использование в EditorLayer/SandboxLayer/TestGame).
inline glm::vec3 EffectiveColor(const MeshRendererComponent& mr) {
    return mr.MaterialPtr ? mr.MaterialPtr->Albedo : mr.Color;
}

// Участие сущности в запечённом глобальном освещении (GI, см. sage/gi).
// Статичная геометрия — окклюдер и отражатель света бейкера; при
// Lightmapped == true сущность получает и собственную лайтмапу (непрямой свет
// запекается в атлас), иначе только влияет на освещение других. TexelScale —
// локальный множитель плотности лайтмапы (крупным неважным поверхностям можно
// уменьшить). Сериализуется; бейк запускается из панели Lighting редактора.
struct GIStaticComponent {
    bool Lightmapped = true;
    float TexelScale = 1.0f;
};

// Участие сущности в сетевой репликации (мультиплеер, см. sage/net):
// СЕРВЕР включает такие сущности в снапшоты (позиция/поворот/масштаб/цвет/
// меш-примитив), клиенты применяют их к своей сцене (по id) с интерполяцией.
// Пустой компонент-маркер; сериализуется со сценой.
struct NetReplicatedComponent {
    bool Placeholder = true; // entt требует непустой тип; поле не используется
};

// Поведение сущности на Lua: путь к .lua файлу со стандартными хуками
// OnStart(entity)/OnUpdate(entity, dt) (см. sage/scripting/ScriptEngine.h).
// Сам компонент — только ДАННЫЕ (путь, сериализуется вместе со сценой);
// привязку к ScriptEngine выполняет рантайм: игра — при старте уровня,
// редактор — при входе в Play-режим.
struct ScriptComponent {
    std::string Path;
};

// Игровая камера сцены. Позицию и ориентацию задаёт Transform сущности
// (Scale не влияет). Панель Game редактора — и игры, которым это удобно —
// рендерят изображение от ПЕРВОЙ сущности с Primary == true; так камера
// становится частью сцены (сериализуется), а не хардкодом кода игры.
struct CameraComponent {
    float Fov = 60.0f;      // вертикальный угол обзора, градусы
    float NearClip = 0.1f;
    float FarClip = 200.0f;
    bool Primary = true;    // первая Primary-камера сцены — «главная»
};

// Свет на сущности: позиция берётся из Transform (двигается гизмо,
// анимируется скриптами — как любой объект). Тип задаёт форму источника:
//   • Point — точечный, светит во все стороны (лампа, факел);
//   • Spot  — прожектор, светит конусом вдоль «вперёд» сущности
//             (-Z её поворота): фонарик, лампа-спот, сценический свет.
// Затухание с расстоянием — из Range (см. Point/SpotLight в Light.h); у Spot
// дополнительно InnerCone/OuterCone задают жёсткость края конуса.
//
// Направленное «солнце» и фоновая засветка остаются НАСТРОЙКАМИ СЦЕНЫ
// (Scene::Lighting — их логично править как окружение, а не таскать по миру).
// Рендер собирает итоговое освещение кадра через sage::ecs::CollectLighting:
// окружение сцены + света-сущности.
struct LightComponent {
    enum class Type { Point, Spot };

    Type Kind = Type::Point;
    glm::vec3 Color{1.0f, 0.9f, 0.7f};
    float Intensity = 1.5f;
    float Range = 12.0f;         // дистанция затухания, единицы мира
    float InnerConeDeg = 20.0f;  // Spot: полная яркость внутри этого полуугла
    float OuterConeDeg = 30.0f;  // Spot: край конуса (яркость -> 0)
};

// Физическое тело сущности (см. sage/physics). Тип задаёт роль:
// Static (пол/стены), Dynamic (падает/сталкивается), Kinematic (двигается
// скриптом, толкает динамику). Форму столкновения задаёт ColliderComponent
// (если его нет — берётся единичный бокс по масштабу сущности). Симуляция идёт
// в Play-режиме редактора и в игре (PhysicsScene): позиция/поворот Dynamic-тел
// пишутся обратно в Transform. Данные сериализуются; RuntimeBody — рантайм.
struct RigidBodyComponent {
    sage::physics::BodyType Type = sage::physics::BodyType::Dynamic;
    float Mass = 1.0f;
    float Friction = 0.5f;
    float Restitution = 0.1f;
    sage::physics::BodyHandle RuntimeBody = sage::physics::kInvalidBody; // не сериализуется
};

// Форма коллайдера. Размеры — ЛОКАЛЬНЫЕ (для единичной сущности); при создании
// тела умножаются на Transform.Scale. Без RigidBodyComponent коллайдер не
// участвует в симуляции (но редактор рисует его гизмо).
struct ColliderComponent {
    sage::physics::ShapeType Shape = sage::physics::ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule

    // СОСТАВНАЯ форма: если Parts непусто, тело строится из этих дочерних форм
    // (каждая со своим локальным смещением/поворотом), а поля выше игнорируются.
    // Так один твёрдый объект получает несколько примитивов — «молоток»,
    // Т/Г-образные детали, грубая аппроксимация модели кластером примитивов.
    struct Part {
        sage::physics::ShapeType Shape = sage::physics::ShapeType::Box;
        glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f};
        float Radius = 0.5f;
        float HalfHeight = 0.5f;
        glm::vec3 Offset{0.0f};   // локальное смещение внутри тела
        glm::vec3 EulerDeg{0.0f}; // локальный поворот (градусы, XYZ)
    };
    std::vector<Part> Parts;
};

// Соединение (constraint/joint) сущности с ДРУГИМ телом или с миром. У сущности
// должен быть RigidBodyComponent (её тело — BodyA). TargetId — id сущности с
// телом-партнёром (BodyB); -1 -> прикрепить к неподвижному миру. Anchor —
// смещение точки крепления от МИРОВОЙ позиции этой сущности; Axis — ось в
// мировых координатах (петля/ползун — вдоль неё, конус — twist-ось). Строится
// PhysicsScene вторым проходом (после всех тел). RuntimeJoint — рантайм.
struct JointComponent {
    sage::physics::JointType Type = sage::physics::JointType::Point;
    int TargetId = -1;                  // id партнёра; -1 -> к миру
    glm::vec3 Anchor{0.0f, 0.0f, 0.0f}; // смещение pivot от позиции сущности (мир)
    glm::vec3 Axis{0.0f, 1.0f, 0.0f};   // ось (Hinge/Slider/Cone)
    bool UseLimits = false;
    float MinLimit = 0.0f;              // Hinge °: [-180..0]; Slider: ед.
    float MaxLimit = 0.0f;
    float MinDistance = 0.0f;           // Distance
    float MaxDistance = 1.0f;
    float ConeHalfAngle = 45.0f;        // Cone °
    sage::physics::JointHandle RuntimeJoint = sage::physics::kInvalidJoint; // не сериализуется
};

namespace sage::render { class SkinnedModel; }

// Скелетно-анимированная модель на сущности. Path — файл .glb/.gltf со скином и
// анимациями; ПУСТОЙ Path означает встроенную процедурную демо-модель (щупалец
// с клипом «Wave»), удобную для примеров и тестов без ассетов. Позой управляет
// система анимации (sage/anim/AnimationSystem): при первом обновлении грузит
// модель, привязывает Animator к её скелету и запускает клип Clip; каждый кадр
// продвигает время и обновляет палитру костей, которой рисуется модель.
//
// Сериализуются только «описательные» поля (Path/DemoSegments/Clip/Speed/Loop/
// Playing); Model/Anim/Ready — рантайм-состояние, восстанавливается загрузкой.
struct AnimatedModelComponent {
    std::string Path;         // .glb/.gltf; пусто -> процедурный демо-щупалец
    int DemoSegments = 6;     // число костей процедурного демо (если Path пуст)
    int Clip = 0;             // индекс проигрываемого клипа (смена -> кросс-фейд)
    float Speed = 1.0f;
    bool Loop = true;
    bool Playing = true;
    float BlendTime = 0.25f;  // длительность кросс-фейда при смене Clip (0 — резко)

    std::shared_ptr<sage::render::SkinnedModel> Model; // рантайм (не сериализуется)
    sage::anim::Animator Anim;                          // рантайм-состояние проигрывания
    bool Ready = false;                                 // инициализирован ли (загрузка+rig)
};

// Элемент игрового интерфейса — UI как часть СЦЕНЫ: создаётся и настраивается
// в редакторе (Inspector), сериализуется в .sage, рендерится в панели Game и в
// собранной игре (SagePlayer) системой sage::ui::DrawSceneUI.
//
// Позиционирование — якорь + отступ ВНУТРИ РОДИТЕЛЯ: если сущность через
// HierarchyComponent вложена в другую сущность с UIElementComponent, её якорь
// считается от прямоугольника родителя (контейнеры/панели), иначе — от экрана.
// Layer задаёт порядок отрисовки среди соседей (больше — поверх); дети всегда
// рисуются поверх родителя. ClipChildren превращает элемент в МАСКУ: всё
// поддерево обрезается его прямоугольником (списки, миникарты, прогресс-контейнеры).
//
// Kind:
//   • Panel — скруглённая подложка (+рамка); с Text — готовая «кнопка»;
//   • Label — только текст (фон не рисуется);
//   • Image — текстура из TexturePath (тонируется Color, скругляется Rounding);
//   • Bar   — полоса прогресса Value 0..1 (фон Color, заполнение BarFillColor).
// Каждый Kind дополнительно рисует Text, если он задан.
struct UIElementComponent {
    enum class Kind { Panel, Label, Image, Bar };

    Kind Type = Kind::Panel;
    UIAnchor Anchor = UIAnchor::TopLeft;
    glm::vec2 Offset{16.0f, 16.0f};
    glm::vec2 Size{200.0f, 56.0f};
    int Layer = 0;              // порядок среди соседей (больше — поверх)
    bool Visible = true;
    bool ClipChildren = false;  // маска: обрезать поддерево своим прямоугольником

    // Стиль подложки/картинки: цвет-заливка (для Image — тонирование), радиус
    // скругления углов, рамка (толщина 0 — без рамки).
    glm::vec4 Color{0.09f, 0.10f, 0.14f, 0.85f};
    float Rounding = 8.0f;
    float BorderThickness = 0.0f;
    glm::vec4 BorderColor{0.85f, 0.80f, 0.65f, 0.9f};

    std::string Text;           // надпись (у любого Kind; пусто — не рисуется)
    float TextScale = 2.0f;
    glm::vec4 TextColor{1.0f, 1.0f, 1.0f, 1.0f};
    bool TextCentered = true;   // по центру прямоугольника (иначе — левый верх + отступ)

    std::string TexturePath;    // Image: путь к текстуре (png/jpg/...)
    float Value = 1.0f;         // Bar: заполнение 0..1 (скрипты пишут сюда)
    glm::vec4 BarFillColor{0.36f, 0.75f, 0.42f, 1.0f};

    std::shared_ptr<Texture> Tex; // рантайм (не сериализуется) — из TexturePath
};

// Эмиттер частиц на сущности: рождает частицы в позиции Transform по правилам
// Config (форма/скорость/цвет/размер/гравитация — см. ParticleEmitterConfig).
// Continuous — непрерывная струя (EmissionRate частиц/сек, дым/огонь);
// иначе — периодические залпы (BurstCount частиц каждые BurstInterval сек,
// искры/всплески). Preset — индекс пресета из ParticlePresets::Registry (для UI
// и «применить пресет»; сама Config сериализуется целиком, правки сохраняются).
struct ParticleEmitterComponent {
    ParticleEmitterConfig Config;
    int Preset = 0;            // выбранный пресет в UI (справочно)
    bool Active = true;
    bool Continuous = true;    // струя (true) или периодические залпы (false)
    int BurstCount = 24;       // частиц в залпе (Continuous=false)
    float BurstInterval = 1.5f;// сек между залпами (Continuous=false)
    float Accumulator = 0.0f;  // рантайм: накопитель эмиссии/таймер залпа (не сериализуется)
};
