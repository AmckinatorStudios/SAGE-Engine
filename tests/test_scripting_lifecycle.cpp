// ВЫЗОВ скриптов: что происходит с движком, когда скрипт есть, когда его нет,
// когда он сломан и когда он ломает сцену из-под самого себя.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ФАЙЛ. Соседний test_scripting.cpp проверяет API: что из Lua
// видно компоненты, иерархия, сообщения, математика. Это важная половина, но не
// та, в которой живут падения. Падения живут в ЖИЗНЕННОМ ЦИКЛЕ вызова:
//
//   • скрипт не найден или не разбирается — движок обязан сказать, ЧТО именно и
//     ГДЕ, и продолжить работу с остальными;
//   • ошибка внутри OnUpdate повторяется КАЖДЫЙ КАДР, то есть шестьдесят раз в
//     секунду. Одна опечатка забивает консоль так, что в ней не видно ничего
//     другого, — и это не косметика: именно консоль читают, когда ищут причину;
//   • скрипт уничтожает свою сущность (или чужую) прямо из OnUpdate — движок в
//     этот момент идёт по списку экземпляров;
//   • таймер заводит таймер, корутина заводит корутину — векторы
//     реаллоцируются под ногами у обхода.
//
// Ни одно из этого не проверялось. Проверяется здесь.
#include "TestFramework.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

namespace {

std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = std::string("sage_life_") + name + ".lua";
    std::ofstream f(path);
    f << body;
    return path;
}

// Ловушка сообщений лога: половина проверок здесь — про то, ЧТО движок сказал,
// а не только про то, что он не упал. Без перехвата лога это пришлось бы
// проверять глазами, то есть не проверять.
class LogCapture {
public:
    LogCapture() {
        Log::SetSink([this](LogLevel level, const std::string&, const std::string& message) {
            if (level == LogLevel::Error) m_errors.push_back(message);
            m_all.push_back(message);
        });
    }
    ~LogCapture() { Log::SetSink(nullptr); }

    int ErrorCount() const { return (int)m_errors.size(); }
    int ErrorsContaining(const std::string& needle) const {
        int n = 0;
        for (const std::string& e : m_errors) {
            if (e.find(needle) != std::string::npos) ++n;
        }
        return n;
    }
    bool AnyContains(const std::string& needle) const {
        for (const std::string& e : m_all) {
            if (e.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    void Clear() { m_errors.clear(); m_all.clear(); }

private:
    std::vector<std::string> m_errors;
    std::vector<std::string> m_all;
};

// Таблица-щуп: скрипт пишет в неё, тест читает.
//
// Просто «глобальная переменная скрипта» тут не годится, и это не мелочь теста,
// а свойство движка: каждый скрипт исполняется в СВОЕЙ среде (sol::environment),
// поэтому его `x = 1` создаёт переменную в этой среде, а не в глобальных. Так и
// задумано — иначе два скрипта с переменной `speed` затирали бы друг друга. Но
// значит, что заглянуть в скрипт снаружи можно только через объект, созданный
// ДО него: чтение из среды проваливается в глобальные, и таблицу скрипт находит.
sol::table MakeProbe(ScriptEngine& se) {
    sol::table probe = se.Lua().create_table();
    se.Lua()["probe"] = probe;
    return probe;
}

int ProbeInt(const sol::table& probe, const char* key) { return probe[key].get_or(-1); }

} // namespace

// --- Скрипта нет или он сломан ----------------------------------------------

TEST(ScriptLife_missing_file_reports_the_path_and_keeps_the_engine_usable) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("A");

    bool threw = false;
    std::string what;
    try {
        se.AttachScript(obj, "sage_life_no_such_script.lua");
    } catch (const std::exception& ex) {
        threw = true;
        what = ex.what();
    }
    CHECK_TRUE(threw);
    // Сообщение обязано называть ФАЙЛ. «Скрипт не найден» без имени — это
    // сообщение, с которым нечего делать, когда скриптов в проекте сорок.
    CHECK_TRUE(what.find("sage_life_no_such_script.lua") != std::string::npos);

    // И движок остаётся рабочим: следующий скрипт привязывается как ни в чём не
    // бывало. Иначе одна битая ссылка в сцене выключала бы скрипты целиком.
    const std::string ok = WriteScript("after_missing",
                                       "function OnUpdate(entity, dt) entity.Transform.Position.x = 5 end");
    se.AttachScript(obj, ok);
    se.UpdateAll(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.x, 5.0f, 1e-4);
    std::remove(ok.c_str());
}

TEST(ScriptLife_syntax_error_names_the_file_and_the_line) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("A");

