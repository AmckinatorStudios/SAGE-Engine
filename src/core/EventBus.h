#pragma once
#include <any>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------
// EventBus — генерическая шина событий (pub/sub) для слабой связи между
// подсистемами и игровой логикой. Кто-то публикует именованное событие с
// произвольной полезной нагрузкой (std::any), кто-то другой подписан на это
// имя и получает нагрузку — при этом издатель и подписчик не знают друг о
// друге. Часть ЯДРА движка: сам бус ничего не знает ни про Lua, ни про
// конкретную игру — Lua-биндинги (On/Emit/Off) строятся поверх него в
// ScriptEngine, а C++-код может подписываться/публиковать напрямую.
//
// Нагрузка — std::any: подписчик на C++-стороне достаёт свой тип через
// std::any_cast; Lua-слой кладёт/достаёт sol::object (см. ScriptEngine::
// BindEvents). Событие без данных публикуется с пустым std::any.
//
// Реентерабельность: Emit снимает КОПИЮ списка обработчиков перед вызовом,
// поэтому обработчик может безопасно подписываться/отписываться/публиковать
// во время диспетчеризации, не ломая итерацию. Обработчик, отписавшийся
// (сам или другой) во время этого же Emit, дальше в текущей рассылке не
// вызывается — проверяется по актуальности id.
//
// Использование:
//   EventBus bus;
//   int id = bus.Subscribe("player.died", [](const std::any& data) {
//       int score = std::any_cast<int>(data);
//       ...
//   });
//   bus.Emit("player.died", std::make_any<int>(1200));
//   bus.Unsubscribe(id);
// ---------------------------------------------------------------------
class EventBus {
public:
    using Handler = std::function<void(const std::any&)>;

    // Подписывает обработчик на именованное событие. Возвращает id подписки
    // для последующего Unsubscribe. id уникален в пределах шины и не
    // переиспользуется.
    int Subscribe(const std::string& event, Handler handler) {
        int id = m_nextId++;
        m_handlers[event].push_back({id, std::move(handler)});
        m_idToEvent[id] = event;
        return id;
    }

    // Снимает подписку по id. Если id неизвестен (уже снят или никогда не
    // существовал) — тихо ничего не делает.
    void Unsubscribe(int id) {
        auto it = m_idToEvent.find(id);
        if (it == m_idToEvent.end()) return;
        auto listIt = m_handlers.find(it->second);
        if (listIt != m_handlers.end()) {
            auto& list = listIt->second;
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].Id == id) { list.erase(list.begin() + i); break; }
            }
            if (list.empty()) m_handlers.erase(listIt);
        }
        m_idToEvent.erase(it);
    }

    // Публикует событие. Синхронно вызывает всех текущих подписчиков этого
    // имени с переданной нагрузкой. Порядок вызова — порядок подписки.
    void Emit(const std::string& event, const std::any& payload = std::any{}) {
        auto it = m_handlers.find(event);
        if (it == m_handlers.end()) return;
        // Снимок: защищает от модификации списка обработчиком во время рассылки.
        std::vector<Subscription> snapshot = it->second;
        for (const auto& sub : snapshot) {
            // Пропускаем тех, кого отписали уже во время этой же рассылки.
            if (m_idToEvent.find(sub.Id) == m_idToEvent.end()) continue;
            if (sub.Fn) sub.Fn(payload);
        }
    }

    // Сколько сейчас активных подписок на данное имя события.
    size_t HandlerCount(const std::string& event) const {
        auto it = m_handlers.find(event);
        return it == m_handlers.end() ? 0 : it->second.size();
    }

    // Снимает ВСЕ подписки на все события. Полезно при выгрузке уровня/сцены,
    // чтобы висящие обработчики (в т.ч. держащие Lua-замыкания) не пережили
    // контекст, в котором создавались.
    void Clear() {
        m_handlers.clear();
        m_idToEvent.clear();
    }

private:
    struct Subscription {
        int Id;
        Handler Fn;
    };

    std::unordered_map<std::string, std::vector<Subscription>> m_handlers;
    std::unordered_map<int, std::string> m_idToEvent; // id -> имя события (для Unsubscribe)
    int m_nextId = 1;
};
