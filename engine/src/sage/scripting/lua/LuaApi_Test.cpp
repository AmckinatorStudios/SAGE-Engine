#include "sage/scripting/lua/LuaInternal.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/input/InputSystem.h"
#include "sage/input/Keys.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/Element.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/components/Interact.h"

// ---------------------------------------------------------------------------
// ТЕСТЫ НА LUA — в настоящих кадрах игры (см. sage/scripting/ScriptTests.h).
//
//     function T:test_crate_falls()
//         local crate = Scene.Create("Crate")
//         ...
//         Test.wait(1.0)                       -- кадр уходит движку: физика шагает
//         Test.expect(crate.transform.position.y < 5, "ящик не упал")
//     end
//
// Проверки — функции Test.*: упавшая бросает ошибку Lua, и раннер пишет её с
// файлом и строкой ТЕСТА (уровень 2), а не этой библиотеки.
//
// Ввод — НАСТОЯЩИЙ: Test.click проходит тем же путём, что щелчок мышью
// (sage::ui::UpdateSceneUI по прямоугольнику элемента), Test.key кладёт
// событие клавиши в ту же очередь, что окно. Проверяется игра, а не обход.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

// Экран, в котором тест щёлкает по интерфейсу. Один и тот же для раскладки и
// для щелчка — иначе точка попала бы в другой прямоугольник, чем посчитан.
constexpr int kTestScreenW = 1280;
constexpr int kTestScreenH = 720;

// Проверки и ожидание — на Lua: ошибке проверки нужен уровень 2 (строка
// теста), а ожиданию — yield текущей корутины. Ни то ни другое из C++ не
// делается проще, чем здесь.
constexpr const char* kTestLibrary = R"LUA(
local Test = ...
local function show(v)
    if type(v) == "string" then return string.format("%q", v) end
    return tostring(v)
end
local function prefix(msg) return msg and (tostring(msg) .. ": ") or "" end

function Test.expect(cond, msg)
    if not cond then error(msg or "expectation failed", 2) end
    return cond
end
function Test.eq(actual, expected, msg)
    if actual ~= expected then
        error(prefix(msg) .. "expected " .. show(expected) .. ", got " .. show(actual), 2)
    end
end
function Test.ne(actual, other, msg)
    if actual == other then error(prefix(msg) .. "did not expect " .. show(other), 2) end
end
function Test.near(actual, expected, eps, msg)
    eps = eps or 1e-4
    if type(actual) ~= "number" or math.abs(actual - expected) > eps then
        error(prefix(msg) .. "expected " .. show(expected) .. " ±" .. eps .. ", got " .. show(actual), 2)
    end
end
function Test.fail(msg) error(msg or "failed", 2) end
-- Ожидаемая ошибка: fn обязана бросить, и (если задано) текст содержит needle.
function Test.errors(fn, needle, msg)
    local ok, err = pcall(fn)
    if ok then error(prefix(msg) .. "expected an error, got none", 2) end
    local text = tostring(err)
    if needle and not text:find(needle, 1, true) then
        error(prefix(msg) .. "error " .. show(text) .. " does not mention " .. show(needle), 2)
    end
    return text
end
-- Кадр — движку. Только внутри теста (он идёт корутиной).
function Test.wait(seconds) coroutine.yield({seconds = seconds or 0}) end
function Test.frames(n) coroutine.yield({frames = n or 1}) end
function Test.waitUntil(pred, timeout, msg)
    timeout = timeout or 5
    local start = Test.elapsed()
    while not pred() do
        if Test.elapsed() - start > timeout then
            error(prefix(msg) .. "condition not met in " .. timeout .. " s", 2)
        end
        coroutine.yield({frames = 1})
    end
end
-- Сколько даётся одному тесту (по умолчанию 30 с).
function Test.timeout(seconds) coroutine.yield({timeout = seconds}) end

