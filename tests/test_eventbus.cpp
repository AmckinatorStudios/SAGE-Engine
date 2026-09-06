// Шина событий: типизированные события, сроки жизни и правила «когда — если —
// то» (sage/events).
//
// Проверяется то, ради чего шина существует: подсистемы разговаривают, не зная
// друг о друге; удаление объекта из обработчика не роняет игру; цепочка
// «событие -> условие -> действие -> событие» доигрывается в этом же кадре.
#include "TestFramework.h"

#include <string>
#include <vector>

#include "sage/events/EventTypes.h"
#include "sage/events/Events.h"
#include "sage/events/Rules.h"
#include "sage/events/TypedBus.h"

using sage::events::Binding;
using sage::events::Bus;
using sage::events::Compare;
using sage::events::Condition;
using sage::events::DamageEvent;
using sage::events::DeathEvent;
using sage::events::Event;
using sage::events::Rule;
using sage::events::RuleSet;
using sage::events::TypedBus;
using sage::vars::Value;

// ===========================================================================
//  ТИПИЗИРОВАННАЯ ШИНА
// ===========================================================================

TEST(Events_typed_bus_delivers_only_the_requested_type) {
    TypedBus bus;
    int damage = 0, death = 0;
    bus.Subscribe<DamageEvent>([&](const DamageEvent& e) { damage += (int)e.Amount; });
    bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++death; });

    bus.Publish(DamageEvent{1, 2, 25.0f, {}, "fire"});
    CHECK_EQ(damage, 25);
    CHECK_EQ(death, 0);

    bus.Publish(DeathEvent{2, 1});
    CHECK_EQ(death, 1);
}

TEST(Events_typed_unsubscribe_actually_stops_delivery) {
    TypedBus bus;
    int seen = 0;
    const int id = bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++seen; });
    bus.Publish(DeathEvent{1, 0});
    bus.Unsubscribe(id);
    bus.Publish(DeathEvent{1, 0});
    CHECK_EQ(seen, 1);
    CHECK_EQ(bus.Count<DeathEvent>(), 0);
}

TEST(Events_typed_once_fires_exactly_one_time) {
    TypedBus bus;
    int seen = 0;
    bus.SubscribeOnce<DeathEvent>([&](const DeathEvent&) { ++seen; });
    bus.Publish(DeathEvent{1, 0});
    bus.Publish(DeathEvent{1, 0});
    CHECK_EQ(seen, 1);
}

// Обработчик волен отписаться и подписаться прямо во время рассылки — вектор
// подписок при этом меняется под ногами у цикла.
TEST(Events_typed_bus_survives_subscribing_from_inside_a_handler) {
    TypedBus bus;
    int seen = 0;
    bus.Subscribe<DeathEvent>([&](const DeathEvent&) {
        ++seen;
        bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++seen; });
    });
    bus.Publish(DeathEvent{1, 0});
    CHECK_EQ(seen, 1);   // новая подписка застаёт только СЛЕДУЮЩЕЕ событие
    bus.Publish(DeathEvent{1, 0});
    CHECK_EQ(seen, 3);
}

// ===========================================================================
//  СРОКИ ЖИЗНИ: сразу / в очередь / после фазы
// ===========================================================================

TEST(Events_queued_event_waits_for_its_dispatch) {
    TypedBus bus;
    int seen = 0;
    bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++seen; });

    bus.Queue(DeathEvent{1, 0});
    CHECK_EQ(seen, 0);            // «в очередь» значит НЕ сейчас
    CHECK_EQ((int)bus.QueuedCount(), 1);
    bus.DispatchQueued();
    CHECK_EQ(seen, 1);
    CHECK_EQ((int)bus.QueuedCount(), 0);
}

// Отложенное — единственный безопасный способ удалить объект из обработчика.
TEST(Events_deferred_event_waits_for_the_end_of_the_phase) {
    TypedBus bus;
    int seen = 0;
    bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++seen; });

    bus.Defer(DeathEvent{1, 0});
    bus.DispatchQueued();
    CHECK_EQ(seen, 0);   // очередь кадра отложенное не трогает
    bus.DispatchDeferred();
    CHECK_EQ(seen, 1);
}

