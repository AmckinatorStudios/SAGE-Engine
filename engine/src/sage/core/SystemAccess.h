#pragma once
#include <cstdint>
#include <string>
#include <typeinfo>
#include <vector>

// ---------------------------------------------------------------------------
// Объявление доступа системы кадра: что она читает, что пишет и где обязана
// выполняться. По этим объявлениям планировщик систем (SystemScheduler)
// строит граф кадра: системы без конфликтов идут параллельно на воркерах,
// конфликтующие — в порядке регистрации.
//
// КОНФЛИКТ — две системы трогают одно и то же, и хотя бы одна пишет: запись и
// запись, запись и чтение. Чтение и чтение не конфликтуют. Кроме компонентов
// бывают общие РЕСУРСЫ — то, что не компонент сцены: мир физики, звуковой
// движок, Lua-состояние. Они объявляются именами.
//
// ПО УМОЛЧАНИЮ СИСТЕМА ЭКСКЛЮЗИВНА И ИДЁТ В ГЛАВНОМ ПОТОКЕ. Это не
// осторожность ради осторожности: старая система, ничего не объявившая, может
// делать что угодно (создавать сущности, звать Lua, трогать окно), и пустить
// её параллельно — значит устроить гонку, которую никто не заказывал.
// Параллельной система становится только тогда, когда сама это заявила
// (SystemAccess::Parallel()).
//
// ПРОВЕРКА В РАБОТЕ. Пока система выполняется, её объявление доступно потоку
// (CurrentSystemAccess). Помощники ECS (sage::ecs::ParallelEach,
// CheckRead/CheckWrite) сверяются с ним: обращение к необъявленному компоненту
// — запись в лог (один раз на пару «система — компонент») и в счётчик
// нарушений. Объявление, которое врёт, обнаруживается в первом же кадре, а не
// гонкой через неделю.
// ---------------------------------------------------------------------------
namespace sage {

using ComponentId = uint32_t;

// Идентификатор типа по имени typeid — один и тот же в движке и в модуле
// игры (статический счётчик в шаблоне дал бы в каждой DLL свой).
ComponentId RegisterComponentAccessName(const char* typeName);
std::string ComponentAccessName(ComponentId id);

template <class T>
ComponentId ComponentIdOf() {
    static const ComponentId id = RegisterComponentAccessName(typeid(T).name());
    return id;
}

struct SystemAccess {
    std::vector<ComponentId> Reads;
    std::vector<ComponentId> Writes;
    std::vector<std::string> ReadsResources;
    std::vector<std::string> WritesResources;
    // Явный порядок внутри стадии: выполнить после этих систем (по именам),
    // даже если по данным они не конфликтуют.
    std::vector<std::string> After;
    bool Exclusive = true;   // может трогать что угодно: ни с кем параллельно
    bool MainThread = true;  // только главный поток (Lua, ImGui, окно, GL)

    // Система, которую можно запускать на воркерах параллельно с другими.
    static SystemAccess Parallel() {
        SystemAccess a;
        a.Exclusive = false;
        a.MainThread = false;
        return a;
    }
    // Не эксклюзивна, но в главном потоке (параллельна воркерным системам).
    static SystemAccess MainThreadOnly() {
        SystemAccess a;
        a.Exclusive = false;
        a.MainThread = true;
        return a;
    }

    template <class... T>
    SystemAccess& Read() {
        (Reads.push_back(ComponentIdOf<T>()), ...);
        return *this;
    }
    template <class... T>
    SystemAccess& Write() {
        (Writes.push_back(ComponentIdOf<T>()), ...);
        return *this;
    }
    SystemAccess& ReadResource(std::string name) {
        ReadsResources.push_back(std::move(name));
        return *this;
    }
    SystemAccess& WriteResource(std::string name) {
        WritesResources.push_back(std::move(name));
        return *this;
    }
    SystemAccess& RunAfter(std::string system) {
        After.push_back(std::move(system));
        return *this;
    }

    bool ConflictsWith(const SystemAccess& other) const;
    // Разрешён ли доступ к компоненту (запись разрешает и чтение).
    bool Allows(ComponentId id, bool write) const;
    std::string Describe() const;
};

// --- Проверка доступа в работе ------------------------------------------------
// Объявление системы, выполняющейся на этом потоке (nullptr — вне систем).
const SystemAccess* CurrentSystemAccess();
const char* CurrentSystemName();

// Ставит объявление на время выполнения системы (зовёт планировщик).
class ScopedSystemAccess {
public:
    ScopedSystemAccess(const SystemAccess* access, const char* name);
    ~ScopedSystemAccess();
    ScopedSystemAccess(const ScopedSystemAccess&) = delete;
    ScopedSystemAccess& operator=(const ScopedSystemAccess&) = delete;

private:
    const SystemAccess* m_prevAccess;
    const char* m_prevName;
};

// false — система не объявляла этот доступ (нарушение записано). Вне систем
// и для эксклюзивных систем — всегда true.
bool CheckComponentAccess(ComponentId id, bool write);
int ComponentAccessViolations();

template <class T>
bool CheckRead() { return CheckComponentAccess(ComponentIdOf<T>(), false); }
template <class T>
bool CheckWrite() { return CheckComponentAccess(ComponentIdOf<T>(), true); }

} // namespace sage
