#pragma once
#include <cstdint>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "physics/builtin/Collide.h"
#include "sage/physics/CharacterMotor.h"
#include "sage/physics/PhysicsWorld.h"

// ---------------------------------------------------------------------------
// BuiltinWorld — собственный физический движок SAGE. Работает всегда, без
// внешних зависимостей, и делает полноценную работу твёрдого тела:
//
//   • ВРАЩЕНИЕ. У тела есть ориентация, угловая скорость и честный тензор
//     инерции формы. Ящик, брошенный углом вперёд, кувыркается и ложится на
//     грань; доска крутится вдоль своей длинной оси легче, чем поперёк.
//   • НАСТОЯЩИЕ ФОРМЫ. Коробка — повёрнутая (OBB), а не осевой габарит;
//     капсула — капсула. Столкновения ищутся теоремой о разделяющей оси, и
//     ящик, положенный на бок под углом, лежит НА полу, а не висит над ним.
//   • МАНИФОЛЬДЫ. Плоскость на плоскости даёт до четырёх точек за кадр:
//     только так ящик перестаёт качаться, а стопка ящиков — стоять.
//   • ПОСЛЕДОВАТЕЛЬНЫЕ ИМПУЛЬСЫ с тёплым стартом, трением Кулона по двум
//     касательным и упругостью с порогом. Импульс переносится между кадрами
//     по устойчивому номеру точки — без переноса нижний ящик стопки заметно
//     проседает под верхними на каждом кадре.
//   • СОЕДИНЕНИЯ. Все шесть типов (Fixed/Point/Hinge/Slider/Distance/Cone)
//     решаются в том же цикле, что и контакты, — с пределами, где они есть.
//   • ШИРОКАЯ ФАЗА заметанием (sweep and prune) вместо перебора всех пар:
//     на сотнях тел разница между O(n log n) и O(n²) — это разница между
//     игрой и слайд-шоу.
//   • СОН. Улёгшееся тело перестаёт считаться и просыпается от касания —
//     иначе сцена, где ничего не происходит, стоит столько же, сколько
//     сцена в разгар взрыва.
//   • СПЕКУЛЯТИВНЫЕ КОНТАКТЫ вместо туннелирования: точка контакта заводится
//     ещё до касания, и решатель тормозит тело ровно у поверхности.
//   • КОНТРОЛЛЕР ПЕРСОНАЖА общим CharacterMotor — тем же, что доступен
//     играм со своей геометрией мира.
//
// Что осознанно НЕ сделано: произвольные выпуклые оболочки и треугольные
// сетки как коллайдеры, ткань и мягкие тела. Для них есть Jolt
// (SAGE_PHYSICS_JOLT), и подменять его здесь наполовину было бы хуже, чем
// честно отослать к нему.
// ---------------------------------------------------------------------------
namespace sage::physics {

class BuiltinWorld : public PhysicsWorld {
public:
    const char* BackendName() const override { return "Builtin"; }
    bool IsAvailable() const override { return true; }
    void SetGravity(const glm::vec3& gravity) override { m_gravity = gravity; }

    BodyHandle CreateBody(const BodyDesc& desc) override;
    void RemoveBody(BodyHandle body) override;
    void Step(float dt) override;

    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const override;
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) override;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) override;
    glm::vec3 GetLinearVelocity(BodyHandle body) const override;
    void AddImpulse(BodyHandle body, const glm::vec3& impulse) override;

    JointHandle CreateJoint(const JointDesc& desc) override;
    void RemoveJoint(JointHandle joint) override;
    bool SupportsJoints() const override { return true; }

    bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 RayHit& out, LayerMask mask) const override;
    int OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                      LayerMask mask) const override;
    bool SupportsQueries() const override { return true; }

    void PollContacts(std::vector<ContactEvent>& out) override;
    bool SupportsContacts() const override { return true; }

    CharacterHandle CreateCharacter(const CharacterDesc& desc) override;
    void RemoveCharacter(CharacterHandle character) override;
    void MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) override;
    CharacterState GetCharacterState(CharacterHandle character) const override;
    void SetCharacterPosition(CharacterHandle character, const glm::vec3& position) override;
    bool SupportsCharacters() const override { return true; }

