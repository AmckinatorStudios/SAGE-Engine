#pragma once
#include "sage/scene/Scene.h"
#include "sage/core/InputMap.h"
#include "sage/render/Camera.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/BillboardSystem.h"
#include "sage/render/Texture.h"
#include "sage/audio/AudioEngine.h"
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
    // резюмирует активные корутины (StartCoroutine/wait).
    void UpdateAll(float deltaTime);

    // Доступ к состоянию Lua — на случай если игре нужно зарегистрировать
    // свою собственную API-функцию/тип в дополнение к базовой
    sol::state& Lua() { return m_lua; }

private:
    struct ScriptInstance {
        GameObject Object;   // дескриптор сущности; невалиден для уровневых скриптов
        bool HasObject = false; // false для уровневых скриптов (RunScript)
        sol::environment Env;
        sol::protected_function UpdateFn; // может быть невалидной, если OnUpdate не определён
        std::string Path; // для сообщений об ошибках
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

    void RegisterEngineApi();
    void UpdateTimers(float dt);
    void UpdateCoroutines(float dt);

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
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_billboardTextures;

    int m_nextTimerId = 1;
};
