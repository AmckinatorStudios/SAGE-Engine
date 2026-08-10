#pragma once
#include <memory>
#include <vector>
#include "sage/physics/PhysicsTypes.h"

// ---------------------------------------------------------------------------
// PhysicsWorld — абстракция физического мира (аналог rhi::GraphicsDevice для
// графики). Движок/игры создают тела и шагают симуляцию через ЭТОТ интерфейс,
// не завися от конкретной библиотеки. Реализации живут в
// engine/src/physics/<backend>/ и выбираются фабрикой Create(Backend) —
// добавить новый движок физики значит реализовать интерфейс, код движка/игр
// не переписывается. Это и есть требование «подключать разные библиотеки».
//
// Встроенный бэкенд (physics/builtin) работает всегда без внешних
// зависимостей и делает полную работу твёрдого тела. Jolt (jrouwe/JoltPhysics)
// подключается опционально и добавляет выпуклые оболочки, треугольные сетки и
// мягкие тела. Null — заглушка.
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
    // если бэкенд их не поддерживает (Null логирует и возвращает invalid).
    // Составные тела задаются через BodyDesc.Children в CreateBody (не тут).
    virtual JointHandle CreateJoint(const JointDesc& desc) = 0;
    virtual void RemoveJoint(JointHandle joint) = 0;

    // Поддерживает ли бэкенд настоящие соединения (встроенный и Jolt — да, Null — нет).
    virtual bool SupportsJoints() const = 0;

    // --- Запросы к миру -----------------------------------------------------
    //
    // Луч — самая нужная операция физики после самого шага, и её отсутствие
    // видно сразу: без неё нельзя ни выстрелить, ни подобрать предмет
    // прицелом, ни узнать высоту пола под ногой. Последнее не гипотетика:
    // система IK вынуждена требовать высоту опоры ОТ ИГРЫ ровно потому, что
    // спросить её у движка было не у кого.
    //
    // dir не обязан быть нормализован. mask — какие слои интересуют.
    // Возвращает ближайшее попадание; false — луч ушёл в пустоту.
    virtual bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                         RayHit& out, LayerMask mask = kAllLayers) const {
        (void)origin; (void)direction; (void)maxDistance; (void)mask;
        out = RayHit{};
        return false;
    }

    // Все тела, пересекающие сферу. Взрыв, зона подбора, «кто рядом» — это он.
    // Возвращает число найденных (не больше вместимости out).
    virtual int OverlapSphere(const glm::vec3& center, float radius,
                              std::vector<BodyHandle>& out,
                              LayerMask mask = kAllLayers) const {
        (void)center; (void)radius; (void)mask;
        out.clear();
        return 0;
    }

    // Умеет ли бэкенд запросы (луч/сфера). Null — нет.
    virtual bool SupportsQueries() const { return false; }

    // --- События столкновений -----------------------------------------------
    //
    // Забирает события, накопленные с прошлого вызова, и очищает очередь.
    // Опрос, а не колбэк: бэкенд зовёт свои обработчики из рабочих потоков
    // посреди шага, и трогать оттуда сцену или скрипты нельзя.
    virtual void PollContacts(std::vector<ContactEvent>& out) { out.clear(); }
    virtual bool SupportsContacts() const { return false; }

    // --- Контроллер персонажа -----------------------------------------------
    //
    // Почему не «динамическое тело в форме капсулы»: персонаж под управлением
    // игрока физически неправдоподобен по замыслу — мгновенно меняет скорость,
    // не опрокидывается, взбирается на ступеньку и держится на склоне, с
    // которого бочка съехала бы. Из динамики это выжимается либо ватным
    // управлением, либо телом, заваливающимся набок.
    virtual CharacterHandle CreateCharacter(const CharacterDesc& desc) {
        (void)desc;
        return kInvalidCharacter;
    }
    virtual void RemoveCharacter(CharacterHandle character) { (void)character; }

    // Двигает персонажа с заданной скоростью, разбираясь со стенами, ступенями
    // и склонами. Скорость ЗАДАЁТСЯ, а не накапливается: тяготение, прыжок и
    // ускорение — правила игры, а не физики, и держать их здесь значило бы
    // навязывать всем одну модель движения.
    virtual void MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) {
        (void)character; (void)velocity; (void)dt;
    }
    virtual CharacterState GetCharacterState(CharacterHandle character) const {
        (void)character;
        return CharacterState{};
    }
    virtual void SetCharacterPosition(CharacterHandle character, const glm::vec3& position) {
        (void)character; (void)position;
    }
    virtual bool SupportsCharacters() const { return false; }

    // Фабрика бэкенда. Если запрошен Jolt, но он не скомпилирован — вернёт
    // встроенный движок (с предупреждением в лог), чтобы код не падал.
    static std::unique_ptr<PhysicsWorld> Create(Backend backend);

    // Бэкенд по умолчанию: Jolt, если собран с SAGE_PHYSICS_JOLT, иначе встроенный.
    static Backend DefaultBackend();

    // Скомпилирован ли Jolt-бэкенд в этой сборке.
    static bool HasJolt();
};

} // namespace sage::physics
