#pragma once
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/core/Problems.h"
#include "sage/scripting/LanguageBackend.h"
#include "sage/scene/Scene.h"

// ---------------------------------------------------------------------------
// РАНТАЙМ СКРИПТОВ — всё, что одинаково для ЛЮБОГО языка.
//
// Чтение файла (диск или .sagepak), выбор языка по расширению, учёт живых
// экземпляров, порядок хуков, горячая перезагрузка по времени правки, гашение
// потока одинаковых ошибок, отчёт в консоль со ссылкой на строку — написано
// ОДИН раз и работает для Lua ровно так же, как будет работать для C#.
//
// Что НЕ здесь: как выглядит API движка внутри языка (это бэкенд) и кто зовёт
// хуки в кадре (это ScriptingSystem). Границы выбраны по тому, что меняется:
// добавление языка не трогает этот файл, добавление системы движка не трогает
// бэкенды.
//
// ПОЧЕМУ ЭКЗЕМПЛЯР ИЩЕТСЯ ПО СУЩНОСТИ. Скрипты разговаривают друг с другом
// (`other:GetScript("Enemy")`, `enemy:Call("TakeDamage", 20)`), то есть по
// объекту сцены надо найти его скрипт. Указатель на экземпляр чужого рантайма
// при этом наружу не отдаётся — только номер.
//
// СКРИПТОВ У ОБЪЕКТА МОЖЕТ БЫТЬ НЕСКОЛЬКО (ScriptComponent::Scripts). Каждый —
// отдельный экземпляр со своим состоянием, полями, счётчиком ошибок и
// перезагрузкой; ошибка или пропавший файл одного не трогает соседей. Хуки
// кадра идут в порядке подключения: объекты по порядку сцены, внутри объекта
// — по порядку в списке компонента.
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Живой скрипт одного объекта.
struct LiveScript {
    GameObject Owner;
    LanguageBackend* Backend = nullptr;
    InstanceId Instance = kInvalidInstance;
    std::string Path;          // как записано в сцене
    // Место в ScriptComponent::Scripts объекта; -1 — подключён кодом мимо
    // компонента (плагин). По нему инспектор и перезагрузка находят «тот же»
    // скрипт, когда у объекта их несколько и пути могут совпадать.
    int Slot = -1;
    long long Stamp = 0;
    // Значения публичных полей ЭТОГО объекта (из сцены). Хранятся здесь,
    // потому что перезагрузка пересобирает экземпляр с нуля: без копии
    // настройки объекта сбрасывались бы на умолчания скрипта при каждой
    // правке файла — то есть ровно в тот момент, когда их подбирают.
    sage::vars::Table Fields;
    bool Started = false;      // Start уже вызван
    bool Enabled = true;       // OnEnable/OnDisable
    bool Dead = false;         // снят; убирается одним проходом в конце кадра

    // Сколько раз хук этого скрипта уже упал. Ошибка в Update повторяется
    // КАЖДЫЙ КАДР: шестьдесят одинаковых строк в секунду забивают консоль так,
    // что в ней не видно ни второй ошибки, ни чего-либо ещё, ради чего в неё
    // смотрят.
    int Errors = 0;
    bool Muted = false;
};

