#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/ui/Element.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Visual.h"

// ---------------------------------------------------------------------------
// СИГНАЛЫ И СВЯЗИ ГЛАЗАМИ СКРИПТА.
//
//     ui.play.clicked:connect(function(event) Menu.start_game() end)
//     local c = enemy.collision:connect(on_hit)   ...   c:disconnect()
//     door.clicked:once(open)
//     player:emit("player_died", {score = 10})
//     player:on("player_died", function(event, data) ... end)
//     Events.on("level_loaded", fn)     Events.emit("level_loaded", "forest")
//     UI.get("PlayButton"):set_text("Играть")
//
// Всё это — ОДИН механизм шины сцены (sage/events/Events.h): тот же, которым
// пользуются связи инспектора и код на C++. У API нет ни одного имени,
// привязанного к интерфейсу: `clicked` — это сигнал, объявленный кнопкой
// (sage/scene/Signals.h), и ровно так же читается `collision` у тела физики и
// `player_died`, придуманный игрой.
//
// Обработчик получает (event, data): event = {name, sender, data}, data — то,
// что послали. Два аргумента, потому что половине обработчиков нужны только
// данные, а другой половине — ещё и кто прислал.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

std::string TypeName(const sol::object& o) {
    switch (o.get_type()) {
        case sol::type::lua_nil: return "nil";
        case sol::type::boolean: return "boolean";
        case sol::type::number: return "number";
        case sol::type::string: return "string";
        case sol::type::table: return "table";
        case sol::type::function: return "function";
        case sol::type::userdata: return "userdata";
        default: return "value";
    }
}

std::runtime_error Fail(const std::string& text) { return std::runtime_error(text); }

// Имя события из аргумента: строка, непустая, без пробелов. Опечатка в имени
// («clicked » с пробелом) — подписка, которая молча никогда не сработает;
// лучше громко отказать сразу.
std::string NameArg(const sol::object& o, const std::string& who) {
    if (!o.is<std::string>())
        throw Fail(who + ": имя события должно быть строкой, получено " + TypeName(o));
    std::string name = o.as<std::string>();
    if (name.empty()) throw Fail(who + ": пустое имя события");
    if (!sage::signals::IsValidName(name))
        throw Fail(who + ": в имени события '" + name + "' пробелы или служебные символы");
    return name;
}

// Живой объект по прокси или понятная ошибка. Мёртвый объект — самая частая
// ошибка живого кода: скрипт запомнил кнопку, панель пересобрали.
GameObject AliveObject(const ObjectRef& r, const std::string& who) {
    if (!r.Obj.Valid()) throw Fail(who + ": объект уже уничтожен");
    return r.Obj;
}

// Первый аргумент метода сигнала. Через точку (`button.clicked.connect(fn)`)
// сам сигнал не приходит — и вместо невнятного «bad argument #1» человек
// получает то, что надо исправить.
const SignalRef& SelfSignal(sol::variadic_args& va, const char* method) {
    if (va.size() == 0 || !va[0].is<SignalRef>())
        throw Fail(std::string("signal.") + method + ": вызов через точку — пишите signal:" + method +
                   "(...) (например, button.clicked:" + method + "(fn))");
    return va[0].as<const SignalRef&>();
}

sol::object Arg(sol::variadic_args& va, size_t i) {
    return i < va.size() ? sol::object(va[i]) : sol::object(sol::lua_nil);
}

// Сдвиг для разделов (Events:on и Events.on — одно и то же, см. LuaApi_Globals).
size_t Skip(const sol::variadic_args& va) {
    return va.size() > 0 && va[0].get_type() == sol::type::table ? 1u : 0u;
}

// --- Интерфейс как обычный объект -------------------------------------------

// Надпись объекта: своя или первая у потомков. Кнопка обычно собрана из
// подложки и дочерней надписи, и `button.text` обязан менять ту, что видна, а
// не требовать от скрипта знать, из каких частей кнопка собрана.
sage::ui::Label* LabelOf(entt::registry& reg, entt::entity e, int depth = 0) {
    if (sage::ui::Label* l = reg.try_get<sage::ui::Label>(e)) return l;
    if (depth > 8) return nullptr;
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e))
        for (entt::entity c : h->Children)
            if (reg.valid(c))
                if (sage::ui::Label* l = LabelOf(reg, c, depth + 1)) return l;
    return nullptr;
}

