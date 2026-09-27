// Сигналы объектов и связи (sage/events/Events.h, sage/scene/Signals.h,
// scripting/lua/LuaApi_Signals.cpp, ScriptingSystem::InstallLinks).
//
// Проверяется то, ради чего система существует:
//   • connect / disconnect / once / несколько слушателей / данные события;
//   • сигнал адресован ОБЪЕКТУ: clicked одной кнопки не будит другую;
//   • своё событие (player_died) работает так же, как встроенное;
//   • уничтожение объекта снимает его подписки — и те, что НА нём, и те, что
//     завёл его скрипт;
//   • связь из инспектора («On Click → Menu.start_game()») и подписка из Lua —
//     один механизм, и оба доходят;
//   • ошибки называются словами: удалённая цель, нет метода, мёртвый объект,
//     повторный disconnect, плохое имя события, не та функция.
#include "TestFramework.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "sage/core/Log.h"
#include "sage/events/Events.h"
#include "sage/physics/PhysicsComponents.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

using sage::events::Bus;
using sage::events::Event;
using sage::vars::Value;

namespace {

class Errors {
public:
    Errors() {
        Log::SetSink([this](LogLevel level, const std::string&, const std::string& message) {
            if (level == LogLevel::Error || level == LogLevel::Warn) m_all.push_back(message);
        });
    }
    ~Errors() { Log::SetSink(nullptr); }
    int Containing(const std::string& needle) const {
        int n = 0;
        for (const std::string& e : m_all)
            if (e.find(needle) != std::string::npos) ++n;
        return n;
    }
    size_t Size() const { return m_all.size(); }

private:
    std::vector<std::string> m_all;
};

std::string WriteScript(const std::string& name, const std::string& body) {
    const std::string path = "sage_signals_" + name + ".lua";
    std::ofstream f(path, std::ios::binary);
    f << body;
    return path;
}

std::unique_ptr<sage::scripting::ScriptingSystem> MakeSystem(Scene& scene) {
    auto sys = std::make_unique<sage::scripting::ScriptingSystem>();
    sys->AddBackend(sage::scripting::MakeLuaBackend({}));
    sage::scripting::ScriptServices services;
    services.ScenePtr = &scene;
    sys->Bind(services);
    return sys;
}

// Кнопка на весь угол экрана: попасть по ней можно, не считая координат.
GameObject MakeButton(Scene& scene, const char* name) {
    GameObject e = scene.CreateObject(name);
    sage::ui::Element t;
    t.Anchor = UIAnchor::TopLeft;
    t.Position = {0.0f, 0.0f};
    t.Size = {200.0f, 100.0f};
    scene.Registry().emplace<sage::ui::Element>(e.Entity(), t);
    scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    scene.Registry().emplace<sage::ui::Interactable>(e.Entity());
    scene.Registry().emplace<sage::ui::Label>(e.Entity()).Text = "Play";
    return e;
}

void Click(Scene& scene) {
    sage::ui::UIInputState down;
    down.Mouse = {50.0f, 50.0f};
    down.MouseDown = true;
    down.MousePressed = true;
    sage::ui::UpdateSceneUI(scene, down, 800, 600);
    sage::ui::UIInputState up;
    up.Mouse = down.Mouse;
    up.MouseReleased = true;
    sage::ui::UpdateSceneUI(scene, up, 800, 600);
}

// Скрипт сообщает о происходящем ГЛОБАЛЬНЫМ событием "report": тест слышит его
// тем же путём, каким игру слышит C++-подсистема, а не подглядывает в Lua.
struct Reports {
    std::vector<std::string> Got;
    explicit Reports(Scene& scene) {
        scene.Events.On("report", [this](const Event& e) { Got.push_back(e.Arg.AsString()); });
    }
    bool Has(const std::string& s) const {
        for (const std::string& g : Got)
            if (g.find(s) != std::string::npos) return true;
        return false;
    }
};

GameObject AddScript(Scene& scene, const char* objectName, const std::string& path) {
    GameObject o = scene.CreateObject(objectName);
    scene.Registry().emplace<ScriptComponent>(o.Entity()).Path = path;
    return o;
}

} // namespace

// ===========================================================================
//  ЯДРО: соединения шины
// ===========================================================================

