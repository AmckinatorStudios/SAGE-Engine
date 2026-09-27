#include "sage/events/Events.h"

#include <algorithm>
#include <atomic>
#include <exception>

#include "sage/core/Log.h"

namespace sage::events {

namespace {
// Два обработчика, шлющие событие друг другу, иначе роняют движок
// переполнением стека C++ — то есть ошибка в НАСТРОЙКЕ кнопки убивает игру без
// единого понятного слова в логе.
constexpr int kMaxDepth = 16;
// Столько проходов делает разбор очереди — см. DispatchQueued.
constexpr int kMaxQueuePasses = 8;

// Номера соединений и групп — общие на процесс (см. Bus::Connect).
std::atomic<int> g_nextConnection{1};
std::atomic<int> g_nextGroup{1};

std::string Where(int object, const std::string& name) {
    return object == Bus::kGlobal ? "'" + name + "'"
                                  : "'" + name + "' объекта " + std::to_string(object);
}
} // namespace

int Bus::NewGroup() { return g_nextGroup++; }

int Bus::Connect(int object, const std::string& signal, Handler handler, ConnectOptions options) {
    if (signal.empty()) {
        LOG_ERROR("Events") << "Подписка не заведена: пустое имя события"
                            << (object ? " (объект " + std::to_string(object) + ")" : std::string());
        return 0;
    }
    if (!handler) {
        LOG_ERROR("Events") << "Подписка на " << Where(object, signal)
                            << " не заведена: обработчик пуст";
        return 0;
    }
    Slot s;
    s.Id = g_nextConnection++;
    s.Object = object;
    s.Name = signal;
    s.Fn = std::move(handler);
    s.Options = options;
    m_slots.push_back(std::move(s));
    return m_slots.back().Id;
}

int Bus::OnAny(Handler handler) {
    if (!handler) return 0;
    Slot s;
    s.Id = g_nextConnection++;
    s.Fn = std::move(handler);
    s.Any = true;
    m_slots.push_back(std::move(s));
    return m_slots.back().Id;
}

int Bus::Kill(const std::function<bool(const Slot&)>& match) {
    int n = 0;
    for (Slot& s : m_slots) {
        if (s.Dead || !match(s)) continue;
        s.Dead = true;
        ++n;
    }
    if (n) Sweep();
    return n;
}

bool Bus::Disconnect(int id) {
    if (id <= 0) return false;
    return Kill([id](const Slot& s) { return s.Id == id; }) > 0;
}

bool Bus::IsConnected(int id) const {
    for (const Slot& s : m_slots)
        if (s.Id == id) return !s.Dead;
    return false;
}

int Bus::DisconnectSignal(int object, const std::string& signal, int group) {
    return Kill([&](const Slot& s) {
        return !s.Any && s.Object == object && s.Name == signal &&
               (group == 0 || s.Options.Group == group);
    });
}

int Bus::DisconnectObject(int object) {
    if (object == kGlobal) return 0;
    return Kill([object](const Slot& s) { return !s.Any && s.Object == object; });
}

int Bus::DisconnectOwner(int group, int owner) {
    return Kill([=](const Slot& s) { return s.Options.Group == group && s.Options.Owner == owner; });
}

int Bus::DisconnectGroup(int group) {
    if (group == 0) return 0;
    return Kill([group](const Slot& s) { return s.Options.Group == group; });
}

void Bus::Clear() {
    // Пометкой, а не очисткой: Clear могут позвать ИЗ обработчика (скрипт
    // просит сменить уровень), и стирание вектора под ногами у Emit — это
    // висячая ссылка.
    for (Slot& s : m_slots) s.Dead = true;
    // Неразобранные события выгруженного уровня не должны догнать следующий:
    // «дверь открылась» из прошлой сцены в новой означает чужую дверь.
    m_queued.clear();
    m_deferred.clear();
    Sweep();
}

void Bus::Sweep() {
    // Внутри рассылки чужой снимок ещё может ссылаться на слот — уборка
    // только на верхнем уровне.
    if (m_depth != 0 || m_sweeping) return;
    m_sweeping = true;
    m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
                                 [](const Slot& s) { return s.Dead; }),
                  m_slots.end());
    m_sweeping = false;
}

