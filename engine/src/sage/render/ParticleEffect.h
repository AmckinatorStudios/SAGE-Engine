#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/ParticleCurves.h"

// ---------------------------------------------------------------------------
// ParticleEffect — ПОЛНОЕ описание эффекта частиц данными.
//
// ДВИЖОК НЕ ЗНАЕТ, ЧТО ТАКОЕ «ОГОНЬ». Раньше огонь, дым, искры и брызги были
// функциями в коде движка (ParticlePresets), а параметров у частицы хватало ровно
// на эти рецепты: прямая скорость, гравитация по Y, два цвета, два размера. Дождь,
// снег, туман, магия, пыль в луче — всё, чего в списке не было, собрать было
// нельзя, а то, что было, нельзя было заметно изменить.
//
// Теперь эффект — набор МОДУЛЕЙ, каждый со своим выключателем, и все они
// складываются друг с другом: форма испускания, кривые по времени жизни, силы,
// ветер, турбулентность, сопротивление воздуха, столкновения, текстура с
// раскадровкой, способ отрисовки. Огонь — это данные (файл .sagefx или
// компонент в сцене), а не код; новый эффект не требует ни строчки в движке.
//
// Умолчания — нейтральная белая мягкая частица, медленно поднимающаяся вверх:
// видно, что эмиттер работает, и не навязан ни один «жанр».
//
// Углы — в градусах (так их вводит человек), расстояния — в метрах, время — в
// секундах.
// ---------------------------------------------------------------------------
namespace sage::fx {

// Откуда рождаются частицы и куда летят.
enum class EmitShape {
    Point = 0,   // из точки во все стороны
    Sphere,      // из шара (или его поверхности) наружу
    Hemisphere,  // из полушара вверх (по локальной +Y)
    Cone,        // из круга основания в пределах угла (по +Y) — струя, пламя
    Box,         // из объёма коробки, направление — +Y (дождь из облака)
    Circle,      // из окружности/диска в плоскости XZ наружу (кольцо, волна)
    Edge,        // из отрезка вдоль X, направление — +Y (завеса, водопад)
};

// В чьих координатах живут уже рождённые частицы.
enum class SimulationSpace {
    World = 0,  // остаются там, где родились: дым от едущего факела тянется шлейфом
    Local,      // едут вместе с объектом: аура вокруг персонажа
};

// Как частица рисуется.
enum class RenderMode {
    Billboard = 0,      // квадрат лицом к камере
    Stretched,          // вытянут вдоль скорости: капли дождя, искры, трассеры
    Horizontal,         // лежит в плоскости XZ: круги на воде, лужи, следы
    Vertical,           // стоит вертикально и поворачивается к камере только вокруг Y
    Mesh,               // объёмная фигура: осколки, листья, камешки
    Trail,              // лента за частицей: след кометы, взмах, молния
};

// Как частица смешивается с тем, что за ней.
enum class BlendMode {
    Alpha = 0,      // обычная прозрачность: дым, пыль, листья
    Additive,       // складывается со светом: огонь, искры, магия (ярче к центру)
    Premultiplied,  // и светится, и перекрывает: пламя с тёмным дымом в одной текстуре
};

// Форма квадрата, когда текстуры нет.
enum class ParticleSprite {
    SoftCircle = 0,  // круг с мягким краем
    Circle,          // круг с резким краем
    Square,          // квадрат
};

// Что происходит с кадрами раскадровки.
enum class FlipbookMode {
    OverLifetime = 0,  // кадры проходят за жизнь частицы (Cycles раз)
    Speed,             // с постоянной частотой FrameRate, по кругу
    Random,            // каждой частице — случайный кадр на всю жизнь
    Fixed,             // один кадр StartFrame
};

enum class CollisionMode {
    Plane = 0,  // горизонтальная плоскость на высоте PlaneHeight
    World,      // всё, что есть в физическом мире сцены (лучом)
};

enum class MeshShape { Cube = 0, Sphere, Tetrahedron, Quad };

// Залп в заданный момент цикла эмиттера.
struct Burst {
    float Time = 0.0f;       // секунда цикла
    int CountMin = 10;
    int CountMax = 10;
    int Cycles = 1;          // сколько раз повторить (0 — бесконечно, пока идёт цикл)
    float Interval = 0.5f;   // между повторами
};

// Дочерний эффект — рождается там, где с частицей что-то случилось.
struct SubEmitter {
    enum class Trigger { Birth = 0, Death, Collision };
    Trigger When = Trigger::Death;
    std::string Effect;      // файл .sagefx
    int Count = 5;
    bool InheritColor = true;
    float InheritVelocity = 0.0f;  // доля скорости родителя
};

struct ParticleEffect {
    // --- Основное ------------------------------------------------------------
    float Duration = 5.0f;   // длина цикла эмиттера
    bool Loop = true;
    float StartDelay = 0.0f;
    bool Prewarm = false;    // на старте эффект уже «догорел» один цикл (костёр, а не вспышка розжига)
    int MaxParticles = 1000;
    SimulationSpace Space = SimulationSpace::World;
    float SimulationSpeed = 1.0f;
    float GravityScale = 0.0f;         // доля мирового тяготения (9.81 вниз)
    float InheritVelocity = 0.0f;      // доля скорости самого эмиттера