TEST(Signals_connect_delivers_to_every_listener_with_the_data) {
    Bus bus;
    int a = 0, b = 0;
    std::string seen;
    bus.Connect(7, "clicked", [&](const Event& e) { ++a; seen = e.Arg.AsString(); });
    bus.Connect(7, "clicked", [&](const Event&) { ++b; });
    bus.EmitSignal(7, "clicked", Value(std::string("левой")));
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 1);
    CHECK_EQ(seen, std::string("левой"));
    CHECK_EQ(bus.Count(7, "clicked"), 2);
}

TEST(Signals_disconnect_stops_delivery_and_a_repeat_is_harmless) {
    Bus bus;
    int n = 0, other = 0;
    const int id = bus.Connect(1, "hit", [&](const Event&) { ++n; });
    bus.Connect(1, "hit", [&](const Event&) { ++other; });
    CHECK_TRUE(id > 0);
    CHECK_TRUE(bus.IsConnected(id));
    CHECK_TRUE(bus.Disconnect(id));
    CHECK_FALSE(bus.IsConnected(id));
    // Повторное снятие — не ошибка и не трогает соседей.
    CHECK_FALSE(bus.Disconnect(id));
    CHECK_FALSE(bus.Disconnect(0));
    bus.EmitSignal(1, "hit");
    CHECK_EQ(n, 0);
    CHECK_EQ(other, 1);
}

TEST(Signals_once_fires_exactly_once_even_if_the_handler_re_emits) {
    Bus bus;
    int n = 0;
    sage::events::ConnectOptions opt;
    opt.Once = true;
    const int id = bus.Connect(3, "landed", [&](const Event&) {
        ++n;
        bus.EmitSignal(3, "landed");   // тот же сигнал изнутри — второй раз не зовёт
    }, opt);
    bus.EmitSignal(3, "landed");
    bus.EmitSignal(3, "landed");
    CHECK_EQ(n, 1);
    CHECK_FALSE(bus.IsConnected(id));
}

// Сигнал адресован объекту: clicked у кнопки 1 не слышат подписанные на
// кнопку 2 и на глобальное событие с тем же именем.
TEST(Signals_are_addressed_by_object) {
    Bus bus;
    int one = 0, two = 0, global = 0;
    bus.Connect(1, "clicked", [&](const Event&) { ++one; });
    bus.Connect(2, "clicked", [&](const Event&) { ++two; });
    bus.On("clicked", [&](const Event&) { ++global; });
    bus.EmitSignal(1, "clicked");
    CHECK_EQ(one, 1);
    CHECK_EQ(two, 0);
    CHECK_EQ(global, 0);
    bus.Emit("clicked");
    CHECK_EQ(global, 1);
    CHECK_EQ(one, 1);
}

// Своё событие — тот же механизм, что встроенное: движку не нужно знать имя
// заранее.
TEST(Signals_custom_events_work_like_builtin_ones) {
    Bus bus;
    int died = 0;
    int score = 0;
    bus.Connect(42, "player_died", [&](const Event& e) {
        ++died;
        score = e.Arg.AsInt();
    });
    bus.EmitSignal(42, "player_died", Value(17));
    CHECK_EQ(died, 1);
    CHECK_EQ(score, 17);
    CHECK_EQ(bus.Names(42).size(), (size_t)1);
}

// Сосед, отписанный обработчиком выше по списку, замолкает СРАЗУ — в этой же
// рассылке, а не «со следующего раза».
TEST(Signals_a_neighbour_disconnected_during_dispatch_is_not_called) {
    Bus bus;
    int second = 0;
    int secondId = 0;
    bus.Connect(1, "x", [&](const Event&) { bus.Disconnect(secondId); });
    secondId = bus.Connect(1, "x", [&](const Event&) { ++second; });
    bus.EmitSignal(1, "x");
    CHECK_EQ(second, 0);
}

// Упавший обработчик не отменяет остальных и не рвёт рассылку.
TEST(Signals_a_throwing_handler_does_not_stop_the_others) {
    Errors log;
    Bus bus;
    int after = 0;
    bus.Connect(1, "x", [&](const Event&) { throw std::runtime_error("сломался"); });
    bus.Connect(1, "x", [&](const Event&) { ++after; });
    bus.EmitSignal(1, "x");
    CHECK_EQ(after, 1);
    CHECK_TRUE(log.Containing("сломался") > 0);
}

TEST(Signals_an_invalid_name_or_handler_is_refused_with_a_message) {
    Errors log;
    Bus bus;
    CHECK_EQ(bus.Connect(1, "", [](const Event&) {}), 0);
    CHECK_TRUE(log.Containing("пустое имя") > 0);
    CHECK_EQ(bus.Connect(1, "x", nullptr), 0);
    CHECK_TRUE(log.Containing("обработчик пуст") > 0);
    CHECK_TRUE(sage::signals::IsValidName("player_died"));
    CHECK_FALSE(sage::signals::IsValidName("clicked "));
    CHECK_FALSE(sage::signals::IsValidName(""));
}

