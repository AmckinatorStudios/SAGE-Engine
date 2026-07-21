#pragma once
#include "sage/scene/Scene.h"
#include "sage/core/InputMap.h"
#include "sage/core/Tween.h"
#include "sage/render/Camera.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/BillboardSystem.h"
#include "sage/render/Texture.h"
#include "sage/audio/AudioEngine.h"
#include "sage/physics/PhysicsScene.h"

namespace sage::net { class NetworkSystem; }
#include <sol/sol.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

// Система скриптинга на Lua. Позволяет управлять поведением GameObject'ов
// и логикой игры БЕЗ перекомпиляции движка — правишь .lua файл, перезапускаешь
// игру. Часть ЯДРА движка (не зависит от вокселей и конкретной игры).
//
// Помимо базового Transform/Color скрипты получают доступ к:
//   - Vec2/Vec3/Vec4 с арифметикой (+, -, *, длина, нормализация) — для игровой математики
//   - Spawn/Find/Destroy объектов сцены, SetMeshCube/SetMeshModel — можно
//     порождать/убирать сущности из Lua и назначать им геометрию
//   - именованным действиям ввода движка (см. core/InputMap.h) — IsActionDown и т.п.
//   - Schedule/Repeat — отложенные и повторяющиеся вызовы без хранения таймера вручную
//   - StartCoroutine + wait(seconds) — последовательности во времени (катсцены,
//     волны спавна, задержки) пишутся как обычный линейный Lua-код
//   - GetCamera() — чтение/правка позиции и угла обзора камеры (катсцены,
//     программные камера-эффекты)
//   - EmitParticles/CreateParticleStream и т.п. — частицы (залпы и непрерывные
//     струи) с готовыми пресетами из ParticlePresets или своей конфигурацией
//   - AddBillboard/RemoveBillboard и т.п. — именованные спрайты, повёрнутые к камере
//   - ДОСТУП К КОМПОНЕНТАМ сущности: entity:GetLight()/AddRigidBody()/HasCollider()
//     и т.п. для Light/Camera/RigidBody/Collider/ParticleEmitter — скрипт читает и
//     правит любой компонент любой сущности (в т.ч. чужой), т.е. общается со всеми
//     системами движка через ECS, а не только со своим Transform/Color
//   - ИЕРАРХИЯ: entity:SetParent(other)/Parent()/Children()/WorldPosition()/Destroy()
//   - СООБЩЕНИЯ между скриптами: SendMessage(target, name, data)/Broadcast(name, data)
//     и хук OnMessage(entity, name, data) — компоненты общаются друг с другом, не
//     завязываясь на глобальные переменные (событийная модель)
//   - Математика: Cross/Lerp/Clamp/Radians/Degrees сверх Vec-арифметики
//   - GetLighting() — солнце/ambient/туман сцены (день-ночь, программная атмосфера)
//   - SetVelocity/GetVelocity/SetGravity — физика времени выполнения (после BindPhysics)
//
// Использование в игре:
//   ScriptEngine scripts;
//   scripts.BindScene(scene);           // опционально — даёт Spawn/Find/Destroy
//   scripts.BindInput(inputMap);        // опционально — даёт IsActionDown и т.п.
//   scripts.BindCamera(camera);         // опционально — даёт GetCamera()
//   scripts.BindParticles(particles);   // опционально — даёт EmitParticles и т.п.
//   scripts.BindBillboards(billboards); // опционально — даёт AddBillboard и т.п.
//   scripts.AttachScript(myObject, "assets/scripts/spin.lua");
//   ...
//   scripts.UpdateAll(deltaTime); // вызывать каждый кадр — тикает и скрипты, и таймеры/корутины
//
// Пример скрипта (assets/scripts/spin.lua):
//   function OnUpdate(entity, dt)
//       entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 30.0
//   end
class ScriptEngine {
public:
    ScriptEngine();

    // Даёт скриптам доступ к сцене: SpawnObject/FindObject/DestroyObject
    // из Lua будут работать с этой сценой. Без BindScene эти функции
    // бросают ошибку при вызове из Lua — понятную, не сегфолт.
    void BindScene(Scene& scene) { m_scene = &scene; }

    // Даёт скриптам доступ к именованным действиям ввода движка:
    // IsActionDown("Jump"), WasActionPressed("BreakOrHook") и т.п.
    void BindInput(InputMap& input) { m_input = &input; }

    // Даёт скриптам доступ к камере через GetCamera() — читать и менять
    // позицию/угол обзора/FOV, например для программных камера-эффектов
    // или катсцен. Без BindCamera GetCamera() бросает ошибку при вызове.
    void BindCamera(Camera& camera) { m_camera = &camera; }

    // Даёт скриптам доступ к системе частиц: EmitParticles/CreateParticleStream
    // и т.п. Без BindParticles эти функции бросают ошибку при вызове.
    void BindParticles(ParticleSystem& particles) { m_particles = &particles; }

