#include "sage/jobs/JobSystemDetail.h"

#include <algorithm>
#include <cstdio>

#include "sage/core/EngineContext.h"
#include "sage/core/Log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

// Запуск и остановка системы задач, потоки и их циклы.
namespace sage::jobs {

const char* LaneName(Lane lane) {
    switch (lane) {
        case Lane::Worker: return "Worker";
        case Lane::Background: return "Background";
        case Lane::IO: return "IO";
        case Lane::Main: return "Main";
        case Lane::Render: return "Render";
        default: return "?";
    }
}

const char* PriorityName(Priority p) {
    switch (p) {
        case Priority::High: return "High";
        case Priority::Normal: return "Normal";
        case Priority::Low: return "Low";
        default: return "?";
    }
}

const char* StatusName(JobStatus s) {
    switch (s) {
        case JobStatus::Waiting: return "Waiting";
        case JobStatus::Queued: return "Queued";
        case JobStatus::Running: return "Running";
        case JobStatus::Succeeded: return "Succeeded";
        case JobStatus::Failed: return "Failed";
        case JobStatus::Canceled: return "Canceled";
    }
    return "?";
}

const char* CategoryName(Category c) {
    switch (c) {
        case Category::General: return "General";
        case Category::ECS: return "ECS";
        case Category::Render: return "Render";
        case Category::Physics: return "Physics";
        case Category::Animation: return "Animation";
        case Category::Audio: return "Audio";
        case Category::Scripts: return "Scripts";
        case Category::Resources: return "Resources";
        case Category::AssetImport: return "AssetImport";
        case Category::IO: return "IO";
        case Category::AI: return "AI";
        case Category::Editor: return "Editor";
        case Category::Network: return "Network";
        case Category::Game: return "Game";
        default: return "?";
    }
}

namespace detail {

void SetCurrentThreadName(const char* name) {
#if defined(_WIN32)
    // SetThreadDescription есть с Windows 10 1607 и не во всех заголовках
    // mingw — берём из kernel32 по имени; на старой системе просто без имени.
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (!fn) return;
    wchar_t wide[64];
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    if (n > 0) fn(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    char buf[16];  // предел Linux — 15 символов и ноль
    std::snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#endif
}

int AvailableCores() {
    int n = (int)std::thread::hardware_concurrency();
#if defined(_WIN32)
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
        int bits = 0;
        for (DWORD_PTR m = proc; m; m &= m - 1) ++bits;
        if (bits > 0 && (n <= 0 || bits < n)) n = bits;
    }
#elif !defined(__APPLE__)
    // Контейнер и taskset ограничивают ядра маской сродства, а
    // hardware_concurrency видит все ядра машины: 64 воркера на 2 выделенных
    // ядрах — это только переключения контекста.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int c = CPU_COUNT(&set);
        if (c > 0 && (n <= 0 || c < n)) n = c;
    }
#endif
    return std::max(1, n);
}

Core::Core(const JobSystemConfig& cfg) : Cfg(cfg) {
    Queues[(int)Lane::Worker] = std::make_unique<LaneQueue>("jobs.queue.worker");
    Queues[(int)Lane::Background] = std::make_unique<LaneQueue>("jobs.queue.background");
    Queues[(int)Lane::IO] = std::make_unique<LaneQueue>("jobs.queue.io");
    Queues[(int)Lane::Main] = std::make_unique<LaneQueue>("jobs.queue.main");
    Queues[(int)Lane::Render] = std::make_unique<LaneQueue>("jobs.queue.render");
    StartNs = NowNs();
    MainSlot.Name = "Main";
    MainSlot.Kind = Lane::Main;
    MainSlot.Index = -1;
}

Core::~Core() { Stop(); }

void Core::Start() {
    const int cores = AvailableCores();
    const int workers = Cfg.WorkerThreads > 0 ? Cfg.WorkerThreads : std::max(0, cores - 1);
    // Долгим задачам — не больше половины воркеров: кадр всегда найдёт ядро.
    // На 1–2 воркерах делить нечего — долгое идёт на потоки ввода-вывода.
    BackgroundOnIo = workers < 3;
    BackgroundLimit = Cfg.BackgroundLimit > 0 ? std::min(Cfg.BackgroundLimit, std::max(1, workers))
                                              : std::max(1, workers / 2);
    const int io = Cfg.IoThreads > 0 ? Cfg.IoThreads : (cores <= 1 ? 1 : 2);
    MaxIo = std::max(io, Cfg.MaxIoThreads > 0 ? Cfg.MaxIoThreads : 8);
    if (BackgroundOnIo) BackgroundLimit = MaxIo;

    MainThread = std::this_thread::get_id();
    RenderThread = MainThread;

    Workers.reserve((size_t)workers);
    for (int i = 0; i < workers; ++i) {
        auto slot = std::make_unique<ThreadSlot>();
        slot->Kind = Lane::Worker;
        slot->Index = i;
        slot->Name = "SAGE Worker " + std::to_string(i);
        slot->Rng = 0x9E3779B9u ^ (uint32_t)(i * 2654435761u);
        Workers.push_back(std::move(slot));
    }
    // Потоки стартуют после того, как вектор воркеров заполнен: вор читает
    // его без замка, и рост вектора под ногами у вора был бы гонкой.
    for (auto& w : Workers) {
        ThreadSlot* s = w.get();
        s->Thread = std::thread([this, s] { WorkerLoop(s); });
    }
    {
        std::lock_guard<std::mutex> lk(IoM);
        Io.reserve((size_t)MaxIo);  // без перераспределения: Stats читает под IoM
        for (int i = 0; i < io; ++i) SpawnIo();
    }
    LOG_INFO("Jobs") << "Система задач: " << workers << " воркеров (+главный поток), " << io
                     << " потоков ввода-вывода (до " << MaxIo << "), долгим задачам — до "
                     << BackgroundLimit << (BackgroundOnIo ? " на потоках ввода-вывода" : " воркеров")
                     << "; доступно ядер: " << cores;
}

void Core::SpawnIo() {
    auto slot = std::make_unique<ThreadSlot>();
    slot->Kind = Lane::IO;
    slot->Index = (int)(Workers.size() + Io.size());
    slot->Name = "SAGE IO " + std::to_string(Io.size());
    ThreadSlot* s = slot.get();
    Io.push_back(std::move(slot));
    IoCount.fetch_add(1, std::memory_order_acq_rel);
    s->Thread = std::thread([this, s] { IoLoop(s); });
}

void Core::MaybeGrowIo() {
    // Все потоки ввода-вывода заняты (ждут диск, процесс, другую задачу), а
    // работа есть — добавить поток. Не больше MaxIo: рост ограничен, потоки
    // не плодятся бесконтрольно; лишние не убиваются — их немного.
    //
    // Сравниваем ОЧЕРЕДЬ со спящими, а не «есть ли спящие»: разбуженный поток
    // ещё числится спящим, пока не проснулся, и четыре задачи подряд все
    // «доставались» бы одному потоку.
    auto backlog = [this] {
        int n = Queues[(int)Lane::IO]->Size.load(std::memory_order_acquire);
        if (BackgroundOnIo) n += Queues[(int)Lane::Background]->Size.load(std::memory_order_acquire);
        if (Workers.empty()) n += Queues[(int)Lane::Worker]->Size.load(std::memory_order_acquire);
        return n;
    };
    if (IoCount.load(std::memory_order_acquire) >= MaxIo) return;
    if (backlog() <= IoIdle.load(std::memory_order_acquire)) return;
    if (Stopping.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(IoM);
    if (Stopping.load(std::memory_order_acquire)) return;
    if ((int)Io.size() >= MaxIo || backlog() <= IoIdle.load(std::memory_order_acquire)) return;
    SpawnIo();
}

void Core::WorkerLoop(ThreadSlot* slot) {
    SetCurrentThreadName(slot->Name.c_str());
    ThreadCtx& ctx = Ctx();
    ctx.Owner = this;
    ctx.Slot = slot;
    for (;;) {
        bool stolen = false;
        JobRecord* j = FindWorkerJob(slot, stolen);
        if (j) {
            Execute(j, slot, stolen);
            continue;
        }
        if (Stopping.load(std::memory_order_acquire)) break;
        // Короткое кручение до сна: работа часто приходит через микросекунды
        // (следующий кусок кадра), а сон и пробуждение стоят десятки.
        Spinning.fetch_add(1, std::memory_order_seq_cst);
        for (int i = 0; i < 64 && !j; ++i) {
            sync::CpuRelax();
            if ((i & 15) == 15) j = FindWorkerJob(slot, stolen);
        }
        Spinning.fetch_sub(1, std::memory_order_seq_cst);
        if (j) {
            // Нашёл, перестав быть «ищущим», — работа, похоже, идёт потоком:
            // разбудить соседа, чтобы следующую задачу не ждали.
            WakeWorker();
            Execute(j, slot, stolen);
            continue;
        }
        const uint64_t epoch = WorkerParker.Prepare();
        j = FindWorkerJob(slot, stolen);
        if (j || Stopping.load(std::memory_order_acquire)) {
            WorkerParker.Cancel();
            if (j) Execute(j, slot, stolen);
            else break;
            continue;
        }
        slot->Parks.fetch_add(1, std::memory_order_relaxed);
        WorkerParker.Commit(epoch, std::chrono::milliseconds(100));
    }
    ctx = ThreadCtx{};
}

void Core::IoLoop(ThreadSlot* slot) {
    SetCurrentThreadName(slot->Name.c_str());
    ThreadCtx& ctx = Ctx();
    ctx.Owner = this;
    ctx.Slot = slot;
    for (;;) {
        if (JobRecord* j = FindIoJob(slot)) {
            Execute(j, slot, false);
            continue;
        }
        if (Stopping.load(std::memory_order_acquire)) break;
        IoIdle.fetch_add(1, std::memory_order_acq_rel);
        const uint64_t epoch = IoParker.Prepare();
        JobRecord* j = FindIoJob(slot);
        if (j || Stopping.load(std::memory_order_acquire)) {
            IoParker.Cancel();
            IoIdle.fetch_sub(1, std::memory_order_acq_rel);
            if (j) Execute(j, slot, false);
            else break;
            continue;
        }
        slot->Parks.fetch_add(1, std::memory_order_relaxed);
        IoParker.Commit(epoch, std::chrono::milliseconds(200));
        IoIdle.fetch_sub(1, std::memory_order_acq_rel);
    }
    ctx = ThreadCtx{};
}

void Core::Stop() {
    if (Stopping.exchange(true, std::memory_order_acq_rel)) {
        // Повторный вызов: потоки уже присоединены первым.
        return;
    }
    auto cancelAll = [this] {
        // Ждущие зависимостей — отменить (зависимость может не наступить
        // никогда: обещание, которое никто не выполнит).
        std::vector<JobRecord*> waiting;
        {
            std::lock_guard<std::mutex> lk(WaitingM);
            waiting.assign(WaitingSet.begin(), WaitingSet.end());
            for (JobRecord* r : waiting) r->AddRef();
        }
        for (JobRecord* r : waiting) {
            uint8_t expected = (uint8_t)JobStatus::Waiting;
            if (r->Status.compare_exchange_strong(expected, (uint8_t)JobStatus::Queued,
                                                  std::memory_order_acq_rel)) {
                {
                    std::lock_guard<std::mutex> lk(WaitingM);
                    WaitingSet.erase(r);
                    r->InWaitingSet.store(false, std::memory_order_release);
                }
                FinishCascade(r, JobStatus::Canceled, nullptr);
            }
            r->Release();
        }
        for (auto& q : Queues)
            for (JobRecord* r : q->DrainAll()) FinishCascade(r, JobStatus::Canceled, nullptr);
        for (auto& w : Workers)
            while (JobRecord* r = w->Deque.Steal()) FinishCascade(r, JobStatus::Canceled, nullptr);
    };
    cancelAll();
    WorkerParker.NotifyAll();
    IoParker.NotifyAll();
    for (auto& w : Workers)
        if (w->Thread.joinable()) w->Thread.join();
    {
        std::lock_guard<std::mutex> lk(IoM);
        for (auto& s : Io)
            if (s->Thread.joinable()) s->Thread.join();
    }
    // Задачи, завершившиеся во время присоединения, могли поставить
    // последователей — Enqueue их уже отклонил, но и дек мог получить новые.
    cancelAll();
}

Core* CoreForSubmit() { return JobSystem::Get().CoreForSubmit(); }

} // namespace detail
} // namespace sage::jobs