// Цепочка обязана доиграться в ЭТОМ кадре: разложенная по кадрам, она
// превращает открытие двери в лестницу задержек.
TEST(Events_a_chain_queued_from_a_handler_finishes_in_the_same_dispatch) {
    TypedBus bus;
    int deaths = 0;
    bus.Subscribe<DamageEvent>([&](const DamageEvent& e) {
        if (e.Amount >= 100.0f) bus.Queue(DeathEvent{e.Target, e.Source});
    });
    bus.Subscribe<DeathEvent>([&](const DeathEvent&) { ++deaths; });

    bus.Queue(DamageEvent{1, 2, 150.0f, {}, "fall"});
    bus.DispatchQueued();
    CHECK_EQ(deaths, 1);
}

// Два события, кладущих друг друга в очередь, обязаны остановиться, а не
// зациклить кадр насмерть.
TEST(Events_a_queue_loop_stops_instead_of_hanging_the_frame) {
    TypedBus bus;
    int seen = 0;
    bus.Subscribe<DeathEvent>([&](const DeathEvent& e) {
        ++seen;
        bus.Queue(e);
    });
    bus.Queue(DeathEvent{1, 0});
    bus.DispatchQueued();
    CHECK_TRUE(seen > 0);
    CHECK_TRUE(seen < 100);
    CHECK_EQ((int)bus.QueuedCount(), 0);
}

// То же самое для именной шины — ею говорят Lua и связи в инспекторе.
TEST(Events_named_bus_supports_queued_and_deferred_too) {
    Bus bus;
    std::vector<std::string> heard;
    bus.On("hit", [&](const Event& e) { heard.push_back(e.Name); });
    bus.On("die", [&](const Event& e) { heard.push_back(e.Name); });

    bus.Emit("hit");
    bus.Queue("hit");
    bus.Defer("die");
    CHECK_EQ((int)heard.size(), 1);

    bus.DispatchQueued();
    CHECK_EQ((int)heard.size(), 2);
    bus.DispatchDeferred();
    CHECK_EQ((int)heard.size(), 3);
    CHECK_EQ(heard[2], std::string("die"));
}

// Неразобранные события выгруженного уровня не должны догнать следующий.
TEST(Events_clearing_the_bus_drops_pending_events) {
    Bus bus;
    int seen = 0;
    bus.On("hit", [&](const Event&) { ++seen; });
    bus.Queue("hit");
    bus.Clear();
    bus.DispatchQueued();
    CHECK_EQ(seen, 0);
}

// ===========================================================================
//  ПРАВИЛА: КОГДА — ЕСЛИ — ТО
// ===========================================================================

namespace {

// Мир, из которого правила берут значения. В игре это сцена, в тесте —
// таблица: правило не знает разницы, и в этом весь смысл резолвера.
struct FakeWorld {
    bool DoorLocked = true;
    bool HasKey = false;

    sage::events::Resolver Lookup() {
        return [this](const std::string& var) -> Value {
            if (var == "Door.IsLocked") return Value(DoorLocked);
            if (var == "Player.HasKey") return Value(HasKey);
            return Value();
        };
    }
};

Rule MakeOpenDoorRule() {
    Rule rule;
    rule.Name = "Открыть дверь";
    rule.When = "Interact";
    rule.If.push_back(Condition{"Door.IsLocked", Compare::IsFalse, Value()});
    Binding open;
    open.Event = "DoorOpened";
    rule.Then.push_back(open);
    return rule;
}

} // namespace