// Уничтожение объекта снимает подписки на ЕГО сигналы — вместе с детьми.
TEST(Signals_destroying_an_object_drops_its_connections) {
    Scene scene("s");
    GameObject panel = scene.CreateObject("Panel");
    GameObject button = scene.CreateObject("Button");
    scene.SetParent(button.Entity(), panel.Entity());
    const int panelId = panel.Id(), buttonId = button.Id();
    int calls = 0;
    const int a = scene.Events.Connect(buttonId, "clicked", [&](const Event&) { ++calls; });
    const int b = scene.Events.Connect(panelId, "shown", [&](const Event&) { ++calls; });
    scene.RemoveObject(panelId);
    CHECK_FALSE(scene.Events.IsConnected(a));
    CHECK_FALSE(scene.Events.IsConnected(b));
    scene.Events.EmitSignal(buttonId, "clicked");
    CHECK_EQ(calls, 0);
}

TEST(Signals_components_declare_their_signals) {
    Scene scene("s");
    GameObject button = MakeButton(scene, "Play");
    GameObject rock = scene.CreateObject("Rock");
    CHECK_TRUE(sage::signals::IsDeclared(scene.Registry(), button.Entity(), "clicked"));
    CHECK_TRUE(sage::signals::IsDeclared(scene.Registry(), button.Entity(), "hovered"));
    CHECK_FALSE(sage::signals::IsDeclared(scene.Registry(), rock.Entity(), "clicked"));
    // Свой компонент объявляет свои сигналы одной строкой, без правки движка.
    struct Door {};
    sage::signals::Declare("Door", [](const entt::registry& r, entt::entity e) { return r.all_of<Door>(e); },
                           {{"opened", "", "The door opened"}});
    scene.Registry().emplace<Door>(rock.Entity());
    CHECK_TRUE(sage::signals::IsDeclared(scene.Registry(), rock.Entity(), "opened"));
    CHECK_EQ(sage::signals::Title("clicked"), std::string("On Click"));
    CHECK_EQ(sage::signals::Title("player_died"), std::string("On Player Died"));
}

// ===========================================================================
//  LUA: button.clicked:connect и остальное
// ===========================================================================

// Скрипт меню управляет кнопкой, на которой сам НЕ висит: подписка и логика —
// в одном месте, кнопка — только кнопка.
TEST(Signals_lua_connect_receives_the_event_from_another_object) {
    Scene scene("ui");
    GameObject play = MakeButton(scene, "Play");
    Reports reports(scene);
    const std::string path = WriteScript("menu", R"(
local Menu = {}
function Menu:Start()
    local play = UI.get("Play")
    play.clicked:connect(function(event, data)
        Events.emit("report", "clicked:" .. event.name .. ":" .. event.sender.name)
    end)
end
return Menu
)");
    AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    CHECK_EQ(sys->AttachScene(scene), 1);
    Click(scene);
    CHECK_TRUE(reports.Has("clicked:clicked:Play"));
    (void)play;
    std::remove(path.c_str());
}

TEST(Signals_lua_disconnect_once_and_connected) {
    Scene scene("ui");
    MakeButton(scene, "Play");
    Reports reports(scene);
    const std::string path = WriteScript("conn", R"(
local M = {}
function M:Start()
    local play = UI.get("Play")
    local c = play.clicked:connect(function() Events.emit("report", "persistent") end)
    play.clicked:once(function() Events.emit("report", "once") end)
    self.c = c
end
function M:Drop()
    local first = self.c:disconnect()
    local second = self.c:disconnect()     -- повторно: безопасно, false
    Events.emit("report", "drop:" .. tostring(first) .. ":" .. tostring(second) .. ":" ..
                tostring(self.c.connected))
end
return M
)");
    GameObject menu = AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);

    Click(scene);
    CHECK_EQ((int)reports.Got.size(), 2);        // persistent + once
    Click(scene);
    CHECK_EQ((int)reports.Got.size(), 3);        // once больше не зовётся
    sys->Runtime().Invoke(menu.Entity(), "Drop", {});
    CHECK_TRUE(reports.Has("drop:true:false:false"));
    reports.Got.clear();
    Click(scene);
    CHECK_EQ((int)reports.Got.size(), 0);
    std::remove(path.c_str());
}