class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // --- Языки ---------------------------------------------------------------
    void AddBackend(std::unique_ptr<LanguageBackend> backend);
    LanguageBackend* BackendFor(const std::string& path) const;
    bool HasBackends() const { return !m_backends.empty(); }

    void Bind(const ScriptServices& services);
    const ScriptServices& Services() const { return m_services; }

    // Корень проекта: пути скриптов в сцене записаны ОТНОСИТЕЛЬНО него
    // ("assets/scripts/player.lua"), а рабочий каталог редактора — не он.
    void SetProjectDir(const std::filesystem::path& dir);

    // Читает исходник (диск, затем папка проекта, затем vfs/.sagepak).
    bool ReadSource(const std::string& path, ScriptSource& out) const;

    // Публичные поля, объявленные файлом. Пустая таблица — не ошибка.
    sage::vars::Table ParseFields(const std::string& path) const;

    // --- Экземпляры ----------------------------------------------------------

    // Привязывает скрипт к объекту ВМЕСТО всех его скриптов (прежний смысл
    // для плагинов и кода, собирающего объект с одним скриптом). false —
    // файла нет или он не собрался; причина уже в консоли.
    bool Attach(GameObject owner, const std::string& path, const sage::vars::Table& fields);
    // Добавляет ещё один скрипт объекту, не трогая остальные. nullptr —
    // файла нет или он не собрался (причина в консоли); соседи работают.
    // Указатель живёт до конца кадра (уборка снятых — в Dispatch).
    LiveScript* Add(GameObject owner, const std::string& path, const sage::vars::Table& fields, int slot = -1);
    // Подключение пачкой (запуск сцены): Attach внутри Begin/EndBatch не зовёт
    // Start сразу, а EndBatch зовёт Start всех в порядке подключения — когда
    // каждый скрипт уже может найти любого соседа.
    void BeginBatch();
    void EndBatch();

    // Снимает ВСЕ скрипты объекта (зовёт OnDisable/OnDestroy).
    void Detach(entt::entity entity);
    void DetachAll();

    // Первый скрипт объекта (по порядку списка) — для мест, где «скрипт
    // объекта» один по смыслу.
    LiveScript* Find(entt::entity entity);
    const LiveScript* Find(entt::entity entity) const;
    // Все живые скрипты объекта в порядке исполнения.
    std::vector<LiveScript*> FindAll(entt::entity entity);
    // Первый скрипт объекта, у которого есть метод: `enemy:Call("TakeDamage")`
    // и связи инспектора адресуют МЕТОД объекта, не зная, какой из его
    // скриптов его объявил.
    LiveScript* FindMethod(entt::entity entity, const std::string& method);

    // --- Кадр ----------------------------------------------------------------

    // Зовёт хук у ВСЕХ живых скриптов (Update/FixedUpdate/LateUpdate).
    void Dispatch(Hook hook, float dt);

    // Зовёт хук у скриптов одной сущности (столкновения, зоны) — у каждого.
    void DispatchTo(entt::entity entity, Hook hook, GameObject other);
    void DispatchNamed(entt::entity entity, Hook hook, const std::string& name);
    // Всем скриптам, у которых хук есть: OnKeyDown("Space") и подобное —
    // событие мира, а не одного объекта.
    void DispatchNamedAll(Hook hook, const std::string& name);
    // Есть ли хоть у одного живого скрипта один из двух хуков.
    bool AnyHas(Hook a, Hook b) const;

    // Вызов метода чужого скрипта: `enemy:Call("TakeDamage", 20)` из Lua и то
    // же самое из C++ — одна дорога.
    bool Invoke(entt::entity entity, const std::string& method,
                const std::vector<sage::vars::Value>& args);
    // То же с ответом метода — так разговаривают скрипты на РАЗНЫХ языках:
    // `enemy:Call("TakeDamage", 20)` из Lua доходит до скрипта на любом языке.
    bool Invoke(entt::entity entity, const std::string& method, const std::vector<sage::vars::Value>& args,
                std::vector<sage::vars::Value>& results);

    // Метод скрипта объекта как обработчик сигнала (связи инспектора).
    bool InvokeEvent(entt::entity entity, const std::string& method, const sage::events::Event& event);

    // Включение/выключение объекта: хуки OnEnable/OnDisable зовутся ровно на
    // КРАЮ, а не каждый кадр, пока объект включён.
    void SetEnabled(entt::entity entity, bool enabled);

    // Тесты на языке скриптов (ScriptTests.h): поставить в очередь тесты
    // скриптов объекта, шагнуть очередь, сколько осталось.
    int QueueTests(entt::entity entity);
    void TickTests(float dt, std::vector<TestResult>& finished);
    int PendingTests() const;

    // Тик рантаймов языков (таймеры, корутины).
    void Tick(float dt);

    // ГОРЯЧАЯ ПЕРЕЗАГРУЗКА: перечитывает скрипты, файлы которых изменились.
    // Возвращает, сколько перечитано. Скрипт с опечаткой НЕ заменяет рабочий:
    // недописанная строка — обычное состояние файла в середине правки.
    int ReloadChanged();

    // Полная перезагрузка одного файла у всех, кто его держит (правка в
    // редакторе кода, «перезапустить скрипт»).
    int Reload(const std::string& path);

    size_t Count() const { return m_scripts.size(); }

    // Куда уходят ошибки. По умолчанию — в лог категории языка; редактор
    // подменяет, чтобы отдать консоли файл и строку для перехода.
    using ErrorSink = std::function<void(const ScriptError&)>;
    void SetErrorSink(ErrorSink sink) { m_sink = std::move(sink); }

    // Предел одного вызова скрипта (сторож зависаний), секунды.
    void SetCallBudget(double seconds) {
        for (auto& b : m_backends) b->SetCallBudget(seconds);
    }
    // Предел памяти каждого языка, байты (0 — без предела).
    void SetMemoryLimit(size_t bytes) {
        for (auto& b : m_backends) b->SetMemoryLimit(bytes);
    }