    // Ломаем НА ВТОРОЙ строке намеренно: сообщение без номера строки
    // бесполезно ровно так же, как без имени файла.
    const std::string path = WriteScript("broken", "local x = 1\nfunction OnUpdate( end\n");
    bool threw = false;
    std::string what;
    try {
        se.AttachScript(obj, path);
    } catch (const std::exception& ex) {
        threw = true;
        what = ex.what();
    }
    std::remove(path.c_str());

    CHECK_TRUE(threw);
    CHECK_TRUE(what.find(path) != std::string::npos);
    CHECK_TRUE(what.find(":2") != std::string::npos);
}

TEST(ScriptLife_error_in_OnStart_does_not_lose_the_script) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("A");

    // OnStart падает, OnUpdate исправен. Скрипт обязан остаться привязанным:
    // «упал при старте — значит не работает вовсе» превратило бы одну ошибку
    // инициализации в молчаливое отключение поведения объекта.
    const std::string path = WriteScript("start_boom", R"(
        function OnStart(entity) error("boom in start") end
        function OnUpdate(entity, dt) entity.Transform.Position.y = 3 end
    )");
    se.AttachScript(obj, path);
    std::remove(path.c_str());

    CHECK_EQ(log.ErrorsContaining("OnStart"), 1);
    se.UpdateAll(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.y, 3.0f, 1e-4);
}

// САМОЕ ВАЖНОЕ ЗДЕСЬ.
//
// Ошибка внутри OnUpdate повторяется каждый кадр. При шестидесяти кадрах в
// секунду одна опечатка выдаёт три с половиной тысячи одинаковых строк в
// минуту: консоль редактора перестаёт быть читаемой, а вместе с ней — и все
// прочие сообщения, ради которых в неё смотрят. Найти в этом потоке ВТОРУЮ
// ошибку невозможно.
//
// Движок обязан сказать про ошибку и замолчать, а не повторять её.
TEST(ScriptLife_repeating_error_in_OnUpdate_is_reported_once_not_every_frame) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("Buggy");

    const std::string path = WriteScript("update_boom",
                                         "function OnUpdate(entity, dt) error(\"boom\") end");
    se.AttachScript(obj, path);
    std::remove(path.c_str());

    for (int i = 0; i < 120; ++i) se.UpdateAll(0.016f);

    // Одно сообщение об ошибке плюс одно о том, что скрипт отключён. Ровно два
    // на две секунды игры — а не двести сорок.
    CHECK_EQ(log.ErrorsContaining("Ошибка в OnUpdate"), 1);
    CHECK_TRUE(log.ErrorCount() <= 2);
    // И движок обязан СКАЗАТЬ, что перестал звать скрипт: молча замолчать —
    // значит оставить человека с мыслью «ошибка была одна, дальше всё хорошо».
    CHECK_TRUE(log.AnyContains("отключ"));
}

TEST(ScriptLife_broken_script_does_not_stop_the_healthy_ones) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject bad = scene.CreateObject("Bad");
    GameObject good = scene.CreateObject("Good");

    const std::string badPath = WriteScript("bad", "function OnUpdate(e, dt) error(\"nope\") end");
    const std::string goodPath =
        WriteScript("good", "function OnUpdate(e, dt) e.Transform.Position.x = e.Transform.Position.x + 1 end");
    se.AttachScript(bad, badPath);
    se.AttachScript(good, goodPath);
    std::remove(badPath.c_str());
    std::remove(goodPath.c_str());

    for (int i = 0; i < 5; ++i) se.UpdateAll(0.016f);
    // Здоровый скрипт отработал все пять кадров: отключается ТОЛЬКО виновник.
    CHECK_NEAR(good.GetTransform().Position.x, 5.0f, 1e-4);
}

