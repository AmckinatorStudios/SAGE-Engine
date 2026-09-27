// Шина событий: типизированные события и сроки жизни (sage/events).
// Сигналы объектов и связи — tests/test_signals.cpp.
//
// Проверяется то, ради чего шина существует: подсистемы разговаривают, не зная
// друг о друге; удаление объекта из обработчика не роняет игру; цепочка
// «событие -> условие -> действие -> событие» доигрывается в этом же кадре.
#include "TestFramework.h"

#include <string>
#include <vector>

#include "sage/events/EventTypes.h"
#include "sage/events/Events.h"
#include "sage/events/TypedBus.h"

using sage::events::Bus;
using sage::events::DamageEvent;
using sage::events::DeathEvent;
using sage::events::Event;
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
