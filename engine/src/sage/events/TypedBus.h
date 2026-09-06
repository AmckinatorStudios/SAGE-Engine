#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ТИПИЗИРОВАННАЯ ШИНА СОБЫТИЙ — подсистемы разговаривают, не зная друг о друге.
//
// Рядом с именной шиной (Events.h), а не вместо неё, и это разделение по делу:
//
//   Events::Bus — события ПО ИМЕНИ со значением-переменной. Ими говорят данные:
//                 кнопка в инспекторе, скрипт на Lua, связь, настроенная мышью.
//                 Имя приходит из файла, поэтому тип полезной нагрузки заранее
//                 неизвестен и проверить его компилятором нельзя.
//
//   TypedBus    — события КАК ТИПЫ C++. Ими говорит код движка: столкновение,
//                 урон, смерть, загрузка сцены. Здесь компилятор обязан ловить
//                 «подписался на DamageEvent, а поле назвал Ammount» — событие,
//                 у которого молча не сработал обработчик, ищут часами.
//
// ТРИ СРОКА ЖИЗНИ (§18 ТЗ), потому что «сразу» подходит не всему:
//   Publish — немедленно, в стеке отправителя. Нажатие кнопки: задержка на
//             кадр была бы видна как «интерфейс тормозит».
//   Queue   — в очередь кадра, разбирается в отведённой фазе. Урон, создание
//             сущностей: порядок обработки становится предсказуемым, а не
//             зависящим от того, кто первым позвал.
//   Defer   — после текущей фазы обновления. Единственный безопасный способ
//             УДАЛИТЬ сущность из обработчика события: удалить её сразу
//             значит выдернуть память из-под цикла, который ещё идёт.
// ---------------------------------------------------------------------------
namespace sage::events {

// Тождество типа без RTTI: у каждого T свой адрес статической переменной.
// dynamic_cast и typeid здесь не нужны — сравнивается указатель, а не строка,
// и это работает даже там, где RTTI выключено ради размера сборки.
using TypeId = const void*;

template <typename T>
TypeId TypeOf() {
    static const char tag = 0;
    return &tag;
}

class TypedBus {
public:
    // Возвращает номер подписки — по нему её снимают. Возвращать функцию
    // нельзя: две одинаковые лямбды неразличимы, снять нужную было бы нечем.
    template <typename T>
    int Subscribe(std::function<void(const T&)> handler) {
        if (!handler) return 0;
        const int id = m_next++;
        Slot slot;
        slot.Id = id;
        slot.Type = TypeOf<T>();
        slot.Fn = [handler = std::move(handler)](const void* payload) {
            handler(*static_cast<const T*>(payload));
        };
        m_slots.push_back(std::move(slot));
        return id;
    }

    // Одноразовая подписка: сработала — снялась. Без неё «дождаться первого
    // попадания» пишут как подписку, снимающую саму себя, и в этой строчке
    // ошибаются все.
    template <typename T>
    int SubscribeOnce(std::function<void(const T&)> handler) {
        const int id = Subscribe<T>(std::move(handler));
        for (Slot& s : m_slots)
            if (s.Id == id) s.Once = true;
        return id;
    }

    void Unsubscribe(int id);
    void Clear();

    template <typename T>
    void Publish(const T& event) {
        Dispatch(TypeOf<T>(), &event);
    }

    template <typename T>
    void Queue(const T& event) {
        // Копия, а не ссылка: событие переживёт отправителя — его разберут,
        // когда локальная переменная давно уничтожена.
        m_queued.push_back([this, copy = event] { Publish(copy); });
    }

    template <typename T>
    void Defer(const T& event) {
        m_deferred.push_back([this, copy = event] { Publish(copy); });
    }

    // Разобрать очередь кадра. События, добавленные обработчиками, попадают в
    // тот же проход (цепочки должны доигрываться в этом кадре, §17), но не
    // бесконечно: глубина ограничена, иначе два обработчика, шлющие событие
    // друг другу, зациклят кадр насмерть.
    void DispatchQueued();

    // Разобрать отложенные. Зовётся движком ПОСЛЕ фазы обновления, когда
    // менять состав сцены безопасно.
    void DispatchDeferred();

    template <typename T>
    int Count() const {
        int n = 0;
        for (const Slot& s : m_slots)
            if (!s.Dead && s.Type == TypeOf<T>()) ++n;
        return n;
    }

    size_t QueuedCount() const { return m_queued.size(); }
    size_t DeferredCount() const { return m_deferred.size(); }

private:
    struct Slot {
        int Id = 0;
        TypeId Type = nullptr;
        std::function<void(const void*)> Fn;
        bool Once = false;
        bool Dead = false;
    };

    void Dispatch(TypeId type, const void* payload);
    void Sweep();

    std::vector<Slot> m_slots;
    std::vector<std::function<void()>> m_queued;
    std::vector<std::function<void()>> m_deferred;
    int m_next = 1;
    int m_depth = 0;
};

} // namespace sage::events