// «КОГДА взаимодействие, ЕСЛИ дверь не заперта, ТО открыть» — та самая
// цепочка из ТЗ, собранная данными, а не кодом.
TEST(Events_a_rule_fires_only_when_its_condition_holds) {
    Bus bus;
    FakeWorld world;
    RuleSet rules;
    rules.SetResolver(world.Lookup());
    rules.Add(MakeOpenDoorRule());
    rules.Install(bus);

    int opened = 0;
    bus.On("DoorOpened", [&](const Event&) { ++opened; });

    bus.Emit("Interact");
    CHECK_EQ(opened, 0);   // заперта — правило молчит

    world.DoorLocked = false;
    bus.Emit("Interact");
    CHECK_EQ(opened, 1);
    CHECK_EQ(rules.FiredCount(), 1);
}

// Правило, которому нечем проверить условие, обязано молчать, а не открывать
// дверь наугад.
TEST(Events_a_rule_without_a_resolver_stays_silent) {
    Bus bus;
    RuleSet rules;
    rules.Add(MakeOpenDoorRule());
    rules.Install(bus);

    int opened = 0;
    bus.On("DoorOpened", [&](const Event&) { ++opened; });
    bus.Emit("Interact");
    CHECK_EQ(opened, 0);
}

// «ТО» одного правила — «КОГДА» другого: цепочка получается сама собой, и ни
// одно правило не знает о существовании соседа (§17 ТЗ).
TEST(Events_rules_chain_through_the_bus) {
    Bus bus;
    FakeWorld world;
    world.DoorLocked = false;
    RuleSet rules;
    rules.SetResolver(world.Lookup());
    rules.Add(MakeOpenDoorRule());

    Rule sound;
    sound.Name = "Звук двери";
    sound.When = "DoorOpened";
    Binding play;
    play.Event = "PlaySound";
    play.Arg = Value(std::string("door_open"));
    sound.Then.push_back(play);
    rules.Add(sound);
    rules.Install(bus);

    std::string played;
    bus.On("PlaySound", [&](const Event& e) { played = e.Arg.AsString(); });

    bus.Emit("Interact");
    CHECK_EQ(played, std::string("door_open"));
}

TEST(Events_rule_conditions_compare_numbers_and_strings) {
    FakeWorld world;
    auto resolver = [](const std::string& var) -> Value {
        if (var == "Player.Health") return Value(30);
        if (var == "Door.State") return Value(std::string("closed"));
        return Value();
    };
    CHECK_TRUE(sage::events::TestCondition({"Player.Health", Compare::Less, Value(50)}, resolver));
    CHECK_FALSE(sage::events::TestCondition({"Player.Health", Compare::Greater, Value(50)}, resolver));
    CHECK_TRUE(sage::events::TestCondition(
        {"Door.State", Compare::Equal, Value(std::string("closed"))}, resolver));
    CHECK_TRUE(sage::events::TestCondition(
        {"Door.State", Compare::NotEqual, Value(std::string("open"))}, resolver));
    (void)world;
}

// Правила принадлежат сцене и обязаны исчезнуть вместе с ней — иначе
// выгруженный уровень продолжает открывать свои двери.
TEST(Events_uninstalling_a_ruleset_stops_it) {
    Bus bus;
    FakeWorld world;
    world.DoorLocked = false;
    RuleSet rules;
    rules.SetResolver(world.Lookup());
    rules.Add(MakeOpenDoorRule());
    rules.Install(bus);

    int opened = 0;
    bus.On("DoorOpened", [&](const Event&) { ++opened; });
    bus.Emit("Interact");
    CHECK_EQ(opened, 1);

    rules.Uninstall();
    bus.Emit("Interact");
    CHECK_EQ(opened, 1);
}

// Правило, добавленное ПОСЛЕ подписки набора на шину, обязано работать: иначе
// разбираться с его молчанием будут долго.
TEST(Events_a_rule_added_after_install_still_works) {
    Bus bus;
    FakeWorld world;
    world.DoorLocked = false;
    RuleSet rules;
    rules.SetResolver(world.Lookup());
    rules.Install(bus);
    rules.Add(MakeOpenDoorRule());

    int opened = 0;
    bus.On("DoorOpened", [&](const Event&) { ++opened; });
    bus.Emit("Interact");
    CHECK_EQ(opened, 1);
}
