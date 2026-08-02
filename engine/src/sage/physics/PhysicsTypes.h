#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Общие типы физической подсистемы — бэкенд-независимые (как sage/rhi для
// графики). Движок и игры описывают тела через эти структуры и обращаются к
// интерфейсу PhysicsWorld; конкретный движок физики (Jolt — основной, плюс
// встроенный Simple и пустой Null) прячется за фабрикой PhysicsWorld::Create.
// ---------------------------------------------------------------------------
namespace sage::physics {

// Доступные бэкенды. Jolt — основной (полноценная физика, jrouwe/JoltPhysics),
// подключается опционально через CMake (SAGE_PHYSICS_JOLT). Simple — встроенный
// лёгкий интегратор (гравитация + столкновение со статикой), без зависимостей,
// доступен всегда. Null — заглушка (физика отключена).
enum class Backend { Null, Simple, Jolt };

// Тип тела:
//   Static    — неподвижное препятствие (пол, стены);
//   Dynamic   — управляется физикой (падает, сталкивается);
//   Kinematic — движется извне (скриптом/анимацией), толкает динамику, но само
//               не реагирует на столкновения.
enum class BodyType { Static, Dynamic, Kinematic };

// Форма коллайдера. Capsule приближается боксом во встроенном Simple-бэкенде;
// Jolt поддерживает её полноценно.
enum class ShapeType { Box, Sphere, Capsule };

// Одна дочерняя форма СОСТАВНОГО (compound) тела: форма + её ЛОКАЛЬНОЕ смещение/
// поворот относительно начала тела. Составное тело = несколько таких форм в
// одном твёрдом теле (напр. «молоток» = ручка-бокс + головка-бокс со сдвигом,
// или коллайдер-аппроксимация модели из нескольких примитивов). Jolt строит из
// них StaticCompoundShape; Simple берёт объемлющий AABB (аркадное приближение).
struct ChildShape {
    ShapeType Shape = ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule
    glm::vec3 Position{0.0f};                // локальное смещение внутри тела
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Дескриптор тела внутри мира (0 — невалидный). Непрозрачный для вызывающего.
using BodyHandle = uint32_t;
constexpr BodyHandle kInvalidBody = 0;

// --- Слои столкновений --------------------------------------------------------
//
// Битовая маска: у тела ОДИН слой, у запроса — маска слоёв, которые его
// интересуют. Нужны ровно затем, что без них любой луч из камеры первым делом
// упирается в самого игрока, а «выстрел» попадает в собственную капсулу.
//
// Маска, а не список имён: проверка попадания — это одно `&`, и она выполняется
// на каждое тело каждого запроса. Имена слоёв — дело игры, движку хватает битов.
using LayerMask = uint32_t;
constexpr LayerMask kLayerDefault = 1u;
constexpr LayerMask kAllLayers = 0xFFFFFFFFu;

// Результат луча/пересечения.
struct RayHit {
    bool Hit = false;
    BodyHandle Body = kInvalidBody;
    glm::vec3 Point{0.0f};    // точка попадания в мире
    glm::vec3 Normal{0.0f};   // нормаль поверхности в этой точке
    float Distance = 0.0f;    // от начала луча
};

// Событие столкновения. Копится бэкендом за шаг и забирается PollContacts —
// НЕ колбэком: Jolt зовёт свой listener из рабочих потоков посреди шага, и
// трогать оттуда сцену, скрипты или ресурсы нельзя. Опрос на главном потоке
// после Step снимает вопрос целиком, а движок и так уже опрашивает результаты
// на кадр позже (см. проверку перекрытия в RenderBatch).
struct ContactEvent {
    enum class Phase { Begin, End };
    Phase When = Phase::Begin;
    BodyHandle A = kInvalidBody;
    BodyHandle B = kInvalidBody;
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0.0f};   // от A к B
    float Impulse = 0.0f;     // сила удара; 0 у сенсоров и у Phase::End
    bool Sensor = false;      // хотя бы одно из тел — сенсор (триггер-зона)
};

// --- Контроллер персонажа -----------------------------------------------------
//
// Отдельная сущность, а не «капсула с телом»: персонаж, которым управляет
// игрок, физически неправдоподобен по замыслу — он мгновенно меняет скорость,
// не опрокидывается, взбирается на ступеньку и не соскальзывает со склона, на
// котором любая бочка съехала бы. Пытаться получить это из динамического тела
// — известный тупик: получается либо ватное управление, либо тело, которое
// заваливается набок от косого взгляда.
struct CharacterDesc {
    float Radius = 0.35f;
    float Height = 1.8f;          // полная высота капсулы
    glm::vec3 Position{0.0f};
    float StepHeight = 0.35f;     // на какую ступеньку взбирается без прыжка
    float MaxSlopeDeg = 50.0f;    // круче — соскальзывает, а не идёт
    float Mass = 70.0f;           // с какой силой толкает динамику
    LayerMask Layer = kLayerDefault;
    LayerMask CollidesWith = kAllLayers;
};

using CharacterHandle = uint32_t;
constexpr CharacterHandle kInvalidCharacter = 0;

// Состояние контроллера после шага.
struct CharacterState {
    glm::vec3 Position{0.0f};
    glm::vec3 Velocity{0.0f};
    bool Grounded = false;        // стоит ли на опоре
    glm::vec3 GroundNormal{0.0f, 1.0f, 0.0f};
    bool OnSteepSlope = false;    // касается земли, но склон круче предела
};

// Описание тела для создания. Позиция/поворот — начальные (дальше их ведёт
// физика для Dynamic). Размеры — в ЛОКАЛЬНЫХ единицах формы; масштаб сущности
// применяется при создании тела (см. PhysicsScene).
struct BodyDesc {
    BodyType Type = BodyType::Dynamic;
    ShapeType Shape = ShapeType::Box;
    glm::vec3 HalfExtents{0.5f, 0.5f, 0.5f}; // Box
    float Radius = 0.5f;                     // Sphere / Capsule
    float HalfHeight = 0.5f;                 // Capsule (половина цилиндрической части)