// «Виден» — одно понятие на всё: у элемента интерфейса это Active (панель
// выпадает из отрисовки И ввода — спрятанная кнопка не должна нажиматься), у
// объекта сцены — то же, что галочка в редакторе (HiddenComponent).
bool IsVisible(Backend& b, const GameObject& o) {
    entt::registry& reg = *o.Registry();
    if (const sage::ui::Element* el = reg.try_get<sage::ui::Element>(o.Entity())) return el->Active;
    Scene* scene = b.ScenePtr();
    return scene ? !scene->IsHidden(o.Entity()) : !reg.all_of<HiddenComponent>(o.Entity());
}

void SetVisible(const GameObject& o, bool on) {
    entt::registry& reg = *o.Registry();
    if (sage::ui::Element* el = reg.try_get<sage::ui::Element>(o.Entity())) {
        el->Active = on;
        return;
    }
    if (on) reg.remove<HiddenComponent>(o.Entity());
    else reg.emplace_or_replace<HiddenComponent>(o.Entity());
}

} // namespace

// =============================================================================
//  Бэкенд: подписка, рассылка, вид события
// =============================================================================

sage::events::Bus& Backend::BusOrThrow(const std::string& who) {
    Scene* scene = ScenePtr();
    if (!scene) throw Fail(who + ": сцена не привязана — сигналам негде жить");
    return scene->Events;
}

ConnectionRef Backend::ConnectLua(const SignalRef& signal, const sol::object& fn, bool once,
                                  const std::string& who) {
    if (fn.get_type() != sol::type::function)
        throw Fail(who + ": обработчик должен быть функцией, получено " + TypeName(fn));
    sage::events::Bus& bus = BusOrThrow(who);
    if (signal.Object != sage::events::Bus::kGlobal && !ScenePtr()->Get(signal.Object).Valid())
        throw Fail(who + ": объект '" + signal.ObjectName + "' уже уничтожен — его '" + signal.Name +
                   "' больше некому послать");
    const int id = m_signals->Connect(bus, signal.Object, signal.Name,
                                      fn.as<sol::protected_function>(), once, (int)m_current);
    if (!id) throw Fail(who + ": подписка на '" + signal.Name + "' не заведена");
    return ConnectionRef{id, signal.Name};
}

void Backend::EmitLua(int object, const std::string& name, const sol::object& data,
                      const std::string& who) {
    sage::events::Bus& bus = BusOrThrow(who);
    sage::events::Event e;
    e.Name = name;
    e.Object = object;
    e.Sender = object;
    // Глобальное событие из скрипта объекта — отправитель тот, чей код шлёт:
    // обработчику «player_died» полезно знать, кто умер.
    if (object == sage::events::Bus::kGlobal && m_current != kInvalidInstance)
        if (const Instance* inst = Get(m_current); inst && inst->Owner.Valid())
            e.Sender = inst->Owner.Id();
    e.Arg = FromLua(data);
    if (data.valid() && data.get_type() != sol::type::lua_nil) e.Payload = data;
    bus.Emit(e);
}

sol::object Backend::EventData(const sage::events::Event& event) {
    // Данные в языке отправителя едут как есть (таблица остаётся таблицей);
    // посланное из C++ — переводится из значения движка.
    if (const sol::object* raw = std::any_cast<sol::object>(&event.Payload)) return *raw;
    return ToLua(event.Arg);
}

sol::table Backend::EventTable(const sage::events::Event& event) {
    sol::table t = m_lua->create_table();
    t["name"] = event.Name;
    t["data"] = EventData(event);
    Scene* scene = ScenePtr();
    if (scene && event.Sender > 0) {
        GameObject from = scene->Get(event.Sender);
        if (from.Valid()) t["sender"] = Wrap(from);
    }
    return t;
}

