#include "sage/core/Problems.h"
#include "sage/scripting/ScriptAdvice.h"
#include "sage/scripting/ScriptRuntime.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/lang/SageCompiler.h"

namespace fs = std::filesystem;

namespace sage::scripting {

const char* HookName(Hook hook) {
    switch (hook) {
        case Hook::Start: return "Start";
        case Hook::Update: return "Update";
        case Hook::FixedUpdate: return "FixedUpdate";
        case Hook::LateUpdate: return "LateUpdate";
        case Hook::OnEnable: return "OnEnable";
        case Hook::OnDisable: return "OnDisable";
        case Hook::OnDestroy: return "OnDestroy";
        case Hook::OnCollisionEnter: return "OnCollisionEnter";
        case Hook::OnCollisionExit: return "OnCollisionExit";
        case Hook::OnTriggerEnter: return "OnTriggerEnter";
        case Hook::OnTriggerExit: return "OnTriggerExit";
        case Hook::OnTriggerStay: return "OnTriggerStay";
        case Hook::OnAnimationEvent: return "OnAnimationEvent";
        case Hook::OnKeyDown: return "OnKeyDown";
        case Hook::OnKeyUp: return "OnKeyUp";
        case Hook::Count: break;
    }
    return "?";
}

std::string ScriptError::Format() const {
    std::string out;
    if (!File.empty()) {
        out += File;
        if (Line > 0) out += ":" + std::to_string(Line);
        out += ": ";
    }
    out += Message;
    return out;
}

ScriptRuntime::ScriptRuntime() = default;

ScriptRuntime::~ScriptRuntime() {
    // Экземпляры снимаются ДО бэкендов: чужой рантайм, разрушенный раньше
    // своих объектов, отвечает падением на попытку их освободить.
    DetachAll();
    m_backends.clear();
}

void ScriptRuntime::AddBackend(std::unique_ptr<LanguageBackend> backend) {
    if (!backend) return;
    backend->Bind(m_services);
    m_backends.push_back(std::move(backend));
}

void ScriptRuntime::SetProjectDir(const std::filesystem::path& dir) {
    m_projectDir = dir;
    // import "Game/Player" в .slua ищется и от корня проекта: рабочий каталог
    // редактора — не папка проекта.
    std::vector<std::string> roots;
    if (!dir.empty()) roots.push_back(dir.generic_string());
    sage::lang::SetModuleRoots(roots);
}

LanguageBackend* ScriptRuntime::BackendFor(const std::string& path) const {
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (const auto& b : m_backends)
        if (b->Handles(ext)) return b.get();
    return nullptr;
}

void ScriptRuntime::Bind(const ScriptServices& services) {
    m_services = services;
    for (auto& b : m_backends) b->Bind(m_services);
}

long long ScriptRuntime::FileStamp(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}

bool ScriptRuntime::ReadSource(const std::string& path, ScriptSource& out) const {
    if (path.empty()) return false;
    out.Path = path;
    out.Text.clear();
    out.Stamp = 0;

    // Порядок поиска — от самого конкретного к самому общему: как дали (скрипты
    // рядом с редактором, абсолютные пути), затем в папке проекта (пути в сцене
    // записаны относительно неё, а рабочий каталог редактора — не она), затем
    // через vfs (в собранной игре файлов на диске нет вовсе).
    std::error_code ec;
    std::vector<std::string> candidates;
    candidates.push_back(path);
    if (!m_projectDir.empty()) candidates.push_back((m_projectDir / path).string());

    for (const std::string& candidate : candidates) {
        if (!sage::assets::vfs::Exists(candidate)) continue;
        std::string text;
        if (!sage::assets::vfs::ReadText(candidate, text)) continue;
        out.Text = std::move(text);
        // Штамп — у ТОГО файла, который прочитали: сравнивать время правки с
        // чем-то другим значит либо не замечать правку, либо перечитывать
        // каждый кадр.
        out.Stamp = FileStamp(candidate);
        return true;
    }

    // Последняя попытка — vfs как есть: в собранной игре это единственный
    // рабочий путь, и существование файла там проверять нечем.
    std::string text;
    if (sage::assets::vfs::ReadText(path, text)) {
        out.Text = std::move(text);
        out.Stamp = 0; // из пакета: перезагружать нечего и не от чего
        return true;
    }
    return false;
}

sage::vars::Table ScriptRuntime::ParseFields(const std::string& path) const {
    sage::vars::Table empty;
    LanguageBackend* backend = BackendFor(path);
    if (!backend) return empty;
    ScriptSource src;
    if (!ReadSource(path, src)) return empty;
    return backend->ParseFields(path, src.Text);
}

void ScriptRuntime::Reindex() {
    m_index.clear();
    for (size_t i = 0; i < m_scripts.size(); ++i)
        if (!m_scripts[i].Dead)
            m_index[(uint32_t)entt::to_integral(m_scripts[i].Owner.Entity())] = i;
    m_dirtyIndex = false;
}

LiveScript* ScriptRuntime::Find(entt::entity entity) {
    if (m_dirtyIndex) Reindex();
    auto it = m_index.find((uint32_t)entt::to_integral(entity));
    if (it == m_index.end()) return nullptr;
    LiveScript& s = m_scripts[it->second];
    return s.Dead ? nullptr : &s;
}

const LiveScript* ScriptRuntime::Find(entt::entity entity) const {
    return const_cast<ScriptRuntime*>(this)->Find(entity);
}

InstanceId ScriptRuntime::Build(LanguageBackend& backend, const ScriptSource& src,
                                GameObject owner, const sage::vars::Table& fields) {
    ScriptError err;
    const InstanceId id = backend.Create(src, owner, err);
    if (id == kInvalidInstance) {
        // Первопричина — без объекта (она одна на файл, сколько бы объектов
        // его ни держали), а каждый объект без скрипта — её последствие.
        Report(err);
        if (owner.Valid() && err.What == ScriptError::Kind::Compile)
            ReportConsequence("script.not-started", sage::Severity::Error, src.Path, owner.Name(),
                              "Объект «" + owner.Name() + "» остался без скрипта " + src.Path,
                              "Скрипт с ошибкой не запускается: объект работает без него.");
        return kInvalidInstance;
    }
    Resolved(src.Path);
    backend.ApplyFields(id, fields);
    return id;
}

// Значения объекта + ОБЪЯВЛЕНИЕ скрипта. Умолчания подмешиваются здесь, а не
// только в инспекторе: скрипт, привязанный кодом (префаб, спавн из Lua, тест),
// обязан получить свои значения по умолчанию, а не nil на первой же строке.
// Значение из сцены при этом сильнее умолчания — см. Table::MergeDeclaration.
sage::vars::Table ScriptRuntime::MergeFields(LanguageBackend& backend, const ScriptSource& src,
                                             const sage::vars::Table& fields) const {
    sage::vars::Table merged = fields;
    merged.MergeDeclaration(backend.ParseFields(src.Path, src.Text));
    return merged;
}

bool ScriptRuntime::Attach(GameObject owner, const std::string& path,
                           const sage::vars::Table& fields) {
    if (!owner.Valid() || path.empty()) return false;
    LanguageBackend* backend = BackendFor(path);
    if (!backend) {
        ScriptError err;
        err.File = path;
        err.Message = "нет языка скриптов для этого файла";
        Report(err);
        return false;
    }
    ScriptSource src;
    if (!ReadSource(path, src)) {
        ScriptError err;
        err.What = ScriptError::Kind::Missing;
        err.File = path;
        err.Message = "файл скрипта не найден";
        Report(err);
        ReportConsequence("script.not-started", sage::Severity::Error, path, owner.Name(),
                          "Объект «" + owner.Name() + "» остался без скрипта " + path,
                          "Файла скрипта нет — объект работает без него.");
        return false;
    }

    Detach(owner.Entity()); // один объект — один скрипт

    const sage::vars::Table merged = MergeFields(*backend, src, fields);
    const InstanceId id = Build(*backend, src, owner, merged);
    if (id == kInvalidInstance) return false;

    LiveScript s;
    s.Owner = owner;
    s.Backend = backend;
    s.Instance = id;
    s.Path = path;
    s.Stamp = src.Stamp;
    s.Fields = merged;
    m_scripts.push_back(std::move(s));
    m_dirtyIndex = true;

    // Start зовётся СРАЗУ: скрипт, созданный посреди кадра (Instantiate), не
    // должен ждать следующего кадра, чтобы себя настроить. Кроме запуска
    // сцены целиком (BeginBatch): там Start ждёт, пока подключатся ВСЕ, —
    // иначе Start первого скрипта, обратившегося к соседу
    // (`Scene.find("Player"):call(...)`), не находил бы у соседа скрипта.
    if (m_batch > 0) {
        m_pendingStart.push_back(owner.Entity());
        return true;
    }
    StartScript(m_scripts.back());
    return true;
}

void ScriptRuntime::StartScript(LiveScript& live) {
    if (live.Started || live.Dead || live.Instance == kInvalidInstance) return;
    live.Started = true;
    LanguageBackend* backend = live.Backend;
    const InstanceId id = live.Instance;
    if (backend->Has(id, Hook::Start)) {
        ScriptError err;
        Guard(live, backend->Call(id, Hook::Start, 0.0f, err), err, Hook::Start);
    }
    if (live.Enabled && backend->Has(id, Hook::OnEnable)) {
        ScriptError err;
        Guard(live, backend->Call(id, Hook::OnEnable, 0.0f, err), err, Hook::OnEnable);
    }
}

void ScriptRuntime::BeginBatch() { ++m_batch; }

void ScriptRuntime::EndBatch() {
    if (m_batch == 0 || --m_batch > 0) return;
    // По ОДНОМУ и заново ищем: Start вправе создать объект со скриптом, и
    // вектор скриптов перевыделится.
    std::vector<entt::entity> pending;
    pending.swap(m_pendingStart);
    for (entt::entity e : pending)
        if (LiveScript* s = Find(e)) StartScript(*s);
}

void ScriptRuntime::Detach(entt::entity entity) {
    LiveScript* s = Find(entity);
    if (!s) return;
    ScriptError err;
    if (s->Enabled && s->Backend->Has(s->Instance, Hook::OnDisable))
        s->Backend->Call(s->Instance, Hook::OnDisable, 0.0f, err);
    if (s->Backend->Has(s->Instance, Hook::OnDestroy))
        s->Backend->Call(s->Instance, Hook::OnDestroy, 0.0f, err);
    s->Backend->Destroy(s->Instance);
    s->Instance = kInvalidInstance;
    s->Dead = true;
    m_dirtyIndex = true;
}

void ScriptRuntime::DetachAll() {
    for (LiveScript& s : m_scripts) {
        if (s.Dead || s.Instance == kInvalidInstance) continue;
        ScriptError err;
        if (s.Enabled && s.Backend->Has(s.Instance, Hook::OnDisable))
            s.Backend->Call(s.Instance, Hook::OnDisable, 0.0f, err);
        if (s.Backend->Has(s.Instance, Hook::OnDestroy))
            s.Backend->Call(s.Instance, Hook::OnDestroy, 0.0f, err);
        s.Backend->Destroy(s.Instance);
        s.Instance = kInvalidInstance;
        s.Dead = true;
    }
    m_scripts.clear();
    m_index.clear();
    m_dirtyIndex = false;
}

void ScriptRuntime::Report(const ScriptError& err, const std::string& subject) {
    if (m_sink) m_sink(err);
    // В реестр проблем — с объяснением, подсказками и строками кода вокруг
    // (ScriptAdvice.h). Исходник читается здесь, на ошибке: ради пары строк
    // контекста держать тексты всех скриптов в памяти незачем.
    ScriptSource src;
    if (!err.File.empty()) ReadSource(err.File, src);
    sage::Problem p = ToProblem(err, src.Text);
    p.Subject = subject;
    sage::Problems::Report(std::move(p));
}

void ScriptRuntime::ReportConsequence(const std::string& code, sage::Severity level, const std::string& file,
                                      const std::string& subject, const std::string& title,
                                      const std::string& detail) {
    sage::Problem p;
    p.Category = "Script";
    p.Code = code;
    p.Level = level;
    p.Title = title;
    p.Detail = detail;
    p.Subject = subject;
    // О том же файле — значит, в группе его первопричины (Problems.h).
    p.Resource = file;
    sage::Problems::Report(std::move(p));
}

void ScriptRuntime::Resolved(const std::string& file) {
    // Файл собрался — ошибки сборки и «объект без скрипта» больше не правда.
    // Ошибки выполнения остаются: они случились, и код мог их не исправить.
    for (const char* code : {"script.compile", "script.missing", "script.not-started"})
        sage::Problems::Resolve(file, code);
}

void ScriptRuntime::Guard(LiveScript& s, bool ok, ScriptError& err, Hook hook) {
    if (ok) return;
    if (s.Muted) return;
    if (err.File.empty()) err.File = s.Path;
    if (err.Message.empty()) err.Message = std::string("ошибка в ") + HookName(hook);
    Report(err, s.Owner.Valid() ? s.Owner.Name() : std::string());
    // Зависание — сразу в тишину: каждый следующий вызов стоил бы ещё
    // несколько секунд замёрзшего кадра.
    if (++s.Errors >= kMaxErrors || err.What == ScriptError::Kind::Timeout) {
        s.Muted = true;
        // ОСТАЛЬНЫЕ скрипты работают: остановлен только этот, на этом объекте.
        ReportConsequence("script.muted", sage::Severity::Warning, err.File.empty() ? s.Path : err.File,
                          s.Owner.Valid() ? s.Owner.Name() : std::string(),
                          s.Path + ": скрипт остановлен на этом объекте (" + HookName(hook) +
                              (err.What == ScriptError::Kind::Timeout ? " завис)" : " падает раз за разом)"),
                          "Чтобы ошибка не повторялась каждый кадр и не мешала остальному, скрипт этого "
                          "объекта больше не вызывается до перезапуска игры или исправления файла. "
                          "Остальные скрипты сцены работают.");
    }
}

void ScriptRuntime::Dispatch(Hook hook, float dt) {
    // По индексу, а не по итератору: хук может породить объект со скриптом
    // (Instantiate), и вектор перевыделится прямо посреди обхода. Новые
    // скрипты этого кадра свой Start уже получили в Attach, а Update получат
    // со следующего кадра — порядок предсказуем.
    const size_t count = m_scripts.size();
    for (size_t i = 0; i < count && i < m_scripts.size(); ++i) {
        LiveScript& s = m_scripts[i];
        if (s.Dead || s.Muted || !s.Enabled) continue;
        if (!s.Owner.Valid()) {
            // Объект уничтожен: экземпляр уходит СРАЗУ, а с ним — его подписки
            // на сигналы. Раньше он только помечался и оставался жить в языке:
            // его обработчики звались бы на чужие щелчки с мёртвым self.
            if (s.Instance != kInvalidInstance) s.Backend->Destroy(s.Instance);
            s.Instance = kInvalidInstance;
            s.Dead = true;
            m_dirtyIndex = true;
            continue;
        }
        if (!s.Backend->Has(s.Instance, hook)) continue;
        ScriptError err;
        Guard(s, s.Backend->Call(s.Instance, hook, dt, err), err, hook);
    }
    Sweep();
}

void ScriptRuntime::DispatchTo(entt::entity entity, Hook hook, GameObject other) {
    LiveScript* s = Find(entity);
    if (!s || s->Muted || !s->Enabled) return;
    if (!s->Backend->Has(s->Instance, hook)) return;
    ScriptError err;
    Guard(*s, s->Backend->CallWith(s->Instance, hook, other, err), err, hook);
}

void ScriptRuntime::DispatchNamed(entt::entity entity, Hook hook, const std::string& name) {
    LiveScript* s = Find(entity);
    if (!s || s->Muted || !s->Enabled) return;
    if (!s->Backend->Has(s->Instance, hook)) return;
    ScriptError err;
    Guard(*s, s->Backend->CallNamed(s->Instance, hook, name, err), err, hook);
}

bool ScriptRuntime::AnyHas(Hook a, Hook b) const {
    for (const LiveScript& s : m_scripts) {
        if (s.Dead || s.Muted || !s.Enabled || s.Instance == kInvalidInstance) continue;
        if (s.Backend->Has(s.Instance, a) || s.Backend->Has(s.Instance, b)) return true;
    }
    return false;
}

void ScriptRuntime::DispatchNamedAll(Hook hook, const std::string& name) {
    // Тот же обход, что у Dispatch: по индексу — обработчик вправе породить
    // объект со скриптом.
    const size_t count = m_scripts.size();
    for (size_t i = 0; i < count && i < m_scripts.size(); ++i) {
        LiveScript& s = m_scripts[i];
        if (s.Dead || s.Muted || !s.Enabled || !s.Owner.Valid()) continue;
        if (!s.Backend->Has(s.Instance, hook)) continue;
        ScriptError err;
        Guard(s, s.Backend->CallNamed(s.Instance, hook, name, err), err, hook);
    }
}

bool ScriptRuntime::Invoke(entt::entity entity, const std::string& method,
                           const std::vector<sage::vars::Value>& args) {
    LiveScript* s = Find(entity);
    if (!s) return false;
    ScriptError err;
    const bool ok = s->Backend->Invoke(s->Instance, method, args, err);
    if (!ok && !err.Empty()) {
        if (err.File.empty()) err.File = s->Path;
        Report(err);
    }
    return ok;
}

int ScriptRuntime::QueueTests(entt::entity entity) {
    LiveScript* s = Find(entity);
    if (!s || s->Instance == kInvalidInstance) return 0;
    return s->Backend->QueueTests(s->Instance, s->Path);
}

void ScriptRuntime::TickTests(float dt, std::vector<TestResult>& finished) {
    for (const auto& b : m_backends) b->TickTests(dt, finished);
}

int ScriptRuntime::PendingTests() const {
    int n = 0;
    for (const auto& b : m_backends) n += b->PendingTests();
    return n;
}

bool ScriptRuntime::InvokeEvent(entt::entity entity, const std::string& method,
                                const sage::events::Event& event) {
    LiveScript* s = Find(entity);
    if (!s) return false;
    ScriptError err;
    const bool ok = s->Backend->InvokeEvent(s->Instance, method, event, err);
    if (!ok && !err.Empty()) {
        if (err.File.empty()) err.File = s->Path;
        Report(err);
    }
    return ok;
}

void ScriptRuntime::SetEnabled(entt::entity entity, bool enabled) {
    LiveScript* s = Find(entity);
    if (!s || s->Enabled == enabled) return;
    s->Enabled = enabled;
    const Hook hook = enabled ? Hook::OnEnable : Hook::OnDisable;
    if (!s->Backend->Has(s->Instance, hook)) return;
    ScriptError err;
    Guard(*s, s->Backend->Call(s->Instance, hook, 0.0f, err), err, hook);
}

void ScriptRuntime::Tick(float dt) {
    for (auto& b : m_backends) b->Tick(dt);
}

void ScriptRuntime::Sweep() {
    if (!m_dirtyIndex) return;
    m_scripts.erase(std::remove_if(m_scripts.begin(), m_scripts.end(),
                                   [](const LiveScript& s) { return s.Dead; }),
                    m_scripts.end());
    Reindex();
}

int ScriptRuntime::ReloadChanged() {
    int reloaded = 0;
    for (size_t i = 0; i < m_scripts.size(); ++i) {
        LiveScript& s = m_scripts[i];
        if (s.Dead || s.Stamp == 0) continue; // из пакета — перезагружать нечего
        ScriptSource src;
        if (!ReadSource(s.Path, src)) continue;
        if (src.Stamp == s.Stamp || src.Stamp == 0) continue;
        // Штамп запоминаем СРАЗУ, даже если скрипт не собрался: иначе одна и та
        // же опечатка писалась бы в консоль шестьдесят раз в секунду.
        s.Stamp = src.Stamp;
        // Объявление могло измениться вместе с файлом: новая переменная
        // обязана приехать со своим умолчанием, а уже настроенная — остаться.
        s.Fields = MergeFields(*s.Backend, src, s.Fields);
        const InstanceId fresh = Build(*s.Backend, src, s.Owner, s.Fields);
        if (fresh == kInvalidInstance) continue; // прежний экземпляр остаётся жить
        ScriptError err;
        if (s.Backend->Has(s.Instance, Hook::OnDestroy))
            s.Backend->Call(s.Instance, Hook::OnDestroy, 0.0f, err);
        s.Backend->Destroy(s.Instance);
        s.Instance = fresh;
        s.Errors = 0;
        s.Muted = false;
        s.Started = false;
        if (s.Backend->Has(fresh, Hook::Start)) {
            ScriptError startErr;
            Guard(s, s.Backend->Call(fresh, Hook::Start, 0.0f, startErr), startErr, Hook::Start);
        }
        s.Started = true;
        ++reloaded;
    }
    if (reloaded > 0) LOG_INFO("Script") << "перечитано скриптов: " << reloaded;
    return reloaded;
}

int ScriptRuntime::Reload(const std::string& path) {
    int reloaded = 0;
    for (LiveScript& s : m_scripts) {
        if (s.Dead || s.Path != path) continue;
        ScriptSource src;
        if (!ReadSource(s.Path, src)) continue;
        s.Fields = MergeFields(*s.Backend, src, s.Fields);
        const InstanceId fresh = Build(*s.Backend, src, s.Owner, s.Fields);
        if (fresh == kInvalidInstance) continue;
        ScriptError err;
        if (s.Backend->Has(s.Instance, Hook::OnDestroy))
            s.Backend->Call(s.Instance, Hook::OnDestroy, 0.0f, err);
        s.Backend->Destroy(s.Instance);
        s.Instance = fresh;
        s.Stamp = src.Stamp;
        s.Errors = 0;
        s.Muted = false;
        if (s.Backend->Has(fresh, Hook::Start)) {
            ScriptError startErr;
            Guard(s, s.Backend->Call(fresh, Hook::Start, 0.0f, startErr), startErr, Hook::Start);
        }
        s.Started = true;
        ++reloaded;
    }
    return reloaded;
}

} // namespace sage::scripting
