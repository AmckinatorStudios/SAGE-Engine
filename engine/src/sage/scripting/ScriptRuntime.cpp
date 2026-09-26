#include "sage/scripting/ScriptRuntime.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "sage/assets/Pack.h"
#include "sage/core/Log.h"

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
        if (!fs::exists(candidate, ec)) continue;
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
    return backend->ParseFields(src.Text);
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
        Report(err);
        return kInvalidInstance;
    }
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
    merged.MergeDeclaration(backend.ParseFields(src.Text));
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
        err.File = path;
        err.Message = "файл скрипта не найден";
        Report(err);
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
    // должен ждать следующего кадра, чтобы себя настроить.
    LiveScript& live = m_scripts.back();
    if (backend->Has(id, Hook::Start)) {
        ScriptError err;
        Guard(live, backend->Call(id, Hook::Start, 0.0f, err), err, Hook::Start);
    }
    live.Started = true;
    if (live.Enabled && backend->Has(id, Hook::OnEnable)) {
        ScriptError err;
        Guard(live, backend->Call(id, Hook::OnEnable, 0.0f, err), err, Hook::OnEnable);
    }
    return true;
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

void ScriptRuntime::Report(const ScriptError& err) {
    if (m_sink) {
        m_sink(err);
        return;
    }
    LOG_ERROR("Script") << err.Format();
    if (!err.Traceback.empty()) LOG_ERROR("Script") << err.Traceback;
}

void ScriptRuntime::Guard(LiveScript& s, bool ok, ScriptError& err, Hook hook) {
    if (ok) return;
    if (s.Muted) return;
    if (err.File.empty()) err.File = s.Path;
    if (err.Message.empty()) err.Message = std::string("ошибка в ") + HookName(hook);
    Report(err);
    if (++s.Errors >= kMaxErrors) {
        s.Muted = true;
        LOG_WARN("Script") << s.Path << ": " << HookName(hook)
                           << " падает каждый кадр — дальнейшие сообщения подавлены";
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
        if (!s.Owner.Valid()) { s.Dead = true; m_dirtyIndex = true; continue; }
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