    // Даёт скриптам доступ к системе билбордов: AddBillboard/RemoveBillboard
    // и т.п. Без BindBillboards эти функции бросают ошибку при вызове.
    void BindBillboards(BillboardSystem& billboards) { m_billboards = &billboards; }

    // Даёт скриптам доступ к звуку: PlaySound/PlaySound3D/PlayMusic/StopMusic/
    // SetMasterVolume. Без BindAudio эти функции бросают ошибку при вызове.
    void BindAudio(AudioEngine& audio) { m_audio = &audio; }

    // Даёт скриптам доступ к физике времени выполнения: SetVelocity/GetVelocity
    // сущности с RigidBodyComponent и SetGravity мира. Привязывается там же, где
    // создаётся PhysicsScene (Play-режим редактора, рантайм игры). Без BindPhysics
    // эти функции бросают понятную ошибку при вызове из Lua.
    void BindPhysics(PhysicsScene& physics) { m_physics = &physics; }

    // Мультиплеер (см. sage/net/NetworkSystem): открывает скриптам таблицу
    // Net.* и хуки OnNetMessage/OnClientConnected/... (события сети
    // доставляются в UpdateAll). Без BindNetwork Net.* бросает ошибку.
    void BindNetwork(sage::net::NetworkSystem& network) { m_network = &network; }

    // Загружает .lua файл и привязывает его к объекту. Скрипт должен
    // определить глобальную функцию OnUpdate(entity, dt) — она будет
    // вызываться каждый кадр из UpdateAll(). Необязательная OnStart(entity)
    // вызывается один раз сразу при загрузке. object — дешёвый дескриптор
    // сущности (см. Scene.h), передаётся по значению.
    void AttachScript(GameObject object, const std::string& scriptPath);

    // Выполняет .lua файл как самостоятельный скрипт уровня игры, не
    // привязанный ни к одному объекту — например, "правила уровня",
    // спавнер волн врагов, скрипт-дирижёр катсцены. Если файл определяет
    // OnUpdate(dt) — она будет вызываться каждый кадр так же, как для
    // объектных скриптов, только без параметра entity.
    void RunScript(const std::string& scriptPath);

    // Вызывает OnUpdate для всех привязанных объектных и уровневых
    // скриптов, а также тикает отложенные вызовы (Schedule/Repeat) и
    // резюмирует активные корутины (StartCoroutine/wait). Раз в секунду
    // проверяет mtime привязанных .lua и перечитывает изменившиеся
    // (горячая перезагрузка: правишь скрипт — поведение меняется без
    // перезапуска; OnStart вызывается заново, локальное состояние скрипта
    // сбрасывается).
    void UpdateAll(float deltaTime);

    // Доставляет события столкновений (из PhysicsScene::CollisionEvents) в
    // необязательные хуки скриптов OnCollisionEnter(entity, other) /
    // OnCollisionExit(entity, other). other — GameObject второй сущности пары
    // (может быть невалидным, если её уже удалили). Вызывать после шага физики.
    void DispatchCollisions(const std::vector<sage::physics::CollisionEvent>& events);

    // Принудительная проверка горячей перезагрузки (UpdateAll делает это сам
    // раз в секунду; метод оставлен для тестов/ручного вызова).
    void CheckHotReload();

    // Доступ к состоянию Lua — на случай если игре нужно зарегистрировать
    // свою собственную API-функцию/тип в дополнение к базовой
    sol::state& Lua() { return m_lua; }

private:
    struct ScriptInstance {
        GameObject Object;   // дескриптор сущности; невалиден для уровневых скриптов
        bool HasObject = false; // false для уровневых скриптов (RunScript)
        sol::environment Env;
        sol::protected_function UpdateFn; // может быть невалидной, если OnUpdate не определён
        sol::protected_function MessageFn; // OnMessage(entity, name, data) — необязателен
        sol::protected_function CollisionEnterFn; // OnCollisionEnter(entity, other) — необязателен
        sol::protected_function CollisionExitFn;  // OnCollisionExit(entity, other) — необязателен
        // Готовый Lua-userdata сущности, создаётся ОДИН раз в AttachScript.
        // Передача GameObject по значению в каждый вызов OnUpdate заставляла
        // sol2 аллоцировать новый userdata на КАЖДЫЙ скрипт КАЖДЫЙ кадр —
        // чистое давление на Lua GC при десятках скриптов. Дескриптор
        // {registry, entity} стабилен всё время жизни скрипта, кэш корректен.
        sol::object EntityRef;
        std::string Path; // для сообщений об ошибках и горячей перезагрузки
        long long MTimeNs = 0; // mtime файла на момент загрузки (hot reload)
        // Когда сущность объекта уничтожена (DestroyObject), Object.Valid()
        // становится false: UpdateAll() пропускает такую запись, не обращаясь к
        // мёртвой сущности, и убирает её из m_instances после прохода. Так как
        // Object теперь дескриптор {registry, entity}, а не сырой указатель,
        // само обращение к нему безопасно (проверяется валидность) — но пропуск
        // нужен, чтобы не сыпать ошибками про невалидную сущность каждый кадр.
    };