-- Один тест целиком — ОДНОЙ корутиной: before_each, тест и after_each могут
-- ждать кадры одинаково (уборка «отпустить клавишу и дождаться кадра»).
-- after_each идёт и после упавшего теста: иначе мусор упавшего ломает соседей.
function Test.__run(self, name, class)
    local before, test, after = class.before_each, class[name], class.after_each
    if before then before(self) end
    local ok, err = pcall(test, self)
    if after then
        local ok2, err2 = pcall(after, self)
        if ok and not ok2 then ok, err = false, "after_each: " .. tostring(err2) end
    end
    if not ok then error(err, 0) end
end
)LUA";

GameObject Need(const sol::object& o, const char* who) {
    if (!o.is<ObjectRef>()) throw std::runtime_error(std::string(who) + ": ожидается объект сцены");
    const GameObject g = o.as<ObjectRef>().Obj;
    if (!g.Valid()) throw std::runtime_error(std::string(who) + ": объект уже уничтожен");
    return g;
}

// Центр элемента на тестовом экране; false — элемент не показан.
bool ElementPoint(Scene& scene, entt::entity e, float t, glm::vec2& out) {
    for (const sage::ui::ElementRect& r :
         sage::ui::SolveSceneRects(scene, kTestScreenW, kTestScreenH, false)) {
        if (r.Entity != e) continue;
        out = {r.Rect.x + r.Rect.w * t, r.Rect.y + r.Rect.h * 0.5f};
        return true;
    }
    return false;
}

sage::ui::UIInputResult Ui(Scene& scene, const sage::ui::UIInputState& in) {
    return sage::ui::UpdateSceneUI(scene, in, kTestScreenW, kTestScreenH);
}

} // namespace