void Backend::CallHandler(const sol::protected_function& fn, const sage::events::Event& event,
                          int owner) {
    // Подписка экземпляра, чей объект уже уничтожен (в этом же кадре, до
    // уборки): звать его код с мёртвым self — значит получить ошибку там, где
    // её никто не совершал. Молчим; подписка снимется вместе с экземпляром.
    if (owner != (int)kInvalidInstance) {
        const Instance* inst = Get((InstanceId)owner);
        if (!inst || !inst->Owner.Valid()) return;
    }
    CurrentScope scope(*this, (InstanceId)owner);
    sol::protected_function_result r = fn(EventTable(event), EventData(event));
    if (r.valid()) return;
    sol::error err = r;
    std::string who;
    if (event.Object != sage::events::Bus::kGlobal) {
        Scene* scene = ScenePtr();
        GameObject o = scene ? scene->Get(event.Object) : GameObject();
        who = " объекта '" + (o.Valid() ? o.Name() : std::to_string(event.Object)) + "'";
    }
    // Ошибка одного обработчика не отменяет соседей: кнопка с тремя
    // слушателями, у которой сломан один, обязана выполнить два других.
    LOG_ERROR("Lua") << "Обработчик сигнала '" << event.Name << "'" << who << ": " << err.what();
}

// =============================================================================
//  Регистрация
// =============================================================================