void Bus::Emit(const Event& event) {
    if (event.Name.empty()) {
        LOG_ERROR("Events") << "Событие без имени не послано"
                            << (event.Object ? " (объект " + std::to_string(event.Object) + ")"
                                             : std::string());
        return;
    }
    if (m_depth >= kMaxDepth) {
        LOG_ERROR("Events") << "Превышена глубина вложенной рассылки (" << kMaxDepth
                            << ") на событии " << Where(event.Object, event.Name)
                            << " — обработчики шлют события друг другу по кругу";
        return;
    }
    ++m_depth;

    // Снимок ЖИВЫХ подписчиков: обработчик волен подписаться, отписаться и
    // уничтожить объект — любое из этого меняет вектор под итератором. Номер
    // слота в векторе на время рассылки не меняется: уборка идёт только на
    // верхнем уровне (Sweep), а новые подписки дописываются в конец.
    struct Call {
        size_t Index;
        Handler Fn;   // копия: вектор может переехать, пока функция исполняется
    };
    std::vector<Call> snapshot;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const Slot& s = m_slots[i];
        if (s.Dead) continue;
        if (!s.Any && (s.Object != event.Object || s.Name != event.Name)) continue;
        snapshot.push_back({i, s.Fn});
    }

    for (const Call& c : snapshot) {
        // Соединение сняли, пока шла рассылка (обработчик выше по списку
        // отписал соседа или уничтожил объект), или одноразовое уже сработало
        // во вложенной рассылке — его больше не зовём: снятое обязано
        // замолчать сразу, а не «со следующего раза».
        Slot& slot = m_slots[c.Index];
        if (slot.Dead) continue;
        // Одноразовое гасится ДО вызова: обработчик может слать то же событие.
        if (slot.Options.Once) slot.Dead = true;
        // Исключение одного обработчика не отменяет остальных и не рвёт
        // рассылку посередине: кнопка, у которой сломан один из трёх
        // слушателей, обязана выполнить два других.
        try {
            c.Fn(event);
        } catch (const std::exception& ex) {
            LOG_ERROR("Events") << "Обработчик события " << Where(event.Object, event.Name)
                                << " упал: " << ex.what();
        } catch (...) {
            LOG_ERROR("Events") << "Обработчик события " << Where(event.Object, event.Name)
                                << " упал с неизвестной ошибкой";
        }
    }

    --m_depth;
    Sweep();
}

void Bus::Queue(const std::string& name, const sage::vars::Value& arg, int sender) {
    Event e;
    e.Name = name;
    e.Arg = arg;
    e.Sender = sender;
    Queue(e);
}

void Bus::Defer(const std::string& name, const sage::vars::Value& arg, int sender) {
    Event e;
    e.Name = name;
    e.Arg = arg;
    e.Sender = sender;
    Defer(e);
}

void Bus::DispatchQueued() {
    // Очередь забирается ЦЕЛИКОМ на каждый проход: обработчик волен положить в
    // неё новое событие, а дописывание в вектор, по которому идёт цикл, — это
    // переаллокация под ногами у итератора.
    //
    // Проходов несколько, потому что цепочка «событие -> действие -> событие»
    // обязана доиграться в ЭТОМ кадре: разложенная по кадрам, она превращает
    // открытие двери в лестницу задержек. Но и не бесконечно: два события,
    // кладущих друг друга в очередь, иначе зациклят кадр насмерть.
    for (int pass = 0; pass < kMaxQueuePasses && !m_queued.empty(); ++pass) {
        std::vector<Event> batch;
        batch.swap(m_queued);
        for (const Event& e : batch) Emit(e);
    }
    if (!m_queued.empty()) {
        LOG_ERROR("Events") << "Очередь событий не разобралась за " << kMaxQueuePasses
                            << " проходов — события кладут друг друга по кругу; осталось "
                            << m_queued.size();
        m_queued.clear();
    }
}

void Bus::DispatchDeferred() {
    std::vector<Event> batch;
    batch.swap(m_deferred);
    for (const Event& e : batch) Emit(e);
}

int Bus::Count(int object, const std::string& signal) const {
    int n = 0;
    for (const Slot& s : m_slots)
        if (!s.Dead && !s.Any && s.Object == object && s.Name == signal) ++n;
    return n;
}

std::vector<std::string> Bus::Names(int object) const {
    std::vector<std::string> out;
    for (const Slot& s : m_slots) {
        if (s.Dead || s.Any || s.Object != object) continue;
        if (std::find(out.begin(), out.end(), s.Name) == out.end()) out.push_back(s.Name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace sage::events