TEST(ScriptLife_script_without_OnUpdate_is_legitimate) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("A");

    // «Настроил в OnStart и забыл» — нормальный вид скрипта, и ругаться на него
    // движок не должен.
    const std::string path = WriteScript("only_start",
                                         "function OnStart(entity) entity.Transform.Position.z = 7 end");
    se.AttachScript(obj, path);
    std::remove(path.c_str());
    for (int i = 0; i < 3; ++i) se.UpdateAll(0.016f);

    CHECK_NEAR(obj.GetTransform().Position.z, 7.0f, 1e-4);
    CHECK_EQ(log.ErrorCount(), 0);
}

// --- Уровневые скрипты (RunScript) ------------------------------------------

TEST(ScriptLife_level_script_gets_dt_without_entity) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Уровневый скрипт не привязан к сущности, и его OnUpdate принимает ОДИН
    // аргумент. Если движок передаст ему entity первым, dt окажется вторым, и
    // счётчик времени будет считать мусор — молча.
    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("level", R"(
        probe.elapsed = 0
        probe.calls = 0
        function OnUpdate(dt) probe.elapsed = probe.elapsed + dt probe.calls = probe.calls + 1 end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 4; ++i) se.UpdateAll(0.25f);
    CHECK_NEAR(probe["elapsed"].get_or(0.0f), 1.0f, 1e-4);
    CHECK_EQ(ProbeInt(probe, "calls"), 4);
}

// --- Скрипт ломает сцену из-под самого себя ---------------------------------

TEST(ScriptLife_script_destroying_its_own_entity_mid_update) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject self = scene.CreateObject("Self");
    GameObject other = scene.CreateObject("Other");

    const std::string selfPath = WriteScript("self_destroy", R"(
        function OnUpdate(entity, dt) DestroyObject(entity) end
    )");
    const std::string otherPath =
        WriteScript("survivor", "function OnUpdate(e, dt) e.Transform.Position.x = e.Transform.Position.x + 1 end");
    se.AttachScript(self, selfPath);
    se.AttachScript(other, otherPath);
    std::remove(selfPath.c_str());
    std::remove(otherPath.c_str());

    for (int i = 0; i < 10; ++i) se.UpdateAll(0.016f);

    // Ни падения, ни потока ошибок про мёртвую сущность: экземпляр снимается
    // после прохода, а сосед продолжает работать все десять кадров.
    CHECK_EQ(log.ErrorCount(), 0);
    CHECK_NEAR(other.GetTransform().Position.x, 10.0f, 1e-4);
}

TEST(ScriptLife_script_destroying_another_scripted_entity) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject killer = scene.CreateObject("Killer");
    GameObject victim = scene.CreateObject("Victim");
    const int victimId = victim.Id();

    const std::string killerPath = WriteScript("killer", R"(
        function OnUpdate(entity, dt)
            local v = FindObject("Victim")
            if v then DestroyObject(v) end
        end
    )");
    const std::string victimPath =
        WriteScript("victim", "function OnUpdate(e, dt) e.Transform.Position.y = e.Transform.Position.y + 1 end");
    se.AttachScript(killer, killerPath);
    se.AttachScript(victim, victimPath);
    std::remove(killerPath.c_str());
    std::remove(victimPath.c_str());

    for (int i = 0; i < 5; ++i) se.UpdateAll(0.016f);
    CHECK_FALSE(scene.Get(victimId).Valid());
    CHECK_EQ(log.ErrorCount(), 0);
}

TEST(ScriptLife_message_to_a_destroyed_entity_is_silent) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject sender = scene.CreateObject("Sender");
    GameObject target = scene.CreateObject("Target");
    const int targetId = target.Id();

    const std::string targetPath = WriteScript("msg_target", R"(
        function OnMessage(entity, name, data) end
    )");
    se.AttachScript(target, targetPath);
    std::remove(targetPath.c_str());

    scene.RemoveObject(targetId);
    // Отправка мёртвому адресату — обычное дело в игре (враг умер между кадром
    // и сообщением). Это не ошибка и ругаться на неё нельзя.
    se.Lua().script("SendMessage(" + std::to_string(targetId) + ", 'ping', 1)");
    se.UpdateAll(0.016f);
    CHECK_EQ(log.ErrorCount(), 0);
}

