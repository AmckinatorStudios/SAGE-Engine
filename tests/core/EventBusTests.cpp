#include <doctest/doctest.h>
#include "core/EventBus.h"

TEST_CASE("EventBus: Subscribe/Emit доставляет нагрузку подписчику") {
    EventBus bus;
    int received = -1;
    bus.Subscribe("test.event", [&](const std::any& data) {
        received = std::any_cast<int>(data);
    });
    bus.Emit("test.event", std::make_any<int>(42));
    CHECK(received == 42);
}

TEST_CASE("EventBus: Emit без данных передаёт пустой std::any") {
    EventBus bus;
    bool called = false;
    bool hadValue = true;
    bus.Subscribe("test.tick", [&](const std::any& data) {
        called = true;
        hadValue = data.has_value();
    });
    bus.Emit("test.tick");
    CHECK(called);
    CHECK_FALSE(hadValue);
}

TEST_CASE("EventBus: несколько подписчиков вызываются в порядке подписки") {
    EventBus bus;
    std::vector<int> order;
    bus.Subscribe("multi", [&](const std::any&) { order.push_back(1); });
    bus.Subscribe("multi", [&](const std::any&) { order.push_back(2); });
    bus.Subscribe("multi", [&](const std::any&) { order.push_back(3); });
    bus.Emit("multi");
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
}

TEST_CASE("EventBus: Unsubscribe останавливает доставку") {
    EventBus bus;
    int hits = 0;
    int id = bus.Subscribe("evt", [&](const std::any&) { ++hits; });
    bus.Emit("evt");
    bus.Unsubscribe(id);
    bus.Emit("evt");
    CHECK(hits == 1);
}

TEST_CASE("EventBus: Unsubscribe с неизвестным id ничего не ломает") {
    EventBus bus;
    bus.Unsubscribe(9999);
    CHECK(bus.HandlerCount("nothing") == 0);
}

TEST_CASE("EventBus: HandlerCount отражает число активных подписок") {
    EventBus bus;
    CHECK(bus.HandlerCount("x") == 0);
    int a = bus.Subscribe("x", [](const std::any&) {});
    int b = bus.Subscribe("x", [](const std::any&) {});
    CHECK(bus.HandlerCount("x") == 2);
    bus.Unsubscribe(a);
    CHECK(bus.HandlerCount("x") == 1);
    bus.Unsubscribe(b);
    CHECK(bus.HandlerCount("x") == 0);
}

TEST_CASE("EventBus: Clear снимает все подписки на все события") {
    EventBus bus;
    bus.Subscribe("a", [](const std::any&) {});
    bus.Subscribe("b", [](const std::any&) {});
    bus.Clear();
    CHECK(bus.HandlerCount("a") == 0);
    CHECK(bus.HandlerCount("b") == 0);
}

TEST_CASE("EventBus: обработчик может отписаться сам во время Emit, не ломая рассылку") {
    EventBus bus;
    int hits = 0;
    int selfId = -1;
    selfId = bus.Subscribe("reentrant", [&](const std::any&) {
        ++hits;
        bus.Unsubscribe(selfId);
    });
    bus.Subscribe("reentrant", [&](const std::any&) { ++hits; });

    bus.Emit("reentrant");
    CHECK(hits == 2); // оба вызваны в первой рассылке — снимок сделан ДО отписки
    bus.Emit("reentrant");
    CHECK(hits == 3); // второй Emit — уже только второй обработчик
}

TEST_CASE("EventBus: обработчик может подписать нового слушателя во время Emit безопасно") {
    EventBus bus;
    int outerHits = 0;
    int innerHits = 0;
    bus.Subscribe("spawn", [&](const std::any&) {
        ++outerHits;
        bus.Subscribe("spawn", [&](const std::any&) { ++innerHits; });
    });
    bus.Emit("spawn"); // новый подписчик не должен вызваться в ЭТОЙ же рассылке
    CHECK(outerHits == 1);
    CHECK(innerHits == 0);
    bus.Emit("spawn"); // а в следующей — уже оба
    CHECK(outerHits == 2);
    CHECK(innerHits == 1);
}

TEST_CASE("EventBus: обработчик может отписать ДРУГОГО подписчика во время Emit") {
    EventBus bus;
    int victimHits = 0;
    int victimId = bus.Subscribe("cancel", [&](const std::any&) { ++victimHits; });
    bus.Subscribe("cancel", [&](const std::any&) { bus.Unsubscribe(victimId); });

    // Порядок подписки: victim первый, канцелятор второй — victim успевает
    // отработать в этой же рассылке (снимок сделан заранее), но не в следующей.
    bus.Emit("cancel");
    CHECK(victimHits == 1);
    bus.Emit("cancel");
    CHECK(victimHits == 1);
}

TEST_CASE("EventBus: Emit на событие без подписчиков не падает") {
    EventBus bus;
    CHECK_NOTHROW(bus.Emit("nobody.listens", std::make_any<int>(1)));
}
