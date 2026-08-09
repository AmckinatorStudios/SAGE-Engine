#pragma once
#include "sage/scene/Scene.h"
#include "sage/core/InputMap.h"
#include "sage/core/RawInput.h"
#include "sage/core/Tween.h"
#include "sage/render/Camera.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/BillboardSystem.h"
#include "sage/render/Texture.h"
#include "sage/audio/AudioEngine.h"
#include "sage/physics/PhysicsScene.h"
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
//   - именованным действиям ввода движка (см. core/InputMap.h) — IsActionDown и т.п.,
//     объявляемым ПРЯМО ИЗ LUA (BindAction("Jump", "SPACE")) — раскладка игры
//     живёт в игре, а не зашита в C++
//   - «сырому» вводу: GetMouseDelta/SetMouseCaptured — вид от первого лица
//     пишется скриптом, без единой строки C++ (см. core/RawInput.h)
//   - МОДУЛЯМ: require("voxel") подтягивает соседний .lua из папок, добавленных
//     через AddScriptSearchPath — игра размером больше одного файла раскладывается
//     по модулям, а общий код (воксельное ядро, утилиты) пишется ОДИН раз
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
    // IsActionDown("Jump"), WasActionPressed("BreakOrHook") и т.п., а также
    // право ОБЪЯВЛЯТЬ свои действия из Lua — BindAction("Jump", "SPACE").
    void BindInput(InputMap& input) { m_input = &input; }

    // Даёт скриптам «сырой» ввод: GetMouseDelta/GetScrollDelta/SetMouseCaptured.
    // Без этого из Lua нельзя написать вид от первого лица — именованные
    // действия дискретны и обзора не дают (см. core/RawInput.h).
    void BindRawInput(sage::RawInputSource& raw) { m_rawInput = &raw; }

    // Параметр запуска игры: скрипт читает его как LaunchArg("autopilot").
    // Хост наполняет их из командной строки (--autopilot=1) и переменной
    // SAGE_GAME_ARGS ("autopilot=1 seed=42"). Зачем: игра целиком на скриптах
    // не может спросить у ОС ничего сама (библиотеки os/io скриптам намеренно
    // закрыты), а автопрогон в CI, номер зерна мира и режим отладки задавать
    // снаружи надо — иначе headless-проверка игры невозможна в принципе.
    void SetLaunchArg(const std::string& key, const std::string& value);

    // Разбирает строку вида "autopilot=1 seed=42" в набор параметров запуска.
    // Голый ключ без '=' («--autopilot») получает значение "1".
    void SetLaunchArgsFromString(const std::string& args);

    // Добавляет папку в путь поиска модулей Lua (require "voxel" найдёт
    // <dir>/voxel.lua). Вызывается хостом со скриптовой папкой проекта —
    // так игра раскладывается по модулям вместо одного файла на всё.
    // Загрузка НАТИВНЫХ модулей (package.cpath) намеренно отключена: скрипты
    // игры не должны уметь подгружать произвольные .so/.dll.
    void AddScriptSearchPath(const std::string& dir);

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
        sol::protected_function MessageFn; // OnMessage(entity, name, data) — необязателен
        // Готовый Lua-userdata сущности, создаётся ОДИН раз в AttachScript.
        // Передача GameObject по значению в каждый вызов OnUpdate заставляла
        // sol2 аллоцировать новый userdata на КАЖДЫЙ скрипт КАЖДЫЙ кадр —
        // чистое давление на Lua GC при десятках скриптов. Дескриптор
        // {registry, entity} стабилен всё время жизни скрипта, кэш корректен.
        sol::object EntityRef;
        std::string Path; // для сообщений об ошибках
        // Сколько раз OnUpdate этого скрипта уже упал и не зовём ли мы его
        // больше. Ошибка в OnUpdate повторяется КАЖДЫЙ КАДР — шестьдесят
        // одинаковых строк в секунду забивают консоль так, что в ней не видно
        // ни второй ошибки, ни чего-либо ещё, ради чего в неё смотрят (см.
        // kMaxUpdateErrors в .cpp).
        int UpdateErrors = 0;
        bool UpdateDisabled = false;
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
        // То же, что у скриптов: повторяющийся таймер с ошибкой в теле — это
        // поток одинаковых сообщений с частотой своего интервала.
        int Errors = 0;
        sol::protected_function Fn;
    };

    struct CoroutineInstance {
        sol::coroutine Co;
        float WaitTime = 0.0f; // сколько ещё ждать перед следующим resume
        // Корутина доиграла или упала. Снимаются такие ОДНИМ проходом в конце
        // кадра, а не по месту: см. UpdateCoroutines в .cpp.
        bool Dead = false;
        // Отдельный Lua-поток, на котором реально резюмится Co (см.
        // StartCoroutine в ScriptEngine.cpp) — держит его живым для GC, пока
        // жива сама корутина; без этого поля поток мог бы быть собран сборщиком
        // мусора между резюмированиями, а сама Co ссылалась бы на мёртвый поток.
        sol::thread Runner;
    };

    // --- Привязка функций: модуль + псевдоним ------------------------------
    //
    // API из Lua раньше был ПЛОСКОЙ КУЧЕЙ: 125 глобальных имён, где
    // SetIKFootLock, SetWaterReflection и BorrowAnimations лежали рядом и
    // ничем не отличались от функций самой игры. Плоское пространство имён
    // плохо не тем, что некрасиво: подсказка редактора бесполезна (список из
    // 125 несвязанных имён), узнать «что вообще есть про анимацию» можно
    // только чтением исходника движка, а игра, объявившая свою функцию
    // Raycast, молча затирает движковую.
    //
    // Теперь каждая функция живёт в модуле — sage.ik.SetFootLock,
    // sage.anim.Borrow, sage.physics.Raycast, — и ОДНОВРЕМЕННО доступна под
    // прежним глобальным именем. Псевдоним ссылается на ТУ ЖЕ функцию, а не
    // регистрируется вторым вызовом: две регистрации одного поведения — это
    // та же болезнь, от которой уходим, только в новой форме.
    //
    // Старые имена не помечены устаревшими и удалять их не планируется: все
    // существующие игры написаны на них, и ломать работающие скрипты ради
    // порядка в пространстве имён — цена без выгоды.
    sol::table Module(const char* name);