// --- Таймеры ----------------------------------------------------------------

TEST(ScriptLife_schedule_fires_once_at_the_right_time) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("timer_once", R"(
        probe.fired = 0
        function OnStart() Schedule(0.5, function() probe.fired = probe.fired + 1 end) end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    se.UpdateAll(0.2f);
    CHECK_EQ(ProbeInt(probe, "fired"), 0); // ещё рано
    se.UpdateAll(0.2f);
    CHECK_EQ(ProbeInt(probe, "fired"), 0);
    se.UpdateAll(0.2f);
    CHECK_EQ(ProbeInt(probe, "fired"), 1); // сработал
    for (int i = 0; i < 10; ++i) se.UpdateAll(0.2f);
    CHECK_EQ(ProbeInt(probe, "fired"), 1); // и ровно один раз
}

TEST(ScriptLife_repeat_keeps_its_period_and_cancels) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("timer_repeat", R"(
        probe.ticks = 0
        probe.id = 0
        function OnStart() probe.id = Repeat(1.0, function() probe.ticks = probe.ticks + 1 end) end

    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 30; ++i) se.UpdateAll(0.1f); // три секунды
    CHECK_EQ(ProbeInt(probe, "ticks"), 3);

    // Отмена — из Lua, а не из C++: CancelTimer это скриптовое API, и проверять
    // надо его. Скрипт исполняется в своей среде, поэтому зовём через
    // глобальную таблицу-щуп, куда он положил номер таймера.
    se.Lua().script("CancelTimer(probe.id)");
    for (int i = 0; i < 30; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "ticks"), 3); // после отмены — молчок
}

TEST(ScriptLife_timer_started_from_a_timer_does_not_fire_in_the_same_frame) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Таймер, заведённый ВНУТРИ прохода таймеров, не должен тикать этим же
    // кадром: иначе он теряет один dt и срабатывает на кадр раньше срока, а
    // цепочка из десяти таких «разворачивается» мгновенно.
    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("timer_chain", R"(
        probe.inner = 0
        function OnStart()
            Schedule(0.1, function()
                Schedule(0.1, function() probe.inner = probe.inner + 1 end)
            end)
        end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    se.UpdateAll(0.15f);                        // сработал внешний
    CHECK_EQ(ProbeInt(probe, "inner"), 0);  // внутренний ещё не должен
    se.UpdateAll(0.15f);
    CHECK_EQ(ProbeInt(probe, "inner"), 1);
}

TEST(ScriptLife_error_inside_a_timer_does_not_break_the_others) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("timer_boom", R"(
        probe.ok = 0
        function OnStart()
            Repeat(0.1, function() error("timer boom") end)
            Repeat(0.1, function() probe.ok = probe.ok + 1 end)
        end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 10; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "ok"), 10);
    // Повторяющийся таймер с ошибкой — тот же поток в консоль, что и OnUpdate.
    CHECK_TRUE(log.ErrorCount() <= 2);
}

// --- Корутины ---------------------------------------------------------------

TEST(ScriptLife_coroutine_waits_and_finishes) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("coro", R"(
        probe.stage = 0
        function OnStart()
            StartCoroutine(function()
                probe.stage = 1
                wait(0.5)
                probe.stage = 2
                wait(0.5)
                probe.stage = 3
            end)
        end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    CHECK_EQ(ProbeInt(probe, "stage"), 0); // корутина стартует с первым кадром
    se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "stage"), 1);
    // Шагов с запасом: ожидание складывается из кадровых dt, и попадание ровно
    // в границу зависит от точности float. Тест проверяет ПОРЯДОК стадий и то,
    // что ожидание вообще есть, а не арифметику кадров.
    for (int i = 0; i < 3; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "stage"), 1); // ещё ждёт
    for (int i = 0; i < 4; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "stage"), 2);
    for (int i = 0; i < 8; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "stage"), 3);

    // И после завершения ничего не тикает: корутина снята со списка.
    for (int i = 0; i < 10; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "stage"), 3);
}

