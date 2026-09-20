#include "EditorPlaySession.h"

#include <cstdint>

#include "imgui.h"

#include "sage/audio/AudioSystem.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Systems.h"
#include "sage/render/ParticleSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace fs = std::filesystem;

void EditorPlaySession::Attach(sage::input::GlfwBridge& bridge, Window& window) {
    m_bridge = &bridge;
    m_bridge->Attach(window, m_input);
    m_cursor.Attach(*m_bridge);
    m_input.SetCursorControl(&m_cursor);
}

sage::input::GlfwBridge& EditorPlaySession::Bridge() { return *m_bridge; }

AudioEngine& EditorPlaySession::Audio() {
    // Устройство может отсутствовать (headless CI): AudioEngine в этом случае
    // работает вхолостую, но вызовы из Lua остаются валидными.
    if (!m_audio) m_audio = std::make_unique<AudioEngine>();
    return *m_audio;
}

int EditorPlaySession::Start(const PlayContext& ctx) {
    if (Active()) return -1;
    Scene& scene = **ctx.ScenePtr;

    // Снимок сцены — Stop вернёт всё ровно как было до Play. Берётся ЗДЕСЬ, а
    // не в BuildRuntime: переход на другой уровень посреди игры не имеет права
    // затереть снимок документа, к которому Stop обязан вернуть человека.
    m_snapshot = SceneSerializer::SaveToString(scene);

    const int attached = BuildRuntime(ctx, scene);
    m_state = EditorPlayState::Playing;
    return attached;
}