void RegisterTest(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;
    sol::table test = lua.create_table();

    // Сколько игрового времени идёт текущий тест (для waitUntil).
    test.set_function("elapsed", [self]() -> float {
        return self->m_tests.empty() ? 0.0f : self->m_tests.front().Elapsed;
    });
    test.set_function("log", [self](sol::variadic_args va) {
        std::string line;
        sol::protected_function tostr = self->Lua()["tostring"];
        for (auto v : va) {
            if (!line.empty()) line += " ";
            sol::protected_function_result r = tostr(sol::object(v));
            line += r.valid() ? r.get<std::string>() : std::string("?");
        }
        LOG_INFO("Test") << line;
    });

    // --- Интерфейс: щелчок, набор, ползунок — настоящим путём -----------------
    //
    // true — щелчок дошёл до элемента; false — элемента на экране нет (спрятан,
    // выключен, за пределами). Сигналы (clicked и прочие) приходят ДО возврата.
    test.set_function("click", [self](sol::object target) {
        const GameObject g = Need(target, "Test.click");
        Scene* scene = SceneOrThrow(*self, "Test.click");
        glm::vec2 p;
        if (!ElementPoint(*scene, g.Entity(), 0.5f, p)) return false;
        sage::ui::UIInputState down;
        down.Mouse = p;
        down.MouseDown = true;
        down.MousePressed = true;
        Ui(*scene, down);
        sage::ui::UIInputState up;
        up.Mouse = p;
        up.MouseReleased = true;
        Ui(*scene, up);
        return true;
    });
    // Набрать текст в поле: щелчок (фокус) и символы одним кадром.
    test.set_function("type", [self](sol::object target, const std::string& text) {
        const GameObject g = Need(target, "Test.type");
        Scene* scene = SceneOrThrow(*self, "Test.type");
        glm::vec2 p;
        if (!ElementPoint(*scene, g.Entity(), 0.5f, p)) return false;
        sage::ui::UIInputState down;
        down.Mouse = p;
        down.MouseDown = true;
        down.MousePressed = true;
        Ui(*scene, down);
        sage::ui::UIInputState up;
        up.Mouse = p;
        up.MouseReleased = true;
        Ui(*scene, up);
        sage::ui::UIInputState typing;
        typing.Mouse = p;
        typing.TypedText = text;
        Ui(*scene, typing);
        return true;
    });
    // Потянуть ползунок в долю t (0..1) его ширины.
    test.set_function("slide", [self](sol::object target, float t) {
        const GameObject g = Need(target, "Test.slide");
        Scene* scene = SceneOrThrow(*self, "Test.slide");
        glm::vec2 p;
        if (!ElementPoint(*scene, g.Entity(), glm::clamp(t, 0.0f, 1.0f), p)) return false;
        sage::ui::UIInputState down;
        down.Mouse = p;
        down.MouseDown = true;
        down.MousePressed = true;
        Ui(*scene, down);
        sage::ui::UIInputState hold;
        hold.Mouse = p;
        hold.MouseDown = true;
        Ui(*scene, hold);
        sage::ui::UIInputState up;
        up.Mouse = p;
        up.MouseReleased = true;
        Ui(*scene, up);
        return true;
    });
    // Навести курсор (hovered) или увести его (unhovered: target = nil).
    test.set_function("hover", [self](sol::object target) {
        Scene* scene = SceneOrThrow(*self, "Test.hover");
        sage::ui::UIInputState move;
        if (target.valid() && target.get_type() != sol::type::lua_nil) {
            const GameObject g = Need(target, "Test.hover");
            if (!ElementPoint(*scene, g.Entity(), 0.5f, move.Mouse)) return false;
        }
        Ui(*scene, move);
        return true;
    });

    // --- Клавиши: событие в очередь окна; видно со следующего кадра ----------
    test.set_function("key", [self](const std::string& name, sol::optional<bool> downArg) {
        sage::input::InputSystem* in = self->Services().Input;
        if (!in) throw std::runtime_error("Test.key: ввод не привязан");
        const sage::input::Key key = sage::input::ParseKey(name);
        if (key == sage::input::Key::Unknown)
            throw std::runtime_error("Test.key: неизвестная клавиша '" + name + "'");
        const bool down = downArg.value_or(true);
        in->Push(down ? sage::input::InputEvent::KeyDown(key) : sage::input::InputEvent::KeyUp(key));
    });

    lua["Test"] = test;
    // Библиотека получает таблицу аргументом: загрузить и позвать с ней.
    sol::load_result chunk = lua.load(kTestLibrary, "=SageTest");
    if (chunk.valid()) {
        sol::protected_function fn = chunk;
        fn(test);
    }

    // --- UI.create: элемент интерфейса из скрипта -----------------------------
    //
    // Программная сборка интерфейса — и тестам, и игре: «кнопка Play в
    // панели» одной строкой, тем же набором заготовок, что в редакторе.
    sol::object uiObj = lua["UI"];
    if (uiObj.get_type() == sol::type::table) {
        sol::table ui = uiObj.as<sol::table>();
        ui.set_function("create", [self](sol::variadic_args va) -> sol::object {
            size_t b = va.size() > 0 && va[0].get_type() == sol::type::table ? 1u : 0u;
            Scene* scene = SceneOrThrow(*self, "UI.create");
            sol::object presetArg = b < va.size() ? sol::object(va[b]) : sol::object(sol::lua_nil);
            if (!presetArg.is<std::string>())
                throw std::runtime_error("UI.create: первым аргументом — вид элемента (\"Button\", \"Panel\", …)");
            const std::string preset = presetArg.as<std::string>();
            std::string name = preset;
            if (b + 1 < va.size() && va[b + 1].is<std::string>()) name = va[b + 1].as<std::string>();
            GameObject obj = scene->CreateObject(name);
            if (!sage::ui::ApplyPreset(*scene, obj.Entity(), preset)) {
                scene->RemoveObject(obj.Id());
                throw std::runtime_error("UI.create: нет вида элемента '" + preset + "'");
            }
            if (b + 2 < va.size() && va[b + 2].is<ObjectRef>()) {
                const GameObject parent = va[b + 2].as<ObjectRef>().Obj;
                if (parent.Valid()) scene->SetParent(obj.Entity(), parent.Entity());
            }
            return self->Wrap(obj);
        });
    }
}

// =============================================================================
//  Раннер
// =============================================================================