namespace sage {

using namespace jobs;
using jobs::detail::Core;

JobSystem& JobSystem::Get() { return EngineContext::Current().Jobs(); }

JobSystem* JobSystem::TryGet() {
    EngineContext* ctx = EngineContext::CurrentOrNull();
    return ctx ? &ctx->Jobs() : nullptr;
}

namespace {
// Исполнитель для sage::jobs::ParallelFor (ParallelFor.h): система задач
// текущего контекста, без контекста — последовательно.
void ParallelForViaContext(std::size_t count, const std::function<void(std::size_t, std::size_t)>& body,
                           const ParallelOptions& options) {
    if (JobSystem* js = JobSystem::TryGet()) js->ParallelFor(count, body, options);
    else body(0, count);
}
int ParallelismViaContext() {
    JobSystem* js = JobSystem::TryGet();
    return js ? js->ThreadCount() : 1;
}
} // namespace

JobSystem::JobSystem() { jobs::SetParallelBackend(&ParallelForViaContext, &ParallelismViaContext); }

JobSystem::~JobSystem() { Shutdown(); }

namespace {
std::mutex& InitMutex() {
    static std::mutex m;
    return m;
}
} // namespace

void JobSystem::Initialize(int workerThreads) {
    JobSystemConfig cfg = m_config;
    cfg.WorkerThreads = std::max(0, workerThreads);
    Initialize(cfg);
}

void JobSystem::Initialize(const JobSystemConfig& config) {
    std::lock_guard<std::mutex> lk(InitMutex());
    const bool same = m_core && m_config.WorkerThreads == config.WorkerThreads &&
                      m_config.IoThreads == config.IoThreads &&
                      m_config.MaxIoThreads == config.MaxIoThreads &&
                      m_config.BackgroundLimit == config.BackgroundLimit;
    m_config = config;
    m_enabled = config.Enabled;
    if (same) {
        m_core->Cfg.HungJobMs = config.HungJobMs;
        m_core->Cfg.HungBackgroundMs = config.HungBackgroundMs;
        return;  // уже так — ничего
    }
    if (m_core) {
        m_core->Stop();
        m_core.reset();
    }
    m_wasShutDown = false;
    auto core = std::make_unique<Core>(config);
    core->Start();
    m_core = std::move(core);
}

void JobSystem::Shutdown() {
    std::lock_guard<std::mutex> lk(InitMutex());
    if (!m_core) return;
    m_core->Stop();
    m_core.reset();
    m_wasShutDown = true;
}

bool JobSystem::IsRunning() const { return m_core != nullptr; }

void JobSystem::SetEnabled(bool enabled) {
    m_enabled = enabled;
    m_config.Enabled = enabled;
}

int JobSystem::WorkerCount() const { return m_core ? (int)m_core->Workers.size() : 0; }

int JobSystem::IoThreadCount() const { return m_core ? m_core->IoCount.load() : 0; }

Core* JobSystem::CoreOrNull() const { return m_core.get(); }

Core* JobSystem::CoreForSubmit() {
    if (Core* c = m_core.get()) return c;
    if (m_wasShutDown) return nullptr;
    // Ленивый подъём: утилите или тесту без Application не нужно помнить про
    // Initialize. Вызвавший поток становится главным.
    Initialize(m_config);
    return m_core.get();
}

bool JobSystem::IsMainThread() const { return m_core && m_core->IsMainThread(); }
bool JobSystem::IsRenderThread() const { return m_core && m_core->IsRenderThread(); }
bool JobSystem::IsWorkerThread() const {
    const auto& ctx = jobs::detail::Ctx();
    return m_core && ctx.Owner == m_core.get() && ctx.Slot && ctx.Slot->Kind == Lane::Worker;
}

void JobSystem::SetRenderThreadToCurrent() {
    if (Core* c = CoreForSubmit()) c->RenderThread = std::this_thread::get_id();
}

bool JobSystem::CheckRenderThread(const char* what) const {
    if (!m_core || m_core->IsRenderThread()) return true;
    // Первые нарушения — в лог с именем вызова; дальше только счётчик, чтобы
    // ошибка в цикле не залила лог.
    if (m_core->RenderViolations.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG_ERROR("Jobs") << "Вызов графического контекста не из потока рендера: " << (what ? what : "?");
    return false;
}

bool JobSystem::HelpOnce() {
    Core* c = m_core.get();
    return c && c->HelpOnce();
}

int JobSystem::PumpMainThread(double budgetMs) {
    Core* c = m_core.get();
    if (!c) return 0;
    if (!c->IsMainThread()) {
        LOG_ERROR("Jobs") << "PumpMainThread вызван не из главного потока — пропущено";
        return 0;
    }
    return c->PumpLane(Lane::Main, budgetMs, 0);
}

int JobSystem::PumpRenderThread(double budgetMs) {
    Core* c = m_core.get();
    if (!c) return 0;
    if (!c->IsRenderThread()) {
        LOG_ERROR("Jobs") << "PumpRenderThread вызван не из потока рендера — пропущено";
        return 0;
    }
    return c->PumpLane(Lane::Render, budgetMs, 0);
}

JobHandle JobSystem::WhenAll(std::vector<JobHandle> deps, JobOptions opts) {
    for (JobHandle& h : deps) opts.After.push_back(std::move(h));
    if (opts.Name == nullptr || std::string(opts.Name) == "job") opts.Name = "when_all";
    return Async([] {}, std::move(opts));
}

} // namespace sage