// Всё, что делает сцену ЖИВОЙ: скрипты, ввод, звук, физика, состав кадра.
// Отдельно от Start, потому что то же самое нужно при переходе на другой
// уровень прямо во время игры (scene:Load) — и делать это вторым, похожим, но
// другим куском кода значит, что превью и игра однажды разойдутся.
int EditorPlaySession::BuildRuntime(const PlayContext& ctx, Scene& scene) {
    m_scripts = std::make_unique<ScriptEngine>();
    m_scripts->BindScene(scene);
    // Паритет с рантаймом: частицы доступны скриптам уже в OnStart
    // (EmitParticles/CreateParticleStream рисуются в предпросмотре сцены).
    if (ctx.Particles) m_scripts->BindParticles(*ctx.Particles);

    // Ввод — как в собранной игре: действия объявляют сами скрипты (BindAction),
    // поэтому карту действий начинаем с ЧИСТОГО ЛИСТА на каждый Play (иначе
    // раскладка прошлого запуска пережила бы правку скрипта), а привязываем ДО
    // AttachScript — OnStart скриптов зовёт BindAction прямо оттуда.
    m_input.ClearActions();
    m_scripts->BindInput(m_input);
    // Действия уходят и на шину сцены («input.Jump») — ровно как в собранной
    // игре: превью обязано вести себя так же, иначе редактор перестаёт
    // заменять сборку.
    m_input.SetEventBus(&scene.Events);

    m_scripts->BindAudio(Audio());

    // Модули Lua (require "voxel") ищутся в скриптовой папке ОТКРЫТОГО ПРОЕКТА —
    // тот же контракт, что в собранной игре, где CWD и есть корень проекта.
    m_scripts->AddScriptSearchPath((ctx.ProjectDir / "assets" / "scripts").string());
    m_scripts->AddScriptSearchPath("assets/scripts"); // скрипты рядом с редактором

    // Параметры запуска игры (LaunchArg в Lua) — до AttachScript, потому что
    // OnStart скриптов читает их сразу. В редакторе источник один: окружение
    // (headless-прогон CI ставит SAGE_GAME_ARGS="autopilot=1").
    if (const std::string args = sage::EnvString("SAGE_GAME_ARGS"); !args.empty())
        m_scripts->SetLaunchArgsFromString(args);

    // Физика: строим мир по сущностям с RigidBodyComponent. Бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный движок (см. PhysicsWorld::DefaultBackend).
    //
    // ДО скриптов, а не после: Start скрипта вправе спросить у себя
    // CharacterController и физику, а не получить «не привязано» ровно на
    // первом кадре. Состав мира всё равно досинхронизируется каждым шагом
    // (PhysicsScene::SyncBodies), поэтому объекты, созданные из Start, не
    // теряются.
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), scene);
    m_scripts->BindPhysics(*m_physics);

    // --- СИСТЕМА СКРИПТИНГА -------------------------------------------------
    //
    // Поведение объектов ведёт она, а не прежний ScriptEngine: у неё жизненный
    // цикл (Start/Update/FixedUpdate/LateUpdate/OnDestroy), события
    // столкновений и публичные переменные из сцены. Язык подключается
    // бэкендом — Lua работает на состоянии прежнего движка, поэтому и старые
    // скрипты (глобальные OnStart/OnUpdate), и новые (`return Player`) живут
    // рядом и видят один и тот же API.
    m_scripting = std::make_unique<sage::scripting::ScriptingSystem>();
    m_scripting->AddBackend(sage::scripting::MakeLuaBackend({m_scripts.get()}));
    m_scripting->SetProjectDir(ctx.ProjectDir);
    sage::scripting::ScriptServices services;
    services.ScenePtr = &scene;
    services.Input = &m_input;
    services.Physics = m_physics.get();
    services.Audio = m_audio.get();
    m_scripting->Bind(services);

    // Ошибка в одном скрипте (нет файла, синтаксис) не срывает Play —
    // логируется, остальные продолжают работать.
    const int attached = m_scripting->AttachScene(scene);

    // Раскладка управления проекта — ПОСЛЕ скриптов, и это не мелочь порядка.
    // Скрипты объявляют СВОИ умолчания (BindAction в Start), а файл проекта —
    // это «как решил автор игры», и он обязан их замещать, а не дописываться к
    // ним. Иначе переназначенное в редакторе действие продолжало бы работать и
    // на старой клавише — то есть панель «Управление» выглядела бы сломанной.
    // Тот же порядок у собранной игры (см. PlayerLayer).
    if (ctx.ApplyProjectInputMapping) ctx.ApplyProjectInputMapping();

    // Состав кадра на время Play — ТОТ ЖЕ, что у собранной игры (см.
    // PlayerLayer): скрипты, физика, анимация, частицы, звук в порядке,
    // заданном один раз в RegisterCoreSystems.
    //
    // Без этой регистрации Play выглядел запущенным и не был им: AttachScript
    // выше зовёт OnStart (и в консоли честно появляется «spin.lua attached
    // to: …»), но UpdateAll не звал НИКТО — планировщик о скриптах не знал.
    if (ctx.Systems) {
        sage::CoreSystems core;
        core.Scripts = m_scripts.get();
        core.Scripting = m_scripting.get();
        core.Physics = m_physics.get();
        core.Particles = ctx.Particles;
        core.Audio = m_audio.get();
        // Анимация уже зарегистрирована набором режима правки (превью) и
        // повторной регистрацией только заменилась бы сама на себя.
        core.Animation = false;
        sage::RegisterCoreSystems(*ctx.Systems, core);
    }
    // Звуковые источники сцены оживают вместе с игрой: у кого стоит «играть
    // сразу» — зазвучал. Не в самой системе кадра, потому что «начать» это
    // событие, а система кадра — про каждый кадр: иначе источник
    // перезапускался бы шестьдесят раз в секунду.
    if (m_audio) {
        const int started = sage::audio::StartScene(scene, *m_audio);
        if (started > 0) LOG_INFO("Editor") << "Play: звуковых источников запущено: " << started;
    }

    LOG_INFO("Editor") << "Play: сцена «" << scene.Name() << "» — скриптов " << attached
                       << ", физических тел " << m_physics->BodyCount() << " ("
                       << m_physics->BackendName() << ")";
    return attached;
}

// Обратное BuildRuntime: снять всё живое, не трогая ни снимка, ни состояния
// Play. Порядок — тот же, что в Stop, и по тем же причинам (см. там).
void EditorPlaySession::TeardownRuntime(const PlayContext& ctx, Scene* scene) {
    if (ctx.Systems) {
        ctx.Systems->Remove("scripts");
        ctx.Systems->Remove("scripting");
        ctx.Systems->Remove("scripting.late");
        ctx.Systems->Remove("physics");
    }
    if (m_audio && scene) sage::audio::StopScene(*scene, *m_audio);
    if (m_scripting) m_scripting->Shutdown();
    m_scripting.reset();
    m_scripts.reset();
    m_physics.reset();
    // Шина принадлежит СЦЕНЕ, а её сейчас заменят: указатель обязан уйти раньше.
    m_input.SetEventBus(nullptr);
}