private:
    void StartScript(LiveScript& live);
    // Язык и исходник файла; нет — сообщает в консоль и отвечает false.
    bool Open(GameObject owner, const std::string& path, LanguageBackend*& backend, ScriptSource& src);
    LiveScript* Spawn(LanguageBackend& backend, const ScriptSource& src, GameObject owner,
                      const sage::vars::Table& fields, int slot);
    // OnDisable/OnDestroy и освобождение одного экземпляра.
    void Kill(LiveScript& s);
    // Пересобрать экземпляр по свежему исходнику, сохранив значения полей.
    bool Rebuild(LiveScript& s, const ScriptSource& src);
    // Один вызов хука с разбором ошибки и гашением повторов.
    void Guard(LiveScript& s, bool ok, ScriptError& err, Hook hook);
    // В реестр проблем (Problems.h) с объяснением; subject — объект.
    void Report(const ScriptError& err, const std::string& subject = {});
    // Последствие ошибки в файле file: объект без скрипта, скрипт остановлен.
    void ReportConsequence(const std::string& code, sage::Severity level, const std::string& file,
                           const std::string& subject, const std::string& title, const std::string& detail);
    // Файл собрался — снять его ошибки сборки и их последствия.
    void Resolved(const std::string& file);
    // Создаёт экземпляр по уже прочитанному исходнику.
    InstanceId Build(LanguageBackend& backend, const ScriptSource& src, GameObject owner,
                     const sage::vars::Table& fields);
    // Значения объекта + объявление скрипта (умолчания новых переменных).
    sage::vars::Table MergeFields(LanguageBackend& backend, const ScriptSource& src,
                                  const sage::vars::Table& fields) const;
    void Sweep(); // убирает Dead одним проходом
    static long long FileStamp(const std::string& path);

    std::vector<std::unique_ptr<LanguageBackend>> m_backends;
    ScriptServices m_services;
    std::filesystem::path m_projectDir;

    // Последовательность, а не map: порядок обхода кадра обязан быть
    // стабильным (иначе поведение игры зависит от раскладки хеша), а поиск по
    // сущности закрыт отдельным индексом.
    //
    // deque, а не vector: хук вправе породить объект со скриптом
    // (Instantiate в Start/Update), и у vector это перевыделение ПОСРЕДИ
    // вызова — ссылка на текущий скрипт, по которой потом пишется счётчик
    // ошибок и Started, повисала. deque при добавлении в конец адресов не
    // двигает.
    std::deque<LiveScript> m_scripts;
    std::unordered_map<uint32_t, std::vector<size_t>> m_index;
    bool m_dirtyIndex = false;
    void Reindex();

    ErrorSink m_sink;
    // Запуск сцены целиком: Start всех — после того, как подключены все.
    int m_batch = 0;
    struct PendingStart {
        entt::entity Entity;
        LanguageBackend* Backend;
        InstanceId Instance;
    };
    std::vector<PendingStart> m_pendingStart;
    // Сколько падений одного скрипта терпим, прежде чем замолчать.
    static constexpr int kMaxErrors = 3;
};

} // namespace sage::scripting