void RegisterObjectSignals(Backend& backend, sol::usertype<ObjectRef>& ot) {
    Backend* self = &backend;

    auto signalOf = [](const GameObject& o, const std::string& name) {
        return SignalRef{o.Id(), name, o.Name()};
    };

    // obj:signal("player_died") — сигнал по имени, любой, в том числе свой.
    ot["signal"] = [signalOf](ObjectRef& r, sol::object name) {
        const GameObject o = AliveObject(r, "signal");
        return signalOf(o, NameArg(name, "signal"));
    };
    ot["on"] = [self, signalOf](ObjectRef& r, sol::object name, sol::object fn) {
        const GameObject o = AliveObject(r, "on");
        const SignalRef s = signalOf(o, NameArg(name, "on"));
        return self->ConnectLua(s, fn, false, o.Name() + ":on('" + s.Name + "')");
    };
    ot["once"] = [self, signalOf](ObjectRef& r, sol::object name, sol::object fn) {
        const GameObject o = AliveObject(r, "once");
        const SignalRef s = signalOf(o, NameArg(name, "once"));
        return self->ConnectLua(s, fn, true, o.Name() + ":once('" + s.Name + "')");
    };
    ot["emit"] = [self](ObjectRef& r, sol::object name, sol::variadic_args rest) {
        const GameObject o = AliveObject(r, "emit");
        const std::string n = NameArg(name, "emit");
        self->EmitLua(o.Id(), n, Arg(rest, 0), o.Name() + ":emit('" + n + "')");
    };
    // Снять СВОИ (скриптовые) подписки на сигнал. Связи инспектора и
    // обработчики C++ не трогаются: скрипт не должен молча ломать то, что
    // настроено в сцене.
    ot["off"] = [self](ObjectRef& r, sol::object name) {
        const GameObject o = AliveObject(r, "off");
        return self->BusOrThrow("off").DisconnectSignal(o.Id(), NameArg(name, "off"),
                                                        self->Signals().Group());
    };
    // Какие сигналы объявили компоненты объекта — для отладки и инструментов.
    ot["signals"] = sol::readonly_property([self](ObjectRef& r) {
        const GameObject o = AliveObject(r, "signals");
        sol::table list = self->Lua().create_table();
        int i = 1;
        for (const sage::signals::SignalInfo& s : sage::signals::Of(*o.Registry(), o.Entity()))
            list[i++] = s.Name;
        return list;
    });

    // СИГНАЛ КАК СВОЙСТВО: `button.clicked`. Только объявленные компонентами
    // объекта (sage/scene/Signals.h): иначе любая опечатка в любом поле
    // (`obj.helth`) превращалась бы в «сигнал helth», и `if obj.x then`
    // перестал бы работать. Своё событие — через obj:on/obj:emit/obj:signal.
    //
    // Затем — нынешние имена методов (`obj:get_component`, `obj:call`) и
    // компонент по имени типа (`obj.Audio`): одно правило для self и для
    // любого найденного объекта.
    ot[sol::meta_function::index] = [signalOf, self](ObjectRef& r, sol::stack_object key) -> sol::object {
        if (!key.is<std::string>()) return sol::lua_nil;
        sol::object method = SnakeMethod(key.lua_state(), "SageObject", key);
        if (method.valid() && method.get_type() != sol::type::lua_nil) return method;
        if (!r.Obj.Valid()) return sol::lua_nil;
        const std::string name = key.as<std::string>();
        if (sage::signals::IsDeclared(*r.Obj.Registry(), r.Obj.Entity(), name))
            return sol::make_object(key.lua_state(), signalOf(r.Obj, name));
        if (std::isupper((unsigned char)name[0])) return self->ComponentOf(r.Obj, name);
        return sol::lua_nil;
    };

    // --- Интерфейс как часть объекта ----------------------------------------
    //
    // Кнопка — такой же объект, как дверь: у неё есть свойства (text, visible,
    // enabled), методы (set_text, show, hide) и сигналы (clicked). Отдельного
    // «UIElement» с другими правилами скрипту знать не нужно.
    ot["text"] = sol::property(
        [](ObjectRef& r, sol::this_state ts) -> sol::object {
            const GameObject o = AliveObject(r, "text");
            const sage::ui::Label* l = LabelOf(*o.Registry(), o.Entity());
            if (!l) return sol::lua_nil;   // у объекта без надписи текста нет — это не ошибка чтения
            return sol::make_object(ts, l->Text);
        },
        [](ObjectRef& r, const std::string& text) {
            const GameObject o = AliveObject(r, "text");
            sage::ui::Label* l = LabelOf(*o.Registry(), o.Entity());
            if (!l) throw Fail("text: у объекта '" + o.Name() + "' нет надписи (компонента Label)");
            l->Text = text;
        });
    ot["set_text"] = [](ObjectRef& r, sol::object text) {
        const GameObject o = AliveObject(r, "set_text");
        if (!text.is<std::string>() && text.get_type() != sol::type::number)
            throw Fail("set_text: ожидается строка, получено " + TypeName(text));
        sage::ui::Label* l = LabelOf(*o.Registry(), o.Entity());
        if (!l) throw Fail("set_text: у объекта '" + o.Name() + "' нет надписи (компонента Label)");
        l->Text = text.as<std::string>();
    };
    ot["visible"] = sol::property(
        [self](ObjectRef& r) { return IsVisible(*self, AliveObject(r, "visible")); },
        [](ObjectRef& r, bool on) { SetVisible(AliveObject(r, "visible"), on); });
    ot["show"] = [](ObjectRef& r) { SetVisible(AliveObject(r, "show"), true); };
    ot["hide"] = [](ObjectRef& r) { SetVisible(AliveObject(r, "hide"), false); };
    ot["set_visible"] = [](ObjectRef& r, bool on) { SetVisible(AliveObject(r, "set_visible"), on); };
    // «Доступен» — ловит ли мышь (Interactable). У объекта без него — всегда
    // true: недоступным может быть только то, что вообще можно нажать.
    ot["enabled"] = sol::property(
        [](ObjectRef& r) {
            const GameObject o = AliveObject(r, "enabled");
            const sage::ui::Interactable* act = o.Registry()->try_get<sage::ui::Interactable>(o.Entity());
            return !act || act->Enabled;
        },
        [](ObjectRef& r, bool on) {
            const GameObject o = AliveObject(r, "enabled");
            sage::ui::Interactable* act = o.Registry()->try_get<sage::ui::Interactable>(o.Entity());
            if (!act) throw Fail("enabled: объект '" + o.Name() + "' не нажимается (нет Interactable)");
            act->Enabled = on;
        });
    ot["set_enabled"] = [](ObjectRef& r, bool on) {
        const GameObject o = AliveObject(r, "set_enabled");
        sage::ui::Interactable* act = o.Registry()->try_get<sage::ui::Interactable>(o.Entity());
        if (!act) throw Fail("set_enabled: объект '" + o.Name() + "' не нажимается (нет Interactable)");
        act->Enabled = on;
    };
    // Значение ползунка/галки. Запись шлёт value_changed — как и движение
    // мышью: слушателю всё равно, кто подвинул.
    ot["value"] = sol::property(
        [](ObjectRef& r, sol::this_state ts) -> sol::object {
            const GameObject o = AliveObject(r, "value");
            const sage::ui::Range* range = o.Registry()->try_get<sage::ui::Range>(o.Entity());
            if (!range) return sol::lua_nil;
            if (range->Toggle) return sol::make_object(ts, range->Value >= (range->Min + range->Max) * 0.5f);
            return sol::make_object(ts, range->Value);
        },
        [self](ObjectRef& r, float v) {
            const GameObject o = AliveObject(r, "value");
            sage::ui::Range* range = o.Registry()->try_get<sage::ui::Range>(o.Entity());
            if (!range) throw Fail("value: у объекта '" + o.Name() + "' нет значения (компонента Range)");
            if (range->Value == v) return;
            range->Value = v;
            self->BusOrThrow("value").EmitSignal(o.Id(), sage::signals::kValueChanged, sage::vars::Value(v));
        });
}