// ПЕРЕХОД НА ДРУГОЙ УРОВЕНЬ ПРЯМО В РЕДАКТОРЕ.
//
// Раньше `sage.scene.Load` в Play-режиме не делал НИЧЕГО — редактор писал в
// лог «проверяйте переходы в собранной игре». То есть самую частую ошибку
// уровня (не та сцена, не тот спавн, потерянный игрок) нельзя было увидеть там,
// где её правят: цикл «поправил — посмотрел» требовал полной сборки игры.
//
// Документ человека при этом НЕ ТРОГАЕТСЯ: Play работает с копией, и Stop
// возвращает ровно ту сцену, которая была открыта до запуска, — вместе с
// несохранённой правкой.
bool EditorPlaySession::SwitchScene(const PlayContext& ctx, const std::string& sceneName) {
    if (!Active() || !ctx.LoadSceneForPlay) return false;
    Scene* current = ctx.ScenePtr ? *ctx.ScenePtr : nullptr;

    // Пустое имя — «этот же уровень заново». Берём его из СНИМКА, а не с диска:
    // играют то, что открыто, а открытая сцена может быть ещё не сохранена —
    // перезапуск обязан вернуть именно её, а не прошлую версию файла.
    const bool restartCurrent = sceneName.empty();

    TeardownRuntime(ctx, current);
    const bool loaded = restartCurrent ? (ctx.RestoreScene && ctx.RestoreScene(m_snapshot))
                                       : ctx.LoadSceneForPlay(sceneName);
    if (!loaded) {
        LOG_ERROR("Editor") << "Play: сцена «"
                            << (restartCurrent ? std::string("(текущая)") : sceneName)
                            << "» не загрузилась — игра остановлена";
        return false;
    }
    Scene& fresh = **ctx.ScenePtr;
    BuildRuntime(ctx, fresh);
    if (ctx.ApplyProjectInputMapping) ctx.ApplyProjectInputMapping();
    return true;
}

void EditorPlaySession::StepScripts(Scene& scene, float dt) {
    if (m_scripts) m_scripts->UpdateAll(dt);
    if (!m_scripting) return;
    // Столкновения прошлого шага — до Update, как и в обычном кадре.
    if (m_physics) m_scripting->DispatchPhysicsEvents(*m_physics, scene);
    m_scripting->FixedUpdate(dt);
    m_scripting->Update(dt);
    m_scripting->LateUpdate(dt);
}

// ПАУЗА — ЭТО ОСТАНОВЛЕННЫЙ КАДР, А НЕ ЗНАЧОК.
//
// Раньше Pause/Resume только меняли поле состояния: планировщик систем звался
// каждый кадр независимо от него, а звук шёл из своего устройства. То есть в
// «паузе» продолжали идти скрипты, физика, анимация, частицы и звук — кнопка
// меняла свой вид и больше ничего. Сам кадр останавливает тот, кто гоняет
// системы; здесь — вторая половина: звук.
void EditorPlaySession::Pause() {
    if (m_state != EditorPlayState::Playing) return;
    m_state = EditorPlayState::Paused;
    // Остановленный кадр, из которого продолжает литься шум водопада, паузой не
    // выглядит. Позиция воспроизведения сохраняется — продолжаем с того же
    // места, а не с начала.
    if (m_audio) m_audio->SetAllPaused(true);
}

void EditorPlaySession::Resume() {
    if (m_state != EditorPlayState::Paused) return;
    m_state = EditorPlayState::Playing;
    if (m_audio) m_audio->SetAllPaused(false);
}

void EditorPlaySession::RequestStep() {
    // Только из паузы: «шаг» у работающей игры смысла не имеет — она и так
    // идёт, — а из режима правки шагать нечему.
    if (m_state != EditorPlayState::Paused) return;
    // Кадр ФИКСИРОВАННОЙ длительности, а не реальный dt: шаг делают, чтобы
    // разглядеть происходящее, и его величина обязана быть одинаковой, а не
    // зависеть от того, сколько миллисекунд прошло между нажатиями. 1/60 — тот
    // же шаг, которым идёт игра на обычном мониторе.
    m_pendingStep = 1.0f / 60.0f;
}

