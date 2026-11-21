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
            m_index[(uint32_t)entt::to_integral(m_scripts[i].Owner.Entity())].push_back(i);
    m_dirtyIndex = false;
}

std::vector<LiveScript*> ScriptRuntime::FindAll(entt::entity entity) {
    std::vector<LiveScript*> out;
    if (m_dirtyIndex) Reindex();
    auto it = m_index.find((uint32_t)entt::to_integral(entity));
    if (it == m_index.end()) return out;
    for (size_t i : it->second)
        if (i < m_scripts.size() && !m_scripts[i].Dead) out.push_back(&m_scripts[i]);
    return out;
}

LiveScript* ScriptRuntime::Find(entt::entity entity) {
    if (m_dirtyIndex) Reindex();
    auto it = m_index.find((uint32_t)entt::to_integral(entity));
    if (it == m_index.end()) return nullptr;
    for (size_t i : it->second)
        if (i < m_scripts.size() && !m_scripts[i].Dead) return &m_scripts[i];
    return nullptr;
}

const LiveScript* ScriptRuntime::Find(entt::entity entity) const {
    return const_cast<ScriptRuntime*>(this)->Find(entity);
}

LiveScript* ScriptRuntime::FindMethod(entt::entity entity, const std::string& method) {
    for (LiveScript* s : FindAll(entity))
        if (s->Instance != kInvalidInstance && s->Backend->HasMethod(s->Instance, method)) return s;
    return nullptr;
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

bool ScriptRuntime::Open(GameObject owner, const std::string& path, LanguageBackend*& backend,
                         ScriptSource& src) {
    backend = BackendFor(path);
    if (!backend) {
        ScriptError err;
        err.File = path;
        err.Message = "нет языка скриптов для этого файла";
        Report(err, owner.Name());
        ReportConsequence("script.not-started", sage::Severity::Error, path, owner.Name(),
                          "Объект «" + owner.Name() + "» остался без скрипта " + path,
                          "Для расширения этого файла нет языка — объект работает без него.");
        return false;
    }
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
    return true;
}

bool ScriptRuntime::Attach(GameObject owner, const std::string& path,
                           const sage::vars::Table& fields) {
    if (!owner.Valid() || path.empty()) return false;
    LanguageBackend* backend = nullptr;
    ScriptSource src;
    if (!Open(owner, path, backend, src)) return false;
    Detach(owner.Entity()); // прежний смысл: этот скрипт — ВМЕСТО всех
    return Spawn(*backend, src, owner, fields, -1) != nullptr;
}

LiveScript* ScriptRuntime::Add(GameObject owner, const std::string& path, const sage::vars::Table& fields,
                               int slot) {
    if (!owner.Valid() || path.empty()) return nullptr;
    LanguageBackend* backend = nullptr;
    ScriptSource src;
    if (!Open(owner, path, backend, src)) return nullptr;
    return Spawn(*backend, src, owner, fields, slot);
}

LiveScript* ScriptRuntime::Spawn(LanguageBackend& backend, const ScriptSource& src, GameObject owner,
                                 const sage::vars::Table& fields, int slot) {
    const sage::vars::Table merged = MergeFields(backend, src, fields);
    const InstanceId id = Build(backend, src, owner, merged);
    if (id == kInvalidInstance) return nullptr;

    LiveScript s;
    s.Owner = owner;
    s.Backend = &backend;
    s.Instance = id;
    s.Path = src.Path;
    s.Slot = slot;
    s.Stamp = src.Stamp;
    s.Fields = merged;
    m_scripts.push_back(std::move(s));
    // Индекс дописывается сразу, без полной пересборки: добавление в конец
    // номеров прежних записей не меняет.
    if (!m_dirtyIndex) m_index[(uint32_t)entt::to_integral(owner.Entity())].push_back(m_scripts.size() - 1);
    LiveScript& live = m_scripts.back();

    // Start зовётся СРАЗУ: скрипт, созданный посреди кадра (Instantiate), не
    // должен ждать следующего кадра, чтобы себя настроить. Кроме запуска
    // сцены целиком (BeginBatch): там Start ждёт, пока подключатся ВСЕ, —
    // иначе Start первого скрипта, обратившегося к соседу
    // (`Scene.find("Player"):call(...)`), не находил бы у соседа скрипта.
    if (m_batch > 0) {
        m_pendingStart.push_back({owner.Entity(), &backend, id});
        return &live;
    }
    // Скрипт, снявший свой объект прямо в Start, подключён честно: ответ —
    // запись (Dead её отличит), а не «не загрузился».
    StartScript(live);
    return &live;
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
    // По ОДНОМУ и заново ищем: Start вправе создать объект со скриптом или
    // снять соседа.
    std::vector<PendingStart> pending;
    pending.swap(m_pendingStart);
    for (const PendingStart& p : pending)
        for (LiveScript* s : FindAll(p.Entity))
            if (s->Backend == p.Backend && s->Instance == p.Instance) {
                StartScript(*s);
                break;
            }
}

void ScriptRuntime::Kill(LiveScript& s) {
    if (s.Dead) return;
    // Помечается ДО хуков: OnDestroy, позвавший снятие своего же объекта,
    // не должен войти сюда второй раз.
    s.Dead = true;
    m_dirtyIndex = true;
    const InstanceId id = s.Instance;
    if (id == kInvalidInstance) return;
    ScriptError err;
    if (s.Enabled && s.Started && s.Backend->Has(id, Hook::OnDisable))
        s.Backend->Call(id, Hook::OnDisable, 0.0f, err);
    if (s.Backend->Has(id, Hook::OnDestroy)) s.Backend->Call(id, Hook::OnDestroy, 0.0f, err);
    s.Backend->Destroy(id);
    s.Instance = kInvalidInstance;
}

void ScriptRuntime::Detach(entt::entity entity) {
    for (LiveScript* s : FindAll(entity)) Kill(*s);
}

void ScriptRuntime::DetachAll() {
    // По индексу: OnDestroy вправе создать объект со скриптом — он тоже
    // должен быть снят, а не брошен живым в языке.
    for (size_t i = 0; i < m_scripts.size(); ++i) Kill(m_scripts[i]);
    m_scripts.clear();
    m_index.clear();
    m_dirtyIndex = false;
    m_pendingStart.clear();
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
    for (LiveScript* s : FindAll(entity)) {
        if (s->Dead || s->Muted || !s->Enabled || !s->Backend->Has(s->Instance, hook)) continue;
        ScriptError err;
        const bool ok = s->Backend->CallWith(s->Instance, hook, other, err);
        Guard(*s, ok, err, hook);
    }
}

void ScriptRuntime::DispatchNamed(entt::entity entity, Hook hook, const std::string& name) {
    for (LiveScript* s : FindAll(entity)) {
        if (s->Dead || s->Muted || !s->Enabled || !s->Backend->Has(s->Instance, hook)) continue;
        ScriptError err;
        const bool ok = s->Backend->CallNamed(s->Instance, hook, name, err);
        Guard(*s, ok, err, hook);
    }
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
    std::vector<sage::vars::Value> ignored;
    return Invoke(entity, method, args, ignored);
}

bool ScriptRuntime::Invoke(entt::entity entity, const std::string& method,
                           const std::vector<sage::vars::Value>& args, std::vector<sage::vars::Value>& results) {
    LiveScript* s = FindMethod(entity, method);
    if (!s) return false;
    ScriptError err;
    const bool ok = s->Backend->Invoke(s->Instance, method, args, results, err);
    if (!ok && !err.Empty()) {
        if (err.File.empty()) err.File = s->Path;
        Report(err);
    }
    return ok;
}

int ScriptRuntime::QueueTests(entt::entity entity) {
    int n = 0;
    for (LiveScript* s : FindAll(entity))
        if (s->Instance != kInvalidInstance) n += s->Backend->QueueTests(s->Instance, s->Path);
    return n;
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
    LiveScript* s = FindMethod(entity, method);
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
    for (LiveScript* s : FindAll(entity)) {
        if (s->Enabled == enabled) continue;
        s->Enabled = enabled;
        const Hook hook = enabled ? Hook::OnEnable : Hook::OnDisable;
        // До Start края нет: OnEnable придёт из самого StartScript.
        if (!s->Started || !s->Backend->Has(s->Instance, hook)) continue;
        ScriptError err;
        const bool ok = s->Backend->Call(s->Instance, hook, 0.0f, err);
        Guard(*s, ok, err, hook);
    }
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

// Новый экземпляр собирается РЯДОМ со старым и заменяет его только если
// собрался: скрипт с опечаткой не убивает рабочий. Значения полей — те же,
// что у объекта, плюс умолчания переменных, появившихся в файле.
bool ScriptRuntime::Rebuild(LiveScript& s, const ScriptSource& src) {
    // Копии — до Build: файл при выполнении вправе породить объект со
    // скриптом, и всё, что читается из s после, читается заново.
    LanguageBackend* backend = s.Backend;
    const GameObject owner = s.Owner;
    sage::vars::Table fields = MergeFields(*backend, src, s.Fields);
    const InstanceId fresh = Build(*backend, src, owner, fields);
    if (fresh == kInvalidInstance) return false; // прежний экземпляр остаётся жить
    const InstanceId old = s.Instance;
    s.Fields = std::move(fields);
    s.Instance = fresh;
    s.Errors = 0;
    s.Muted = false;
    s.Started = true;
    ScriptError err;
    if (backend->Has(old, Hook::OnDestroy)) backend->Call(old, Hook::OnDestroy, 0.0f, err);
    backend->Destroy(old);
    if (backend->Has(fresh, Hook::Start)) {
        ScriptError startErr;
        const bool ok = backend->Call(fresh, Hook::Start, 0.0f, startErr);
        Guard(s, ok, startErr, Hook::Start);
    }
    return true;
}

int ScriptRuntime::ReloadChanged() {
    int reloaded = 0;
    // По индексу: пересборка выполняет файл и Start, а они вправе породить
    // объект со скриптом.
    for (size_t i = 0; i < m_scripts.size(); ++i) {
        LiveScript& s = m_scripts[i];
        if (s.Dead || s.Stamp == 0) continue; // из пакета — перезагружать нечего
        ScriptSource src;
        if (!ReadSource(s.Path, src)) continue;
        if (src.Stamp == s.Stamp || src.Stamp == 0) continue;
        // Штамп запоминаем СРАЗУ, даже если скрипт не собрался: иначе одна и та
        // же опечатка писалась бы в консоль шестьдесят раз в секунду.
        s.Stamp = src.Stamp;
        if (Rebuild(s, src)) ++reloaded;
    }
    if (reloaded > 0) LOG_INFO("Script") << "перечитано скриптов: " << reloaded;
    return reloaded;
}

int ScriptRuntime::Reload(const std::string& path) {
    int reloaded = 0;
    for (size_t i = 0; i < m_scripts.size(); ++i) {
        LiveScript& s = m_scripts[i];
        if (s.Dead || s.Path != path) continue;
        ScriptSource src;
        if (!ReadSource(s.Path, src)) continue;
        s.Stamp = src.Stamp;
        if (Rebuild(s, src)) ++reloaded;
    }
    return reloaded;
}

} // namespace sage::scripting