TEST(ScriptLife_broken_coroutine_does_not_take_the_others_with_it) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("coro_boom", R"(
        probe.ticks = 0
        function OnStart()
            StartCoroutine(function() wait(0.1) error("coroutine boom") end)
            StartCoroutine(function()
                while true do probe.ticks = probe.ticks + 1 wait(0.1) end
            end)
        end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 10; ++i) se.UpdateAll(0.1f);
    CHECK_TRUE(ProbeInt(probe, "ticks") >= 5);
    CHECK_EQ(log.ErrorsContaining("корутин"), 1);
}

TEST(ScriptLife_coroutine_started_from_a_coroutine) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Проверка того, что вектор корутин переживает реаллокацию во время
    // собственного обхода: тело корутины заводит ещё пять.
    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("coro_nested", R"(
        probe.done = 0
        function OnStart()
            StartCoroutine(function()
                wait(0.1)
                for i = 1, 5 do
                    StartCoroutine(function() wait(0.1) probe.done = probe.done + 1 end)
                end
            end)
        end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 10; ++i) se.UpdateAll(0.1f);
    CHECK_EQ(ProbeInt(probe, "done"), 5);
}

// --- Пауза и масштаб времени в связке с вызовом -----------------------------

TEST(ScriptLife_paused_host_stops_timers_and_coroutines_too) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);

    // Пауза — это множитель кадра (см. FrameTimeScale), и хозяин кадра обязан
    // умножать на него dt. Если он это делает, замирают и таймеры, и корутины:
    // проверяем именно связку, потому что «скрипты встали, а таймеры тикают» —
    // ровно тот дефект, который выглядит как «пауза не работает».
    sol::table probe = MakeProbe(se);
    const std::string path = WriteScript("pause", R"(
        probe.ticks = 0
        function OnStart() Repeat(0.1, function() probe.ticks = probe.ticks + 1 end) end
        function OnUpdate(dt) end
    )");
    se.RunScript(path);
    std::remove(path.c_str());

    for (int i = 0; i < 5; ++i) se.UpdateAll(0.1f * se.FrameTimeScale());
    CHECK_EQ(ProbeInt(probe, "ticks"), 5);

    se.SetPaused(true);
    CHECK_NEAR(se.FrameTimeScale(), 0.0f, 1e-6);
    for (int i = 0; i < 20; ++i) se.UpdateAll(0.1f * se.FrameTimeScale());
    CHECK_EQ(ProbeInt(probe, "ticks"), 5); // на паузе время не идёт

    se.SetPaused(false);
    for (int i = 0; i < 5; ++i) se.UpdateAll(0.1f * se.FrameTimeScale());
    CHECK_EQ(ProbeInt(probe, "ticks"), 10);
}

// --- Повторная привязка -----------------------------------------------------

TEST(ScriptLife_attaching_the_same_script_twice_does_not_double_the_behaviour) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    GameObject obj = scene.CreateObject("A");

    const std::string path =
        WriteScript("twice", "function OnUpdate(e, dt) e.Transform.Position.x = e.Transform.Position.x + 1 end");
    se.AttachScript(obj, path);
    se.AttachScript(obj, path); // тот же скрипт на ту же сущность — по ошибке
    std::remove(path.c_str());

    se.UpdateAll(0.016f);
    // Объект обязан сдвинуться НА ЕДИНИЦУ. Иначе повторная привязка удваивает
    // скорость всего, что скрипт делает, а выглядит это как «физика/скорость
    // почему-то вдвое больше» — и ищется где угодно, только не здесь.
    CHECK_NEAR(obj.GetTransform().Position.x, 1.0f, 1e-4);
}

