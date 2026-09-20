#pragma once
#include <filesystem>
#include <memory>

#include "sage/render/DebugLines.h"
#include "sage/scripting/ScriptRuntime.h"

class Scene;
class PhysicsScene;

// ---------------------------------------------------------------------------
// СИСТЕМА СКРИПТИНГА — то, с чем разговаривает движок.
//
//      Engine Core  →  ScriptingSystem  →  ScriptRuntime  →  LanguageBackend
//                                                               └→ LuaBackend
//
// Здесь и только здесь решается, ЧТО и В КАКОМ ПОРЯДКЕ происходит в кадре:
// Start у новых объектов, Update, постоянный шаг FixedUpdate, LateUpdate после
// всех Update, события столкновений и зон до Update того же кадра. Ни одного
// понятия конкретного языка тут нет — Lua подключается одной строкой
// AddBackend(MakeLuaBackend(...)), и ровно так же подключится C#.
//
// ПОЧЕМУ НЕ «ДВИЖОК LUA». Прежний ScriptEngine был и языком, и рантаймом, и
// API движка сразу: 6000 строк, где sol2 упоминался в заголовке, который
// включают редактор, сериализатор и половина систем. Добавить второй язык там
// означало переписать всё, что его включает, — то есть никогда.
// ---------------------------------------------------------------------------
namespace sage::scripting {

class ScriptingSystem {
public:
    ScriptingSystem();
    ~ScriptingSystem();
    ScriptingSystem(const ScriptingSystem&) = delete;
    ScriptingSystem& operator=(const ScriptingSystem&) = delete;

    // --- Настройка -----------------------------------------------------------
    void AddBackend(std::unique_ptr<LanguageBackend> backend) {
        m_runtime.AddBackend(std::move(backend));
    }
    // Службы движка, которые получат скрипты. Часы подставляются сами: время
    // кадра принадлежит системе, а не тому, кто её настраивает.
    void Bind(ScriptServices services);
    void SetProjectDir(const std::filesystem::path& dir) { m_runtime.SetProjectDir(dir); }
    void SetFixedStep(float seconds) {
        if (seconds > 0.0f) m_clock.FixedDelta = seconds;
    }
    void SetTimeScale(float scale) { m_clock.Scale = scale < 0.0f ? 0.0f : scale; }
    float TimeScale() const { return m_clock.Scale; }
    const ScriptClock& Clock() const { return m_clock; }

    // Отладочная графика, заказанная скриптами этого кадра. Читает её тот, кто
    // рисует (вьюпорт редактора, панель Game, собранная игра); гасит отжившее —
    // сама система, в Update.
    sage::render::DebugLines& Debug() { return m_debug; }
    const sage::render::DebugLines& Debug() const { return m_debug; }

    ScriptRuntime& Runtime() { return m_runtime; }
    const ScriptRuntime& Runtime() const { return m_runtime; }

    // --- Сцена ---------------------------------------------------------------

    // Привязывает скрипты ко всем сущностям со ScriptComponent. Ошибка в одном
    // скрипте (нет файла, синтаксис) не срывает запуск: остальные работают,
    // причина уходит в консоль. Возвращает, сколько привязано.
    int AttachScene(Scene& scene);

    // Один объект — например, только что созданный из префаба.
    bool Attach(GameObject object);
    void Detach(GameObject object) {
        if (object.Valid()) m_runtime.Detach(object.Entity());
    }

    // --- Кадр ----------------------------------------------------------------

    // Update всех скриптов + тик рантаймов языков. dt — НЕмасштабированный:
    // масштаб времени применяет система, потому что Time.timeScale обязан
    // значить одно и то же для всех языков сразу.
    void Update(float dt);
    // Постоянный шаг: накапливает остаток кадра и зовёт FixedUpdate столько
    // раз, сколько шагов уместилось.
    void FixedUpdate(float dt);
    // После всех Update кадра: камере, следящей за игроком, нужен порядок, а
    // между скриптами он не определён.
    void LateUpdate(float dt);

    // Столкновения и зоны прошлого шага физики → OnCollision*/OnTrigger*.
    // Зовётся ПОСЛЕ физики, но ДО Update: скрипт обязан отреагировать на удар
    // в том же кадре, в котором удар случился, иначе взрыв отстаёт на кадр.
    void DispatchPhysicsEvents(PhysicsScene& physics, Scene& scene);

    // Событие анимации → OnAnimationEvent(name) у скрипта этой сущности.
    void DispatchAnimationEvent(GameObject object, const std::string& name);

    // --- Смена сцены --------------------------------------------------------
    //
    // ПОЧЕМУ ЗАПРОС, А НЕ ДЕЙСТВИЕ. Скрипт зовёт `scene:Load("level2")` изнутри
    // Update — то есть в момент, когда движок ИДЁТ ПО СУЩНОСТЯМ ЭТОЙ ЖЕ СЦЕНЫ,
    // а сам скрипт держит на них ссылки. Загрузить новую сцену прямо там значит
    // уничтожить реестр под ногами у обхода и оставить скрипту висячий объект:
    // падение в лучшем случае, тихая порча памяти в худшем.
    //
    // Поэтому запрос ЗАПОМИНАЕТСЯ, а выполняет его хозяин кадра (плеер или
    // Play-режим редактора) между кадрами, когда ни один скрипт не исполняется.
    void RequestScene(const std::string& name);
    // Перезагрузить ТЕКУЩУЮ (начать уровень заново).
    void RequestSceneReload() { RequestScene(std::string()); }

    // Забирающий геттер: запрос действует ОДИН раз. Скрипт, попросивший сменить
    // сцену дважды за кадр, не должен получить две загрузки. Пустое имя в
    // out — «перезагрузить текущую».
    bool TakeSceneRequest(std::string& name);

    // Горячая перезагрузка изменённых файлов (редактор зовёт каждый кадр).
    int ReloadChanged() { return m_runtime.ReloadChanged(); }

    // Всё снять: OnDisable, OnDestroy, освобождение экземпляров.
    void Shutdown();

private:
    ScriptRuntime m_runtime;
    ScriptClock m_clock;
    sage::render::DebugLines m_debug;
    float m_fixedAccum = 0.0f;
    std::string m_pendingScene;
    bool m_sceneRequested = false;
    // Максимум шагов постоянного шага за кадр: после долгой паузы (перетащили
    // окно, открыли меню) накопитель иначе требует сотни шагов подряд, и игра
    // «догоняет» время, повиснув на секунду.
    static constexpr int kMaxFixedSteps = 8;
};

} // namespace sage::scripting