    Range StartLifetime{2.0f, 3.0f};
    Range StartSpeed{0.6f, 1.0f};
    Range StartSize{0.15f, 0.25f};
    Range StartRotation{0.0f, 360.0f};   // градусы
    // Начальный цвет — случайный между двумя (одинаковые — без разброса).
    glm::vec4 StartColorA{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 StartColorB{1.0f, 1.0f, 1.0f, 1.0f};

    // --- Испускание ----------------------------------------------------------
    float RateOverTime = 12.0f;        // частиц в секунду
    float RateOverDistance = 0.0f;     // частиц на метр пути эмиттера (след за бегущим)
    std::vector<Burst> Bursts;

    // --- Форма ---------------------------------------------------------------
    EmitShape Shape = EmitShape::Cone;
    float Radius = 0.2f;
    float ConeAngle = 15.0f;           // полуугол струи
    float Arc = 360.0f;                // доля окружности (у Circle, Cone, Sphere)
    glm::vec3 BoxSize{1.0f, 1.0f, 1.0f};
    float EdgeLength = 1.0f;
    bool FromShell = false;            // только с поверхности, а не из объёма
    float RandomizeDirection = 0.0f;   // 0 — по форме, 1 — во все стороны
    glm::vec3 ShapeOffset{0.0f};
    glm::vec3 ShapeRotation{0.0f};     // градусы, XYZ

    // --- Скорость по времени жизни ------------------------------------------
    bool UseVelocity = false;
    glm::vec3 LinearVelocity{0.0f};    // добавочная скорость, м/с
    float Orbital = 0.0f;              // вращение вокруг оси Y эмиттера, град/с
    float Radial = 0.0f;               // от оси эмиттера наружу, м/с
    Curve SpeedOverLifetime;           // множитель скорости (1 — без изменения)

    // --- Силы ----------------------------------------------------------------
    bool UseForces = false;
    glm::vec3 Force{0.0f};             // постоянное ускорение, м/с² (плавучесть дыма — вверх)
    glm::vec3 Wind{0.0f};              // скорость ветра, м/с: частицы к ней стремятся
    float WindInfluence = 1.0f;        // как быстро частица подхватывает ветер (1/с)
    float Gustiness = 0.0f;            // порывы: доля колебаний ветра, 0..1
    float Drag = 0.0f;                 // сопротивление воздуха, 1/с
    float MaxSpeed = 0.0f;             // потолок скорости, 0 — без него
    float Attraction = 0.0f;           // к центру эмиттера (+) или прочь (−), м/с²

    // --- Турбулентность ------------------------------------------------------
    bool UseNoise = false;
    float NoiseStrength = 1.0f;        // м/с²
    float NoiseFrequency = 0.5f;       // 1/м — чем больше, тем мельче завихрения
    float NoiseScroll = 0.3f;          // как быстро поле меняется во времени
    int NoiseOctaves = 2;

    // --- Размер / цвет / вращение по времени жизни --------------------------
    bool UseSizeOverLifetime = false;
    Curve SizeOverLifetime;
    bool UseSizeBySpeed = false;       // быстрые — крупнее (искры вытягиваются в полосы)
    Range SizeBySpeedRange{0.0f, 5.0f};
    Curve SizeBySpeed;
    bool UseColorOverLifetime = false;
    Gradient ColorOverLifetime;
    bool UseColorBySpeed = false;      // быстрые — горячее
    Range ColorBySpeedRange{0.0f, 5.0f};
    Gradient ColorBySpeed;
    bool UseRotation = false;
    Range AngularVelocity{-45.0f, 45.0f};  // град/с
    Curve RotationOverLifetime;        // множитель угловой скорости
    bool AlignToVelocity = false;      // поворот квадрата по направлению движения (перо, лист)

    // --- Столкновения --------------------------------------------------------
    bool UseCollision = false;
    CollisionMode Collision = CollisionMode::Plane;
    float PlaneHeight = 0.0f;          // мировая высота плоскости
    float Bounce = 0.3f;               // доля скорости по нормали после удара
    float Friction = 0.2f;             // сколько теряется вдоль поверхности
    float LifetimeLoss = 0.0f;         // доля жизни, отнятая ударом (1 — умирает)
    float CollisionRadius = 0.0f;      // «толщина» частицы для столкновения, м

    // --- Текстура и раскадровка ---------------------------------------------
    // Одна картинка (Texture) или лист кадров TilesX×TilesY. Либо КАДРЫ
    // ОТДЕЛЬНЫМИ ФАЙЛАМИ (Frames): наборы часто так и приходят —
    // smoke_0.png…smoke_11.png; движок сам собирает их в лист.
    std::string Texture;
    std::vector<std::string> Frames;
    int TilesX = 1;
    int TilesY = 1;
    FlipbookMode Flipbook = FlipbookMode::OverLifetime;
    float FrameRate = 12.0f;
    float Cycles = 1.0f;
    int StartFrame = 0;
    bool RandomStartFrame = false;
    bool PixelArt = false;             // без сглаживания (пиксельная графика)

    // --- Отрисовка -----------------------------------------------------------
    RenderMode Render = RenderMode::Billboard;
    BlendMode Blend = BlendMode::Alpha;
    ParticleSprite Sprite = ParticleSprite::SoftCircle;
    float Intensity = 1.0f;            // яркость (больше 1 — светится и цепляет свечение поста)
    bool SortByDistance = false;       // дальние рисуются первыми (для плотного альфа-дыма)
    float StretchLength = 1.0f;        // Stretched: длина относительно размера
    float StretchBySpeed = 0.1f;       // Stretched: + столько размеров на 1 м/с
    MeshShape Mesh = MeshShape::Cube;
    glm::vec3 MeshScale{1.0f};
    // Trail: лента за частицей.
    float TrailLifetime = 0.4f;        // сколько секунд живёт точка следа
    float TrailMinDistance = 0.05f;    // новая точка — не ближе этого к прошлой
    int TrailMaxPoints = 24;
    Curve TrailWidth;                  // по длине следа: 0 — голова, 1 — хвост (множитель размера)
    Gradient TrailColor;               // по длине следа, умножается на цвет частицы
    bool TrailHead = true;             // рисовать ли саму частицу поверх следа

    std::vector<SubEmitter> SubEmitters;
};

} // namespace sage::fx