    // Составная форма: если непусто, тело строится ИЗ ЭТИХ дочерних форм, а
    // Shape/HalfExtents/Radius/HalfHeight выше игнорируются. Пусто — одиночная
    // форма как раньше (обратная совместимость).
    std::vector<ChildShape> Children;

    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};

    float Mass = 1.0f;        // кг (для Dynamic; Static/Kinematic игнорируют)
    float Friction = 0.5f;    // 0..1
    float Restitution = 0.1f; // упругость 0..1 (0 — не отскакивает)

    // Слой тела — по нему его находят (или не находят) лучи и события.
    LayerMask Layer = kLayerDefault;

    // Сенсор (триггер-зона): участвует в обнаружении столкновений, но не
    // отталкивает. «Вошёл в зону», «наступил на кнопку», «поднял предмет» — это
    // он. Без сенсоров зону приходится проверять расстоянием в скрипте каждый
    // кадр, и она работает только для шара.
    bool Sensor = false;
};

// Тип соединения (constraint/joint) между двумя телами (или телом и миром):
//   Fixed    — жёсткая связь (фиксирует взаимное положение/поворот; сварка);
//   Point    — шарнир-точка (ball-socket): точка совпадает, вращение свободно;
//   Hinge    — петля вокруг оси (дверь/колесо), опц. пределы угла [Min,Max]°;
//   Slider   — призма вдоль оси (поршень/лифт), опц. пределы хода [Min,Max];
//   Distance — держит две точки на расстоянии [MinDistance, MaxDistance] (трос);
//   Cone     — конус (ограничивает отклонение оси на ConeHalfAngle°) — основа
//              суставов ragdoll (плечи/бёдра болтаются в конусе).
enum class JointType { Fixed, Point, Hinge, Slider, Distance, Cone };

// Описание соединения. Точка/оси — в МИРОВЫХ координатах на момент создания
// (бэкенд вычисляет из них локальные рамки тел по их текущим позам). BodyB ==
// kInvalidBody — прикрепить BodyA к миру (неподвижный якорь).
struct JointDesc {
    JointType Type = JointType::Point;
    BodyHandle BodyA = kInvalidBody;
    BodyHandle BodyB = kInvalidBody;   // kInvalidBody -> к миру
    glm::vec3 Anchor{0.0f};            // мировая точка крепления (pivot)
    glm::vec3 Axis{0.0f, 1.0f, 0.0f};  // ось: Hinge/Slider — вдоль; Cone — twist

    bool UseLimits = false;            // Hinge/Slider: включить пределы ниже
    float MinLimit = 0.0f;             // Hinge: угол °; Slider: смещение (ед.)
    float MaxLimit = 0.0f;

    float MinDistance = 0.0f;          // Distance
    float MaxDistance = 1.0f;
    float ConeHalfAngle = 45.0f;       // Cone: полу-угол конуса, градусы
};

// Дескриптор соединения внутри мира (0 — невалидный).
using JointHandle = uint32_t;
constexpr JointHandle kInvalidJoint = 0;

} // namespace sage::physics