private:
    struct Body {
        BodyType Type = BodyType::Dynamic;
        std::vector<builtin::Part> Parts;

        // Центр масс задан в координатах тела: у составного тела он почти
        // никогда не совпадает с началом координат, и вращать такое тело
        // вокруг начала — значит получить молоток, который крутится вокруг
        // конца ручки, а не вокруг головки.
        glm::vec3 CenterLocal{0.0f};

        glm::vec3 Position{0.0f};   // положение НАЧАЛА КООРДИНАТ тела
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 LinearVelocity{0.0f};
        glm::vec3 AngularVelocity{0.0f};

        float InvMass = 0.0f;
        glm::mat3 InvInertiaLocal{0.0f};   // в координатах тела
        glm::mat3 InvInertiaWorld{0.0f};   // пересчитывается каждый шаг

        float Friction = 0.5f;
        float Restitution = 0.1f;
        LayerMask Layer = kLayerDefault;
        bool Sensor = false;
        bool Alive = true;

        // Сон. Тело, у которого скорости малы дольше kSleepDelay, выключается
        // из счёта; будит его касание с бодрствующим телом или любое внешнее
        // воздействие (импульс, установка скорости, телепорт).
        float IdleTime = 0.0f;
        bool Sleeping = false;

        glm::vec3 Center() const { return Position + Rotation * CenterLocal; }
        bool Movable() const { return Type == BodyType::Dynamic; }
    };

    struct Joint {
        JointDesc Desc;
        bool Alive = true;
        // Локальные рамки крепления, снятые в момент создания: якорь и оси
        // задаются в МИРОВЫХ координатах «как сейчас», а держать соединение
        // надо в системе каждого тела — иначе оно поедет при первом же
        // повороте.
        glm::vec3 LocalAnchorA{0.0f}, LocalAnchorB{0.0f};
        glm::vec3 LocalAxisA{0.0f, 1.0f, 0.0f}, LocalAxisB{0.0f, 1.0f, 0.0f};
        glm::quat RelativeRotation{1.0f, 0.0f, 0.0f, 0.0f};
        // Накопленные импульсы для тёплого старта.
        glm::vec3 PointImpulse{0.0f};
        glm::vec3 AngularImpulse{0.0f};
        float AxialImpulse = 0.0f;
        float LimitImpulse = 0.0f;
    };

    // Пара «часть A — часть B» с манифольдом, живущая между кадрами: по ней
    // переносятся накопленные импульсы (тёплый старт) и различаются «начал
    // касаться» и «продолжает».
    struct ContactKey {
        BodyHandle A, B;
        uint16_t PartA, PartB;
        bool operator==(const ContactKey& o) const {
            return A == o.A && B == o.B && PartA == o.PartA && PartB == o.PartB;
        }
    };
    struct ContactKeyHash {
        size_t operator()(const ContactKey& k) const {
            return ((size_t)k.A * 73856093u) ^ ((size_t)k.B * 19349663u) ^
                   ((size_t)k.PartA * 83492791u) ^ ((size_t)k.PartB * 2654435761u);
        }
    };

    struct ContactConstraint {
        ContactKey Key;
        builtin::Manifold M;
        float Friction = 0.5f;
        float Restitution = 0.0f;
        bool Sensor = false;
        bool Fresh = true;   // манифольда с этим ключом на прошлом шаге не было
    };

    void SubStep(float dt);
    void BuildContacts(float dt);
    void WarmStart();
    // useBias=false — расслабляющий проход: ограничения решаются без
    // выталкивания, чтобы снять внесённую им лишнюю энергию.
    void SolveVelocities(float dt, int iterations, bool useBias);
    // Отдельным проходом ПОСЛЕ расслабляющего: расслабление гасит скорость
    // разлёта, и отскок, посчитанный вместе с ним, был бы им же и съеден.
    void ApplyRestitution();
    void Integrate(float dt);
    void UpdateSleep(float dt);
    void EmitContactEvents();

    void ApplyImpulse(Body& b, const glm::vec3& impulse, const glm::vec3& at);
    void Wake(Body& b) { b.Sleeping = false; b.IdleTime = 0.0f; }

    void SolveJoint(Joint& j, float dt, bool useBias);

    Body* Find(BodyHandle h);
    const Body* Find(BodyHandle h) const;

    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    std::unordered_map<BodyHandle, Body> m_bodies;
    std::unordered_map<JointHandle, Joint> m_joints;

    // Манифольды прошлого шага — источник импульсов для тёплого старта.
    std::unordered_map<ContactKey, builtin::Manifold, ContactKeyHash> m_cache;
    std::vector<ContactConstraint> m_contacts;

    std::set<std::pair<BodyHandle, BodyHandle>> m_touching;
    std::vector<ContactEvent> m_events;

    struct Character {
        CharacterMotor Motor;
        LayerMask CollidesWith = kAllLayers;
    };
    bool SolidBox(const glm::vec3& min, const glm::vec3& max, LayerMask mask) const;
    std::unordered_map<CharacterHandle, Character> m_characters;
    CharacterHandle m_nextCharacter = 1;

    // Буферы широкой фазы — поля, а не локальные переменные: они переживают
    // кадр и не переаллоцируются на каждом шаге.
    struct Proxy {
        BodyHandle Handle;
        glm::vec3 Min, Max;
    };
    std::vector<Proxy> m_proxies;
    std::vector<std::pair<BodyHandle, BodyHandle>> m_pairs;

    BodyHandle m_next = 1;
    JointHandle m_nextJoint = 1;
    float m_accum = 0.0f;
};

} // namespace sage::physics