void RegisterSignals(Backend& backend) {
    sol::state& lua = backend.Lua();
    Backend* self = &backend;

    // --- Сигнал --------------------------------------------------------------
    sol::usertype<SignalRef> st = lua.new_usertype<SignalRef>("SageSignal", sol::no_constructor);
    st["name"] = sol::readonly_property([](const SignalRef& s) { return s.Name; });
    st["object"] = sol::readonly_property([self](const SignalRef& s) -> sol::object {
        Scene* scene = self->ScenePtr();
        if (!scene || s.Object == sage::events::Bus::kGlobal) return sol::lua_nil;
        return self->Wrap(scene->Get(s.Object));
    });
    st["connect"] = [self](sol::variadic_args va) {
        const SignalRef& s = SelfSignal(va, "connect");
        return self->ConnectLua(s, Arg(va, 1), false, s.Name + ":connect");
    };
    st["once"] = [self](sol::variadic_args va) {
        const SignalRef& s = SelfSignal(va, "once");
        return self->ConnectLua(s, Arg(va, 1), true, s.Name + ":once");
    };
    st["emit"] = [self](sol::variadic_args va) {
        const SignalRef& s = SelfSignal(va, "emit");
        if (s.Object != sage::events::Bus::kGlobal && self->ScenePtr() &&
            !self->ScenePtr()->Get(s.Object).Valid())
            throw Fail(s.Name + ":emit: объект '" + s.ObjectName + "' уже уничтожен");
        self->EmitLua(s.Object, s.Name, Arg(va, 1), s.Name + ":emit");
    };
    st["disconnect_all"] = [self](sol::variadic_args va) {
        const SignalRef& s = SelfSignal(va, "disconnect_all");
        return self->BusOrThrow(s.Name + ":disconnect_all")
            .DisconnectSignal(s.Object, s.Name, self->Signals().Group());
    };
    st["count"] = [self](sol::variadic_args va) {
        const SignalRef& s = SelfSignal(va, "count");
        Scene* scene = self->ScenePtr();
        return scene ? scene->Events.Count(s.Object, s.Name) : 0;
    };
    st[sol::meta_function::to_string] = [](const SignalRef& s) {
        return "Signal(" + s.Name + (s.Object ? " of " + s.ObjectName : std::string()) + ")";
    };

    // --- Соединение ----------------------------------------------------------
    sol::usertype<ConnectionRef> ct = lua.new_usertype<ConnectionRef>("SageConnection", sol::no_constructor);
    // Повторный disconnect — не ошибка: он возвращает false. Два пути к одному
    // «отпишись» (по событию и в OnDestroy) — обычное дело.
    ct["disconnect"] = [self](ConnectionRef& c) {
        Scene* scene = self->ScenePtr();
        return scene && scene->Events.Disconnect(c.Id);
    };
    ct["connected"] = sol::readonly_property([self](const ConnectionRef& c) {
        Scene* scene = self->ScenePtr();
        return scene && scene->Events.IsConnected(c.Id);
    });
    ct["id"] = sol::readonly_property([](const ConnectionRef& c) { return c.Id; });
    ct[sol::meta_function::to_string] = [](const ConnectionRef& c) {
        return "Connection(" + c.Signal + " #" + std::to_string(c.Id) + ")";
    };

    // --- Events: глобальная шина (сигналы «ничьего» объекта 0) ---------------
    sol::table events = lua.create_table();
    auto global = [](const std::string& name) { return SignalRef{sage::events::Bus::kGlobal, name, {}}; };
    auto on = [self, global](sol::variadic_args va) {
        const size_t b = Skip(va);
        const std::string name = NameArg(Arg(va, b), "Events.on");
        return self->ConnectLua(global(name), Arg(va, b + 1), false, "Events.on('" + name + "')");
    };
    auto once = [self, global](sol::variadic_args va) {
        const size_t b = Skip(va);
        const std::string name = NameArg(Arg(va, b), "Events.once");
        return self->ConnectLua(global(name), Arg(va, b + 1), true, "Events.once('" + name + "')");
    };
    auto emit = [self](sol::variadic_args va) {
        const size_t b = Skip(va);
        const std::string name = NameArg(Arg(va, b), "Events.emit");
        self->EmitLua(sage::events::Bus::kGlobal, name, Arg(va, b + 1), "Events.emit('" + name + "')");
    };
    // Снять: соединение, его номер или все СВОИ подписки на имя.
    auto off = [self](sol::variadic_args va) -> int {
        const size_t b = Skip(va);
        sol::object what = Arg(va, b);
        sage::events::Bus& bus = self->BusOrThrow("Events.off");
        if (what.is<ConnectionRef>()) return bus.Disconnect(what.as<ConnectionRef>().Id) ? 1 : 0;
        if (what.get_type() == sol::type::number) return bus.Disconnect(what.as<int>()) ? 1 : 0;
        const std::string name = NameArg(what, "Events.off");
        return bus.DisconnectSignal(sage::events::Bus::kGlobal, name, self->Signals().Group());
    };
    auto count = [self](sol::variadic_args va) {
        const size_t b = Skip(va);
        const std::string name = NameArg(Arg(va, b), "Events.count");
        Scene* scene = self->ScenePtr();
        return scene ? scene->Events.Count(name) : 0;
    };
    events.set_function("on", on);
    events.set_function("once", once);
    events.set_function("emit", emit);
    events.set_function("off", off);
    events.set_function("count", count);
    events.set_function("signal", [global](sol::variadic_args va) {
        const size_t b = Skip(va);
        return global(NameArg(Arg(va, b), "Events.signal"));
    });
    // Прежние имена с большой буквы — те же функции: игры, написанные до
    // сигналов (Events.On(...)), продолжают работать без правки.
    events.set_function("On", on);
    events.set_function("Once", once);
    events.set_function("Emit", emit);
    events.set_function("Off", off);
    events.set_function("Count", count);
    lua["Events"] = events;

    // --- UI: найти элемент по имени ------------------------------------------
    //
    // UI.get бросает понятную ошибку, если элемента нет: nil здесь превратился
    // бы в «attempt to index a nil value» строкой ниже, где уже не видно, КАКОЕ
    // имя не нашлось. UI.find — для тех, кто проверяет сам.
    sol::table ui = lua.create_table();
    auto find = [self](const std::string& name) -> GameObject {
        Scene* scene = self->ScenePtr();
        if (!scene) return GameObject();
        auto view = scene->Registry().view<sage::ui::Element, NameComponent>();
        for (auto e : view)
            if (view.get<NameComponent>(e).Name == name) return GameObject(&scene->Registry(), e);
        return GameObject();
    };
    ui.set_function("find", [self, find](sol::variadic_args va) -> sol::object {
        const size_t b = Skip(va);
        sol::object n = Arg(va, b);
        if (!n.is<std::string>()) throw Fail("UI.find: имя должно быть строкой, получено " + TypeName(n));
        const GameObject o = find(n.as<std::string>());
        return o.Valid() ? self->Wrap(o) : sol::lua_nil;
    });
    ui.set_function("get", [self, find](sol::variadic_args va) -> sol::object {
        const size_t b = Skip(va);
        sol::object n = Arg(va, b);
        if (!n.is<std::string>()) throw Fail("UI.get: имя должно быть строкой, получено " + TypeName(n));
        SceneOrThrow(*self, "UI.get");
        const GameObject o = find(n.as<std::string>());
        if (!o.Valid()) throw Fail("UI.get: элемент интерфейса '" + n.as<std::string>() + "' не найден");
        return self->Wrap(o);
    });
    lua["UI"] = ui;
}

} // namespace sage::scripting::lua