// obj:on / obj:emit — своё событие с данными-таблицей: таблица доходит КАК ЕСТЬ.
TEST(Signals_lua_custom_event_carries_a_table) {
    Scene scene("s");
    Reports reports(scene);
    const std::string path = WriteScript("custom", R"(
local P = {}
function P:Start()
    self:on("player_died", function(event, data)
        Events.emit("report", "died:" .. data.score .. ":" .. event.data.cause)
    end)
    Events.on("level_done", function(event, data) Events.emit("report", "level:" .. data) end)
    self:emit("player_died", {score = 10, cause = "lava"})
    Events.emit("level_done", "forest")
end
return P
)");
    AddScript(scene, "Player", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    CHECK_TRUE(reports.Has("died:10:lava"));
    CHECK_TRUE(reports.Has("level:forest"));
    std::remove(path.c_str());
}

// Уничтожили объект со скриптом — его подписки ушли вместе с ним: кнопка,
// нажатая после этого, не зовёт мёртвый код и не сыплет ошибками.
TEST(Signals_lua_connections_die_with_their_script_object) {
    Errors log;
    Scene scene("ui");
    MakeButton(scene, "Play");
    Reports reports(scene);
    const std::string path = WriteScript("owner", R"(
local M = {}
function M:Start()
    UI.get("Play").clicked:connect(function() Events.emit("report", "alive") end)
end
function M:Update(dt) end
return M
)");
    GameObject menu = AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    Click(scene);
    CHECK_EQ((int)reports.Got.size(), 1);

    const int menuId = menu.Id();
    scene.RemoveObject(menuId);
    // В этом же кадре — до уборки — обработчик мёртвого объекта уже молчит.
    Click(scene);
    CHECK_EQ((int)reports.Got.size(), 1);
    // Кадр — рантайм убирает экземпляр, и подписка уходит из шины.
    sys->Update(0.016f);
    CHECK_EQ(scene.Events.Count(scene.FindByName("Play").Id(), "clicked"), 0);
    CHECK_EQ(log.Containing("Lua"), 0);
    std::remove(path.c_str());
}

// Ошибки — словами, которые говорят, что исправить.
TEST(Signals_lua_errors_explain_themselves) {
    Scene scene("ui");
    MakeButton(scene, "Play");
    GameObject gone = scene.CreateObject("Gone");
    (void)gone;
    Reports reports(scene);
    const std::string path = WriteScript("errors", R"(
local M = {}
local function try(tag, fn)
    local ok, err = pcall(fn)
    Events.emit("report", tag .. "|" .. tostring(err))
end
function M:Start()
    local play = UI.get("Play")
    try("notfn", function() play.clicked:connect(42) end)
    try("dot", function() play.clicked.connect(function() end) end)
    try("empty", function() play:on("", function() end) end)
    try("badname", function() play:on(5, function() end) end)
    try("missing", function() UI.get("Nope") end)
    local gone = Scene.Find("Gone")
    local sig = gone:signal("opened")
    gone:Destroy()
    try("dead", function() sig:connect(function() end) end)
    try("deadobj", function() gone:emit("x") end)
end
return M
)");
    AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    CHECK_TRUE(reports.Has("notfn|") && reports.Has("должен быть функцией, получено number"));
    CHECK_TRUE(reports.Has("вызов через точку"));
    CHECK_TRUE(reports.Has("пустое имя события"));
    CHECK_TRUE(reports.Has("имя события должно быть строкой"));
    CHECK_TRUE(reports.Has("'Nope' не найден"));
    CHECK_TRUE(reports.Has("'Gone' уже уничтожен"));
    CHECK_TRUE(reports.Has("deadobj|") && reports.Has("объект уже уничтожен"));
    std::remove(path.c_str());
}

// Ошибка внутри обработчика — в лог с именем сигнала и объекта; соседний
// обработчик того же сигнала всё равно выполняется.
TEST(Signals_lua_a_failing_handler_is_reported_and_others_still_run) {
    Errors log;
    Scene scene("ui");
    MakeButton(scene, "Play");
    Reports reports(scene);
    const std::string path = WriteScript("fail", R"(
local M = {}
function M:Start()
    local play = UI.get("Play")
    play.clicked:connect(function() error("кнопка сломана") end)
    play.clicked:connect(function() Events.emit("report", "second") end)
end
return M
)");
    AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    Click(scene);
    CHECK_TRUE(reports.Has("second"));
    CHECK_TRUE(log.Containing("кнопка сломана") > 0);
    CHECK_TRUE(log.Containing("'clicked'") > 0 && log.Containing("'Play'") > 0);
    std::remove(path.c_str());
}

// Интерфейс — обычный объект: text, visible, enabled, set_text, show, hide.
TEST(Signals_lua_ui_is_part_of_the_object_model) {
    Scene scene("ui");
    GameObject play = MakeButton(scene, "Play");
    GameObject panel = scene.CreateObject("Settings");
    scene.Registry().emplace<sage::ui::Element>(panel.Entity());
    Reports reports(scene);
    const std::string path = WriteScript("uiobj", R"(
local M = {}
function M:Start()
    local play = UI.get("Play")
    Events.emit("report", "text:" .. play.text)
    play:set_text("Играть")
    play:set_enabled(false)
    UI.get("Settings"):hide()
    Events.emit("report", "visible:" .. tostring(UI.get("Settings").visible) .. ":" .. tostring(play.enabled))
    Events.emit("report", "find:" .. tostring(UI.find("Nope")))
end
return M
)");
    AddScript(scene, "Menu", path);
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    CHECK_TRUE(reports.Has("text:Play"));
    CHECK_EQ(scene.Registry().get<sage::ui::Label>(play.Entity()).Text, std::string("Играть"));
    CHECK_FALSE(scene.Registry().get<sage::ui::Interactable>(play.Entity()).Enabled);
    CHECK_FALSE(scene.Registry().get<sage::ui::Element>(panel.Entity()).Active);
    CHECK_TRUE(reports.Has("visible:false:false"));
    CHECK_TRUE(reports.Has("find:nil"));
    std::remove(path.c_str());
}

// ===========================================================================
//  СВЯЗИ ИНСПЕКТОРА — тот же механизм
// ===========================================================================

// «On Click → Menu.start_game()»: связь из данных сцены зовёт метод скрипта
// ДРУГОГО объекта с тем же событием, что получает подписка кодом. И обе
// работают вместе.
TEST(Signals_inspector_link_calls_the_target_method) {
    Scene scene("ui");
    GameObject play = MakeButton(scene, "Play");
    Reports reports(scene);
    const std::string path = WriteScript("linkmenu", R"(
local Menu = {}
function Menu:Start()
    UI.get("Play").clicked:connect(function() Events.emit("report", "code") end)
end
function Menu:start_game(event, data)
    Events.emit("report", "start:" .. event.name .. ":" .. event.sender.name)
end
return Menu
)");
    GameObject menu = AddScript(scene, "Menu", path);
    sage::signals::Link link;
    link.Signal = "clicked";
    link.Target.Id = menu.Id();
    link.Method = "start_game";
    scene.Registry().emplace<sage::signals::SignalLinksComponent>(play.Entity()).Links.push_back(link);

    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    Click(scene);
    CHECK_TRUE(reports.Has("start:clicked:Play"));
    CHECK_TRUE(reports.Has("code"));

    // Выключенная связь молчит.
    scene.Registry().get<sage::signals::SignalLinksComponent>(play.Entity()).Links[0].Enabled = false;
    sys->InstallLinks(scene, play.Entity());
    reports.Got.clear();
    Click(scene);
    CHECK_FALSE(reports.Has("start:"));
    CHECK_TRUE(reports.Has("code"));

    // После Shutdown связей в шине нет.
    sys->Shutdown();
    CHECK_EQ(scene.Events.Count(play.Id(), "clicked"), 0);
    std::remove(path.c_str());
}

TEST(Signals_inspector_link_errors_name_the_problem) {
    Errors log;
    Scene scene("ui");
    GameObject play = MakeButton(scene, "Play");
    const std::string path = WriteScript("linkerr", R"(
local Menu = {}
function Menu:start_game() end
return Menu
)");
    GameObject menu = AddScript(scene, "Menu", path);
    GameObject plain = scene.CreateObject("Plain");
    auto& links = scene.Registry().emplace<sage::signals::SignalLinksComponent>(play.Entity()).Links;
    links.push_back({"clicked", {menu.Id()}, "nope", true, {}});        // нет метода
    links.push_back({"clicked", {plain.Id()}, "start_game", true, {}});  // нет скрипта
    links.push_back({"clicked", {}, "start_game", true, {}});           // нет цели
    links.push_back({"clicked", {menu.Id()}, "start_game", true, {}});   // цель удалят

    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    CHECK_TRUE(log.Containing("нет метода nope") > 0);
    CHECK_TRUE(log.Containing("у объекта 'Plain' нет скрипта") > 0);
    CHECK_TRUE(log.Containing("не выбран объект-получатель") > 0);

    scene.RemoveObject(menu.Id());
    Click(scene);
    CHECK_TRUE(log.Containing("объект-получатель 'Menu' удалён") > 0);
    std::remove(path.c_str());
}

// Связь старой сцены, славшая событие по имени, работает и после перевода.
TEST(Signals_a_migrated_broadcast_link_sends_the_global_event) {
    Scene scene("ui");
    GameObject play = MakeButton(scene, "Play");
    sage::signals::Link link;
    link.Signal = "clicked";
    link.Broadcast = "game.start";
    scene.Registry().emplace<sage::signals::SignalLinksComponent>(play.Entity()).Links.push_back(link);
    int heard = 0, sender = 0;
    scene.Events.On("game.start", [&](const Event& e) {
        ++heard;
        sender = e.Sender;
    });
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    Click(scene);
    CHECK_EQ(heard, 1);
    CHECK_EQ(sender, play.Id());
}

// Список методов для инспектора — из текста скрипта, без хуков жизненного цикла.
TEST(Signals_script_methods_are_listed_for_the_inspector) {
    const std::vector<std::string> m = sage::scripting::ParseMethods("menu.lua", R"(
local Menu = {}
function Menu:Start() end
function Menu:start_game(event) end
function Menu.open_settings() end
function Menu:_private() end
local function helper() end
function OnMessage() end
-- function commented() end — строка комментария тоже может попасть; безобидно
return Menu
)");
    auto has = [&](const char* n) { return std::find(m.begin(), m.end(), n) != m.end(); };
    CHECK_TRUE(has("start_game"));
    CHECK_TRUE(has("open_settings"));
    CHECK_FALSE(has("helper"));
    CHECK_FALSE(has("Start"));
    CHECK_FALSE(has("_private"));
    CHECK_FALSE(has("OnMessage"));
}

// Физика шлёт сигналы объекта: на вход в зону подписывается ЛЮБОЙ скрипт
// (`zone.trigger_entered:connect`), а не только скрипт самой зоны хуком.
TEST(Signals_physics_zone_emits_trigger_signals_with_the_other_object) {
    Scene scene("test");
    GameObject zone = scene.CreateObject("Zone");
    zone.GetTransform().Position = glm::vec3(5.0f, 1.0f, 0.0f);
    RigidBodyComponent rb{sage::physics::BodyType::Static};
    rb.Sensor = true;
    scene.Registry().emplace<RigidBodyComponent>(zone.Entity(), rb);
    ColliderComponent col;
    col.HalfExtents = glm::vec3(1.0f);
    scene.Registry().emplace<ColliderComponent>(zone.Entity(), col);
    GameObject hero = scene.CreateObject("Hero");
    CharacterControllerComponent cc;
    cc.Gravity = 0.0f;
    scene.Registry().emplace<CharacterControllerComponent>(hero.Entity(), cc);
    CHECK_TRUE(sage::signals::IsDeclared(scene.Registry(), zone.Entity(), "trigger_entered"));

    Reports reports(scene);
    const std::string path = WriteScript("zonewatch", R"(
local W = {}
function W:Start()
    local zone = Scene.Find("Zone")
    zone.trigger_entered:connect(function(event, other) Events.emit("report", "in:" .. other.name) end)
    zone.trigger_exited:connect(function(event, other) Events.emit("report", "out:" .. other.name) end)
end
return W
)");
    AddScript(scene, "Watcher", path);

    PhysicsScene physics(sage::physics::Backend::Builtin, scene);
    if (!physics.Available() || !physics.SupportsCharacters()) return;
    auto sys = MakeSystem(scene);
    sys->AttachScene(scene);
    auto step = [&] {
        physics.Step(scene, 1.0f / 60.0f);
        sys->DispatchPhysicsEvents(physics, scene);
    };
    step();
    auto& live = scene.Registry().get<CharacterControllerComponent>(hero.Entity());
    physics.SetCharacterPosition(live.Runtime, glm::vec3(5.0f, 0.0f, 0.0f));
    step();
    step();
    physics.SetCharacterPosition(live.Runtime, glm::vec3(-5.0f, 0.0f, 0.0f));
    step();
    CHECK_TRUE(reports.Has("in:Hero"));
    CHECK_TRUE(reports.Has("out:Hero"));
    sys->Shutdown();
    std::remove(path.c_str());
}