// --- Неверный тип аргумента: опечатка в скрипте не должна ронять движок ------
//
// САМАЯ ДОРОГАЯ ИЗ НАЙДЕННЫХ ЗДЕСЬ ОШИБОК. sol2 по умолчанию аргументы из Lua
// НЕ ПРОВЕРЯЕТ: функция, объявленная как (GameObject&), получив из скрипта
// число, приводила это число к указателю и разыменовывала его. То есть одна
// опечатка в игровом скрипте роняла процесс — редактор с несохранённой сценой
// или игру у игрока. Обратный случай был тише и не лучше: функция с параметром
// (int), получив сущность, молча получала ноль, и DestroyObject(entity) не
// удалял ничего, не сказав ни слова.
//
// Проверки включены сборкой (SOL_ALL_SAFETIES_ON, см. CMakeLists.txt), и этот
// тест — то, что не даст их выключить «ради скорости»: он падает вместе с
// движком, а не тихо меняет поведение.
TEST(ScriptLife_wrong_argument_type_is_a_lua_error_not_a_crash) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.CreateObject("A");

    // Каждая строка — правдоподобная опечатка: перепутаны местами аргументы,
    // передан номер вместо сущности, строка вместо числа.
    const char* mistakes[] = {
        "SetMeshCube(5)",                      // ждёт сущность, дали число
        "DestroyObject('строка')",             // ждёт сущность или номер
        "SendMessage({}, 'x', 1)",             // адресат — таблица
        "FindObject(42)",                      // ждёт имя строкой
        "Schedule('скоро', function() end)",   // ждёт секунды числом
        "local a = SpawnObject('x') a:SetParent(7)",
        "GetLighting().Sun.Direction = 'вниз'",
    };
    for (const char* code : mistakes) {
        auto result = se.Lua().safe_script(code, sol::script_pass_on_error);
        // Ошибка — да; падение — нет. Без проверок первая же строка этого
        // списка снимала весь процесс.
        CHECK_FALSE(result.valid());
    }

    // И движок после всех этих ошибок остаётся рабочим.
    GameObject obj = scene.CreateObject("Alive");
    const std::string path = WriteScript("after_type_errors",
                                         "function OnUpdate(e, dt) e.Transform.Position.x = 2 end");
    se.AttachScript(obj, path);
    std::remove(path.c_str());
    se.UpdateAll(0.016f);
    CHECK_NEAR(obj.GetTransform().Position.x, 2.0f, 1e-4);
}

// --- API без привязки: понятная ошибка вместо падения ------------------------
//
// Половина скриптового API работает только после того, как хозяин кадра отдал
// движку соответствующую систему: BindScene, BindCamera, BindAudio, BindPhysics.
// Скрипт об этом не знает и знать не должен — он просто зовёт GetCamera(). Если
// камеры нет, ответ обязан быть ошибкой Lua с внятным текстом, а не обращением
// по нулевому указателю: сцена без камеры это не редкость, а обычное состояние
// превью ассета или headless-прогона.
TEST(ScriptLife_api_without_its_binding_errors_cleanly) {
    ScriptEngine se; // НИЧЕГО не привязано
    const char* calls[] = {
        "SpawnObject('x')",               // нет сцены
        "FindObject('x')",
        "DestroyObject(1)",
        "GetCamera()",                    // нет камеры
        "PlaySound('a.wav')",             // нет звука
        "SetGravity(Vec3.new(0, -9, 0))", // нет физики
        "GetMouseDelta()",                // нет сырого ввода
    };
    for (const char* code : calls) {
        auto result = se.Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid()) std::printf("       БЕЗ ОШИБКИ: %s\n", code);
        CHECK_FALSE(result.valid());
    }

    // А ВОТ ВВОД — НАОБОРОТ, и это осознанное исключение. Опрос действий идёт
    // КАЖДЫЙ КАДР из каждого скрипта: ошибка вместо ответа означала бы поток
    // сообщений там, где правильный ответ очевиден и безобиден — «ничего не
    // нажато». Ровно это и нужно превью ассета, headless-прогону и любому
    // хозяину кадра без клавиатуры.
    auto input = se.Lua().safe_script("return IsActionDown('Jump')", sol::script_pass_on_error);
    CHECK_TRUE(input.valid());
    CHECK_FALSE(input.get<bool>());
}

