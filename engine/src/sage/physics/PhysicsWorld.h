#pragma once
#include <memory>
#include "sage/physics/PhysicsTypes.h"

// ---------------------------------------------------------------------------
// PhysicsWorld — абстракция физического мира (аналог rhi::GraphicsDevice для
// графики). Движок/игры создают тела и шагают симуляцию через ЭТОТ интерфейс,
// не завися от конкретной библиотеки. Реализации живут в
// engine/src/physics/<backend>/ и выбираются фабрикой Create(Backend) —
// добавить новый движок физики значит реализовать интерфейс, код движка/игр
// не переписывается. Это и есть требование «подключать разные библиотеки».
//
// Основной бэкенд — Jolt (jrouwe/JoltPhysics). Встроенный Builtin работает
// всегда без внешних зависимостей (лёгкая аркадная физика). Null — заглушка.
// ---------------------------------------------------------------------------
namespace sage::physics {

class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;

    virtual const char* BackendName() const = 0;
    // false у Null-бэкенда — вызывающий может решить, стоит ли вообще шагать.
    virtual bool IsAvailable() const = 0;

    virtual void SetGravity(const glm::vec3& gravity) = 0;

    // Создаёт тело по описанию; kInvalidBody при ошибке.
    virtual BodyHandle CreateBody(const BodyDesc& desc) = 0;
    virtual void RemoveBody(BodyHandle body) = 0;

    // Продвигает симуляцию на dt секунд (фиксированный внутренний шаг —
    // на усмотрение бэкенда).
    virtual void Step(float dt) = 0;

    // Позиция/поворот тела в мире (для синхронизации обратно в Transform).
    virtual void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const = 0;
    // Телепорт тела (для Kinematic — каждый кадр из Transform сущности).
    virtual void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) = 0;

    virtual void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) = 0;
    virtual glm::vec3 GetLinearVelocity(BodyHandle body) const = 0;

    // Мгновенный импульс (кг·м/с) в центр масс — толчок/выстрел/пинок ragdoll.
    virtual void AddImpulse(BodyHandle body, const glm::vec3& impulse) = 0;

    // --- Соединения (constraints/joints) ------------------------------------
    // Создаёт соединение между телами по описанию; kInvalidJoint при ошибке или
    // если бэкенд их не поддерживает (Builtin/Null логируют и возвращают invalid).
    // Составные тела задаются через BodyDesc.Children в CreateBody (не тут).
    virtual JointHandle CreateJoint(const JointDesc& desc) = 0;
    virtual void RemoveJoint(JointHandle joint) = 0;

    // Поддерживает ли бэкенд настоящие соединения (Jolt — да; Builtin/Null — нет).
    virtual bool SupportsJoints() const = 0;

    // Фабрика бэкенда. Если запрошен Jolt, но он не скомпилирован — вернёт
    // встроенный Builtin (с предупреждением в лог), чтобы код не падал.
    static std::unique_ptr<PhysicsWorld> Create(Backend backend);

    // Бэкенд по умолчанию: Jolt, если собран с SAGE_PHYSICS_JOLT, иначе Builtin.
    static Backend DefaultBackend();

    // Скомпилирован ли Jolt-бэкенд в этой сборке.
    static bool HasJolt();
};

} // namespace sage::physics