float EditorPlaySession::TakePendingStep() {
    const float step = m_pendingStep;
    m_pendingStep = 0.0f;
    return step;
}

void EditorPlaySession::Stop(const PlayContext& ctx) {
    if (!Active()) return;
    Scene* scene = ctx.ScenePtr ? *ctx.ScenePtr : nullptr;

    // ПОРЯДОК ГАШЕНИЯ — НЕ СТИЛЬ, А УСЛОВИЕ ПРАВИЛЬНОСТИ. Каждая строка ниже
    // стоит там, где стоит, потому что иначе указатель переживает свой объект,
    // и падает не там, где ошибка, и не всегда.

    // Stop — это для игры «выход»: скрипты обязаны узнать о нём раньше, чем
    // исчезнут. Без этого проверить сохранение при выходе можно было только в
    // собранной игре: в Play-режиме прогресс за последние секунды пропадал, и
    // выглядело это как «сохранение не работает в редакторе».
    if (m_scripts) m_scripts->DispatchQuit();

    // Системы снимаются ДО разрушения объектов: планировщик держит на них
    // указатель, и оставленная в кадре система обратилась бы к освобождённой
    // памяти. Снимается ровно то, что добавил Start. "particles", "animation" и
    // "audio" остаются: это превью режима правки, а не игровые системы.
    // Системы, звук, скрипты и физика снимаются одним и тем же кодом, что и при
    // переходе на другой уровень: два похожих порядка гашения — это два места,
    // где однажды разойдётся то, что обязано совпадать.
    TeardownRuntime(ctx, scene);
    // Курсор возвращается человеку: игра могла его захватить, и без этого Stop
    // оставил бы редактор без мыши.
    m_cursor.ReleaseCapture();
    // Действия прошлого запуска отпускаются здесь же: иначе клавиша, зажатая в
    // момент Stop, осталась бы нажатой до следующего Play.
    m_input.ReleaseAll();

    if (ctx.RestoreScene) ctx.RestoreScene(m_snapshot);
    m_snapshot.clear();
    m_pendingStep = 0.0f;
    m_state = EditorPlayState::Editing;
    LOG_INFO("Editor") << "Play stopped, scene restored";
}

void EditorPlaySession::UpdateUiInput(Scene& scene, const PlayUiInput& in) {
    auto uiView = scene.Registry().view<sage::ui::Element>();
    if (uiView.begin() == uiView.end()) return;

    // Захваченный курсор — режим обзора: экранной точки у мыши нет, и
    // подсвечивать ею элементы нельзя (подсветилось бы то, что под центром).
    const bool usable = in.MouseInside && !m_cursor.CursorCaptured();
    const bool down = usable && in.MouseDown;

    sage::ui::UIInputState input;
    input.Mouse = usable ? glm::vec2(in.MouseX, in.MouseY) : glm::vec2(-1.0f);
    input.MouseDown = down;
    input.MousePressed = down && !m_uiMouseWasDown;
    input.MouseReleased = !down && m_uiMouseWasDown;
    input.TypedText = in.TypedText;
    input.DeltaTime = in.DeltaTime;
    input.Wheel = usable ? in.Wheel : 0.0f;
    m_uiMouseWasDown = down;

    if (in.Focused) {
        input.Backspace = in.Backspace;
        input.Delete = in.Delete;
        input.Left = in.Left;
        input.Right = in.Right;
        input.Home = in.Home;
        input.End = in.End;
        input.Enter = in.Enter;
        input.Escape = in.Escape;
        input.Tab = in.Tab;
    }

    const sage::ui::UIInputResult result =
        sage::ui::UpdateSceneUI(scene, input, in.GameWidth, in.GameHeight);

    // Что интерфейс сцены съел, того игра не получит — так же, как в собранной
    // игре. Иначе щелчок по кнопке меню в панели Game одновременно стрелял бы,
    // и превью вело бы себя не как игра.
    uint8_t eaten = sage::input::DeviceNone;
    if (result.WantsMouse) eaten |= sage::input::DeviceMouse;
    if (result.WantsKeyboard) eaten |= sage::input::DeviceKeyboard;
    if (eaten != sage::input::DeviceNone) m_input.BlockDevices(eaten);
}