// --- Обращение к УНИЧТОЖЕННОЙ сущности из скрипта ----------------------------
//
// Самая частая ошибка живого игрового кода: скрипт запомнил врага, враг умер,
// скрипт на следующем кадре трогает его Transform. Ответ обязан быть ошибкой
// Lua — с именем файла и строкой, — а не фатальным завершением процесса: у
// игрока это выглядит как вылет игры, у автора сцены в редакторе — как потеря
// несохранённой работы.
TEST(ScriptLife_touching_a_destroyed_entity_is_an_error_not_a_crash) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    scene.CreateObject("Victim");

    const char* uses[] = {
        "local e = FindObject('Victim') DestroyObject(e) return e.Transform.Position.x",
        "local e = SpawnObject('T') DestroyObject(e) e.Transform.Position.x = 1",
        "local e = SpawnObject('T') DestroyObject(e) return e.Name",
        "local e = SpawnObject('T') DestroyObject(e) e:SetParent(SpawnObject('P'))",
        "local e = SpawnObject('T') DestroyObject(e) return e:WorldPosition()",
    };
    for (const char* code : uses) {
        auto result = se.Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid()) std::printf("       МЁРТВАЯ СУЩНОСТЬ БЕЗ ОШИБКИ: %s\n", code);
        CHECK_FALSE(result.valid());
    }
}

// --- Выход из игры ------------------------------------------------------------
//
// Без хука OnQuit игра узнавала о закрытии НИКОГДА: окно закрывали крестиком,
// меню звало Application::Close(), редактор жал Stop — и во всех трёх случаях
// движок скриптов просто уничтожался. Для игры с сохранением это потеря всего,
// что случилось после последнего автосохранения, причём молчаливая.
TEST(ScriptLife_quit_hook_reaches_object_and_level_scripts_once) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    sol::table probe = MakeProbe(se);
    probe["object"] = 0;
    probe["level"] = 0;

    const std::string objectScript = WriteScript("quit_object", R"(
        function OnQuit(entity)
            probe.object = probe.object + 1
            probe.name = entity.Name
        end
    )");
    const std::string levelScript = WriteScript("quit_level", R"(
        function OnQuit()
            probe.level = probe.level + 1
        end
    )");

    se.AttachScript(scene.CreateObject("World"), objectScript);
    se.RunScript(levelScript);

    CHECK_EQ(ProbeInt(probe, "object"), 0);   // до выхода — ни разу
    se.DispatchQuit();
    CHECK_EQ(ProbeInt(probe, "object"), 1);
    CHECK_EQ(ProbeInt(probe, "level"), 1);
    // Объектный скрипт получает свою сущность — внутри OnQuit ещё можно
    // спросить у мира, что в нём произошло, и записать это в сохранение.
    CHECK_TRUE(probe["name"].get<std::string>() == "World");

    // Повторный вызов ничего не делает: путей выхода несколько (кнопка меню,
    // крестик окна), и звать хук на каждом — правильно, а сохраняться дважды —
    // нет.
    se.DispatchQuit();
    CHECK_EQ(ProbeInt(probe, "object"), 1);
    CHECK_EQ(ProbeInt(probe, "level"), 1);
}

// Ошибка в OnQuit не должна мешать выходу: игра уже закрывается, и падать на
// прощание — худшее, что можно сделать. Сообщаем и идём дальше.
TEST(ScriptLife_error_in_quit_hook_is_reported_not_fatal) {
    LogCapture log;
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    sol::table probe = MakeProbe(se);
    probe["ok"] = 0;

    se.AttachScript(scene.CreateObject("Broken"),
                    WriteScript("quit_broken", "function OnQuit() error('bang') end"));
    se.AttachScript(scene.CreateObject("Fine"),
                    WriteScript("quit_fine", "function OnQuit() probe.ok = 1 end"));

    se.DispatchQuit();
    CHECK_EQ(log.ErrorsContaining("OnQuit"), 1);
    // И сосед по сцене всё равно сохранился.
    CHECK_EQ(ProbeInt(probe, "ok"), 1);
}

// --- Шина событий -------------------------------------------------------------