public:
    // --- Что запросила игра за кадр (спрашивает хозяин кадра) ---------------
    //
    // Забирающие геттеры: запрос действует ОДИН раз. Скрипт, попросивший
    // сменить сцену дважды за кадр, не должен получить две загрузки.
    bool TakeSceneRequest(std::string& sceneName) {
        if (!m_sceneRequested) return false;
        m_sceneRequested = false;
        sceneName = m_pendingScene;
        return true;
    }
    bool TakeRestartRequest() {
        const bool r = m_restartRequested;
        m_restartRequested = false;
        return r;
    }
    bool TakeQuitRequest() {
        const bool r = m_quitRequested;
        m_quitRequested = false;
        return r;
    }
    // Пауза и масштаб времени — состояние, а не запрос: их спрашивают каждый
    // кадр, и «забрать» их было бы неверно.
    bool Paused() const { return m_paused; }
    void SetPaused(bool paused) { m_paused = paused; }
    float TimeScale() const { return m_timeScale; }
    // Множитель кадра с учётом паузы: 0 на паузе. Одно место, где эти два
    // понятия сводятся вместе, — иначе каждый вызывающий сводил бы их сам и
    // однажды забыл про паузу.
    float FrameTimeScale() const { return m_paused ? 0.0f : m_timeScale; }