int Backend::QueueTests(InstanceId id, const std::string& path) {
    Instance* inst = Get(id);
    if (!inst) return 0;
    // Порядок — как в файле; методы, которых разбор не увидел (объявлены
    // присваиванием), — следом, по имени.
    std::vector<std::string> names;
    auto isTest = [](const std::string& n) { return n.rfind("test", 0) == 0; };
    for (const std::string& m : inst->Methods)
        if (isTest(m) && inst->Class[m].get_type() == sol::type::function &&
            std::find(names.begin(), names.end(), m) == names.end())
            names.push_back(m);
    std::vector<std::string> extra;
    for (auto& [k, v] : inst->Class) {
        if (!k.is<std::string>() || v.get_type() != sol::type::function) continue;
        const std::string n = k.as<std::string>();
        if (isTest(n) && std::find(names.begin(), names.end(), n) == names.end()) extra.push_back(n);
    }
    std::sort(extra.begin(), extra.end());
    names.insert(names.end(), extra.begin(), extra.end());
    for (const std::string& n : names) {
        PendingTest t;
        t.Inst = id;
        t.Script = path;
        t.Name = n;
        m_tests.push_back(std::move(t));
    }
    return (int)names.size();
}

bool Backend::StepTest(PendingTest& t, float dt, TestResult& out) {
    out.Script = t.Script;
    out.Name = t.Name;
    auto finish = [&](bool passed, const std::string& message, const std::string& file, int line) {
        out.Passed = passed;
        out.Message = message;
        out.File = file;
        out.Line = line;
        out.Seconds = t.Elapsed;
        out.Frames = t.Frames;
        return true;
    };
    Instance* inst = Get(t.Inst);
    if (!inst) return finish(false, "скрипт теста уничтожен посреди теста", t.Script, 0);
    CurrentScope scope(*this, t.Inst);

    if (!t.Started) {
        t.Started = true;
        sol::object run = (*m_lua)["Test"]["__run"];
        if (run.get_type() != sol::type::function)
            return finish(false, "библиотека Test не загружена", t.Script, 0);
        t.Thread = sol::thread::create(*m_lua);
        t.Co = sol::coroutine(t.Thread.state(), run.as<sol::protected_function>());
    } else {
        t.Elapsed += dt;
        ++t.Frames;
        if (t.Elapsed > t.Timeout)
            return finish(false, "тайм-аут: тест не закончился за " + std::to_string((int)t.Timeout) + " с",
                          t.Script, 0);
        if (t.WaitFrames > 0 && --t.WaitFrames > 0) return false;
        if (t.Wait > 0.0f) {
            t.Wait -= dt;
            if (t.Wait > 0.0f) return false;
        }
    }

    // Шаг корутины. Первый — с self (это метод), дальше — с dt кадра.
    sol::protected_function_result r =
        t.Frames == 0 ? t.Co(inst->Self, t.Name, inst->Class) : t.Co(dt);
    if (r.status() == sol::call_status::yielded) {
        sol::object y = r.get<sol::object>();
        t.Wait = 0.0f;
        t.WaitFrames = 1;
        if (y.get_type() == sol::type::table) {
            sol::table req = y.as<sol::table>();
            if (sol::optional<float> s = req["seconds"]) {
                t.Wait = std::max(0.0f, *s);
                t.WaitFrames = 0;
            }
            if (sol::optional<int> f = req["frames"]) t.WaitFrames = std::max(1, *f);
            if (sol::optional<float> to = req["timeout"]) {
                t.Timeout = std::max(0.1f, *to);
                t.WaitFrames = 0;   // смена тайм-аута — не повод пропускать кадр
                return StepTest(t, 0.0f, out);
            }
        }
        return false;
    }
    if (!r.valid()) {
        ScriptError err;
        Explain(r, t.Script, err);
        return finish(false, err.Message, err.File, err.Line);
    }
    return finish(true, {}, {}, 0);
}

void Backend::TickTests(float dt, std::vector<TestResult>& finished) {
    // По одному тесту за раз: тесты делят сцену и ввод, и два щелчка разных
    // тестов в одном кадре проверяли бы не то, что написано ни в одном из них.
    while (!m_tests.empty()) {
        TestResult out;
        if (!StepTest(m_tests.front(), dt, out)) return;
        finished.push_back(std::move(out));
        m_tests.pop_front();
        dt = 0.0f;   // следующий тест начинается в этом же кадре, но время не делит
    }
}

} // namespace sage::scripting::lua