TEST(Events_deliver_to_every_subscriber_and_carry_a_payload) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    scripts.Lua().script(R"(
        seen = {}
        sage.events.On("boom", function(e) seen[#seen+1] = "a" .. tostring(e.power) end)
        sage.events.On("boom", function(e) seen[#seen+1] = "b" .. tostring(e.power) end)
        sage.events.Emit("boom", {power = 7})
    )");
    sol::table seen = scripts.Lua()["seen"];
    CHECK_EQ((int)seen.size(), 2);
    CHECK_EQ(seen[1].get<std::string>(), std::string("a7"));
    CHECK_EQ(seen[2].get<std::string>(), std::string("b7"));
}

TEST(Events_can_be_unsubscribed_and_Once_fires_exactly_once) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    scripts.Lua().script(R"(
        n, m = 0, 0
        local id = sage.events.On("tick", function() n = n + 1 end)
        sage.events.Once("tick", function() m = m + 1 end)
        sage.events.Emit("tick")
        sage.events.Emit("tick")
        sage.events.Off(id)
        sage.events.Emit("tick")
    )");
    CHECK_EQ(scripts.Lua()["n"].get<int>(), 2);
    CHECK_EQ(scripts.Lua()["m"].get<int>(), 1);   // Once — ровно один раз
}

TEST(Events_survive_subscribing_from_inside_a_handler) {
    // Подписка изнутри обработчика меняет тот самый вектор, по которому идёт
    // рассылка. Без копии списка это висячая ссылка и падение.
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    scripts.Lua().script(R"(
        n = 0
        sage.events.On("x", function()
            n = n + 1
            if n < 3 then sage.events.On("x", function() n = n + 10 end) end
        end)
        sage.events.Emit("x")
        sage.events.Emit("x")
    )");
    CHECK_TRUE(scripts.Lua()["n"].get<int>() >= 2);
}

TEST(Events_stop_a_handler_loop_instead_of_crashing) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    // Два обработчика шлют событие друг другу — без потолка глубины это
    // переполнение стека C++, то есть падение движка из-за ошибки в скрипте.
    scripts.Lua().script(R"(
        sage.events.On("ping", function() sage.events.Emit("pong") end)
        sage.events.On("pong", function() sage.events.Emit("ping") end)
        sage.events.Emit("ping")
        ok = true
    )");
    CHECK_TRUE(scripts.Lua()["ok"].get<bool>());
}

TEST(Events_error_in_a_handler_is_reported_not_fatal) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    scripts.Lua().script(R"(
        after = 0
        sage.events.On("e", function() error("bang") end)
        sage.events.On("e", function() after = after + 1 end)
        sage.events.Emit("e")
    )");
    // Упавший обработчик не мешает остальным получить событие.
    CHECK_EQ(scripts.Lua()["after"].get<int>(), 1);
}

TEST(Fixed_update_runs_at_a_constant_step_regardless_of_frame_length) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    scripts.SetFixedStep(1.0f / 50.0f);

    const std::string path = WriteScript("fixed", R"(
        _G.steps = 0
        _G.total = 0.0
        function OnFixedUpdate(dt) _G.steps = _G.steps + 1; _G.total = _G.total + dt end
    )");
    scripts.RunScript(path);

    // Один длинный кадр и десять коротких — суммарно одно и то же время.
    scripts.UpdateFixed(0.1f);
    for (int i = 0; i < 10; ++i) scripts.UpdateFixed(0.01f);

    const int steps = scripts.Lua()["steps"].get<int>();
    const float total = scripts.Lua()["total"].get<float>();
    std::printf("       шагов: %d, суммарное время: %.4f (реальное 0.2)\n", steps, total);
    CHECK_EQ(steps, 10);                          // 0.2 с / 0.02 = ровно десять
    CHECK_TRUE(std::abs(total - 0.2f) < 0.001f);
}

TEST(Late_update_runs_after_every_regular_update) {
    ScriptEngine scripts;
    Scene scene;
    scripts.BindScene(scene);
    const std::string a = WriteScript("order_a", R"(
        _G.order = {}
        function OnUpdate(dt) _G.order[#_G.order+1] = "update" end
        function OnLateUpdate(dt) _G.order[#_G.order+1] = "late" end
    )");
    scripts.RunScript(a);
    const std::string b = WriteScript("order_b", R"(
        function OnUpdate(dt) _G.order[#_G.order+1] = "update2" end
    )");
    scripts.RunScript(b);

    scripts.UpdateAll(1.0f / 60.0f);
    sol::table order = scripts.Lua()["order"];
    CHECK_EQ((int)order.size(), 3);
    // Поздний хук — последним, после ОБОИХ обычных.
    CHECK_EQ(order[3].get<std::string>(), std::string("late"));
}