private:
    template <typename Fn>
    void Bind(const char* module, const char* name, const char* legacy, Fn&& fn) {
        sol::table table = Module(module);
        table.set_function(name, std::forward<Fn>(fn));
        if (legacy) m_lua[legacy] = table[name];
    }

    // RegisterEngineApi раньше был одной ~600-строчной функцией; теперь это
    // тонкий диспетчер, вызывающий по одному Register*-методу на связную область
    // API (математика, компоненты, сцена, ввод, камера, частицы, …). Порядок
    // вызова сохраняет прежнее поведение, а определения живут в отдельных
    // файлах ScriptApi_*.cpp — по файлу на область, чтобы один .cpp не рос до
    // двух тысяч строк, как было.
    void RegisterEngineApi();
    void RegisterMathTypes();     // Vec2/Vec3/Vec4/Transform + арифметика
    void RegisterComponentTypes();// enum'ы и usertype'ы компонентов ECS
    void RegisterUIApi();         // элемент интерфейса + sage.ui.*
    // Аксессоры интерфейса на GameObject (HasUI/GetUI/AddUI/RemoveUI).
    //
    // Отдельным вызовом, а не строкой в RegisterGameObject, потому что элемент
    // интерфейса — это НАБОР компонентов, и общий BindComponentAccessors<T> тут
    // не подходит: отдавать скрипту один компонент из набора значило бы врать
    // ему о том, что такое элемент. Тело живёт рядом с прокси (ScriptApi_UI.cpp).
    void BindUIAccessors(sol::usertype<GameObject>& t);
    void RegisterTweenApi();      // Ease + sage.tween.*
    void RegisterAnimationApi();  // sage.anim.* и sage.ik.*
    void RegisterGameObject();    // GameObject + аксессоры компонентов + иерархия
    void RegisterSceneApi();      // log, Spawn/Find/DestroyObject
    void RegisterMeshApi();       // SetMeshCube/Sphere/…/Model/None
    void RegisterInputApi();      // IsActionDown/WasActionPressed/…
    void RegisterCameraApi();     // Camera usertype + GetCamera
    void RegisterParticleApi();   // ParticleConfig, пресеты, Emit/Stream
    void RegisterBillboardApi();  // AddBillboard/…
    void RegisterAudioApi();      // PlaySound/PlayMusic/…
    void RegisterSaveApi();       // sage.save.* — прогресс игрока
    void RegisterTimerApi();      // Schedule/Repeat/StartCoroutine + wait()
    void RegisterMessagingApi();  // SendMessage/Broadcast (см. DispatchMessage)
    void RegisterLaunchArgsApi(); // LaunchArg/LaunchFlag
    void RegisterGameFlowApi();   // sage.scene.Load, sage.game.*, sage.time.SetScale
    void RegisterMathHelpers();   // Cross/Lerp/Clamp/Radians/Degrees
    void RegisterLightingApi();   // GetLighting + usertype'ы освещения
    void RegisterPhysicsApi();    // SetVelocity/GetVelocity/SetGravity

    void UpdateTimers(float dt);
    void UpdateCoroutines(float dt);

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
    // ЧЕРЕЗ УКАЗАТЕЛЬ, а не по значению: см. UpdateCoroutines в .cpp.
    std::vector<std::shared_ptr<CoroutineInstance>> m_coroutines;

    Scene* m_scene = nullptr;
    InputMap* m_input = nullptr;
    sage::RawInputSource* m_rawInput = nullptr;
    Camera* m_camera = nullptr;
    ParticleSystem* m_particles = nullptr;
    BillboardSystem* m_billboards = nullptr;
    AudioEngine* m_audio = nullptr;
    PhysicsScene* m_physics = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_billboardTextures;

    // Твины геймплея — тикают в UpdateAll со скриптами (замирают на паузе,
    // умирают вместе с движком скриптов при Stop). Правятся из Lua (Tween*).
    sage::TweenManager m_tweens;

    // Параметры запуска (LaunchArg из Lua). Таблица Lua завести нельзя до
    // RegisterEngineApi, поэтому храним на стороне C++ и отдаём по запросу.
    std::unordered_map<std::string, std::string> m_launchArgs;

    // --- Ход игры: смена сцены, пауза, масштаб времени ---------------------
    //
    // ПОЧЕМУ ЗАПРОС, А НЕ ДЕЙСТВИЕ. Скрипт зовёт sage.scene.Load("level2")
    // изнутри OnUpdate — то есть в момент, когда движок ИДЁТ ПО СУЩНОСТЯМ ЭТОЙ
    // ЖЕ СЦЕНЫ, а сам скрипт держит на них ссылки. Загрузить новую сцену прямо
    // там значит уничтожить реестр под ногами у обхода и оставить скрипту
    // висячий GameObject: падение в лучшем случае, тихая порча памяти в худшем.
    //
    // Поэтому запрос ЗАПОМИНАЕТСЯ, а выполняет его хозяин кадра (плеер или
    // Play-режим редактора) между кадрами, когда ни один скрипт не исполняется.
    // Ровно та же причина, по которой удаление сущностей отложено.
    std::string m_pendingScene;
    bool m_sceneRequested = false;
    bool m_restartRequested = false;
    bool m_quitRequested = false;
    bool m_paused = false;
    float m_timeScale = 1.0f;

    int m_nextTimerId = 1;
};