    struct ScheduledCall {
        int Id = 0;
        float TimeLeft = 0.0f;
        float Interval = 0.0f; // > 0 для Repeat, 0 для одноразового Schedule
        bool Repeating = false;
        bool Cancelled = false;
        sol::protected_function Fn;
    };

    struct CoroutineInstance {
        sol::coroutine Co;
        float WaitTime = 0.0f; // сколько ещё ждать перед следующим resume
        // Отдельный Lua-поток, на котором реально резюмится Co (см.
        // StartCoroutine в ScriptEngine.cpp) — держит его живым для GC, пока
        // жива сама корутина; без этого поля поток мог бы быть собран сборщиком
        // мусора между резюмированиями, а сама Co ссылалась бы на мёртвый поток.
        sol::thread Runner;
    };

    // RegisterEngineApi раньше был одной ~600-строчной функцией; теперь это
    // тонкий диспетчер, вызывающий по одному Register*-методу на связную область
    // API (математика, компоненты, сцена, ввод, камера, частицы, …). Разбивка
    // чисто организационная — порядок вызова сохраняет прежнее поведение, а найти
    // и дополнить нужную группу привязок стало на порядок проще.
    void RegisterEngineApi();
    void RegisterMathTypes();     // Vec2/Vec3/Vec4/Transform + арифметика
    void RegisterComponentTypes();// enum'ы и usertype'ы компонентов ECS
    void RegisterGameObject();    // GameObject + аксессоры компонентов + иерархия
    void RegisterSceneApi();      // log, Spawn/Find/DestroyObject
    void RegisterMeshApi();       // SetMeshCube/Sphere/…/Model/None
    void RegisterInputApi();      // IsActionDown/WasActionPressed/…
    void RegisterCameraApi();     // Camera usertype + GetCamera
    void RegisterParticleApi();   // ParticleConfig, пресеты, Emit/Stream
    void RegisterBillboardApi();  // AddBillboard/…
    void RegisterAudioApi();      // PlaySound/PlayMusic/…
    void RegisterTimerApi();      // Schedule/Repeat/StartCoroutine + wait()
    void RegisterMessagingApi();  // SendMessage/Broadcast (см. DispatchMessage)
    void RegisterMathHelpers();   // Cross/Lerp/Clamp/Radians/Degrees
    void RegisterLightingApi();   // GetLighting + usertype'ы освещения
    void RegisterPhysicsApi();    // SetVelocity/GetVelocity/SetGravity/Raycast
    void RegisterNetApi();        // Net.* (мультиплеер)

    void UpdateTimers(float dt);
    void UpdateCoroutines(float dt);

    // Перечитывает скрипт инстанса в свежее окружение и обновляет хуки
    // (горячая перезагрузка). Возвращает true при успехе.
    bool ReloadInstance(ScriptInstance& inst);
    float m_hotReloadTimer = 0.0f;

    // Раздаёт события сети (DrainScriptEvents) в необязательные хуки скриптов:
    // OnNetMessage(name, data, senderId), OnClientConnected(id),
    // OnClientDisconnected(id), OnNetConnected(), OnNetDisconnected().
    void DispatchNetEvents();

    // Доставляет сообщение обработчикам OnMessage привязанных объектных скриптов.
    // targetId < 0 — широковещательно (всем); иначе — только скриптам сущности с
    // этим Id. Сначала собирает список целей (объект + копия обработчика), затем
    // вызывает — так безопасно к повторному SendMessage/удалению объектов внутри
    // обработчика (никакой инвалидации при реаллокации m_instances). Глубина
    // вложенной рассылки ограничена (см. kMaxMessageDepth в .cpp) — два скрипта,
    // пересылающие сообщение друг другу из OnMessage, не уронят движок
    // переполнением C++-стека, а получат ошибку в лог.
    void DispatchMessage(int targetId, const std::string& name, sol::object data);
    int m_messageDepth = 0; // текущая глубина вложенных DispatchMessage

    // Текстуры для билбордов, заказанных из Lua по пути к файлу, кэшируются
    // здесь (по пути) и живут, пока жив ScriptEngine — билборды в
    // BillboardSystem хранят на них НЕвладеющий указатель (см. AddBillboard).
    const Texture* GetOrLoadBillboardTexture(const std::string& path);

    sol::state m_lua;
    std::vector<ScriptInstance> m_instances;
    std::vector<ScheduledCall> m_scheduled;
    std::vector<CoroutineInstance> m_coroutines;

    Scene* m_scene = nullptr;
    InputMap* m_input = nullptr;
    Camera* m_camera = nullptr;
    ParticleSystem* m_particles = nullptr;
    BillboardSystem* m_billboards = nullptr;
    AudioEngine* m_audio = nullptr;
    PhysicsScene* m_physics = nullptr;
    sage::net::NetworkSystem* m_network = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_billboardTextures;

    // Твины геймплея — тикают в UpdateAll со скриптами (замирают на паузе,
    // умирают вместе с движком скриптов при Stop). Правятся из Lua (Tween*).
    sage::TweenManager m_tweens;

    int m_nextTimerId = 1;
};
