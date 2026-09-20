#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Живой скрипт одного объекта.
struct LiveScript {
    GameObject Owner;
    LanguageBackend* Backend = nullptr;
    InstanceId Instance = kInvalidInstance;
    std::string Path;          // как записано в сцене
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
    void SetProjectDir(const std::filesystem::path& dir) { m_projectDir = dir; }

    // Читает исходник (диск, затем папка проекта, затем vfs/.sagepak).
    bool ReadSource(const std::string& path, ScriptSource& out) const;

    // Публичные поля, объявленные файлом. Пустая таблица — не ошибка.
    sage::vars::Table ParseFields(const std::string& path) const;

    // --- Экземпляры ----------------------------------------------------------

    // Привязывает скрипт к объекту. false — файла нет или он не собрался;
    // причина уже в консоли. Существующий скрипт этого объекта снимается.
    bool Attach(GameObject owner, const std::string& path, const sage::vars::Table& fields);

    // Снимает скрипт объекта (зовёт OnDisable/OnDestroy).
    void Detach(entt::entity entity);
    void DetachAll();

    LiveScript* Find(entt::entity entity);
    const LiveScript* Find(entt::entity entity) const;

    // --- Кадр ----------------------------------------------------------------

    // Зовёт хук у ВСЕХ живых скриптов (Update/FixedUpdate/LateUpdate).
    void Dispatch(Hook hook, float dt);

    // Зовёт хук у скрипта одной сущности (столкновения, зоны).
    void DispatchTo(entt::entity entity, Hook hook, GameObject other);
    void DispatchNamed(entt::entity entity, Hook hook, const std::string& name);

    // Вызов метода чужого скрипта: `enemy:Call("TakeDamage", 20)` из Lua и то
    // же самое из C++ — одна дорога.
    bool Invoke(entt::entity entity, const std::string& method,
                const std::vector<sage::vars::Value>& args);

    // Включение/выключение объекта: хуки OnEnable/OnDisable зовутся ровно на
    // КРАЮ, а не каждый кадр, пока объект включён.
    void SetEnabled(entt::entity entity, bool enabled);

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

private:
    // Один вызов хука с разбором ошибки и гашением повторов.
    void Guard(LiveScript& s, bool ok, ScriptError& err, Hook hook);
    void Report(const ScriptError& err);
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

    // Вектор, а не map: скриптов десятки, порядок обхода кадра обязан быть
    // стабильным (иначе поведение игры зависит от раскладки хеша), а поиск по
    // сущности закрыт отдельным индексом.
    std::vector<LiveScript> m_scripts;
    std::unordered_map<uint32_t, size_t> m_index;
    bool m_dirtyIndex = false;
    void Reindex();

    ErrorSink m_sink;
    // Сколько падений одного скрипта терпим, прежде чем замолчать.
    static constexpr int kMaxErrors = 3;
};

} // namespace sage::scripting
