#include "physics/jolt/JoltJobs.h"

#include <chrono>
#include <thread>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include "sage/core/EngineContext.h"
#include "sage/jobs/JobSystem.h"

namespace sage::physics {
namespace {

class EngineJobSystem final : public JPH::JobSystemWithBarrier {
public:
    EngineJobSystem(unsigned maxJobs, unsigned maxBarriers) {
        JobSystemWithBarrier::Init(maxBarriers);
        m_jobs.Init(maxJobs, maxJobs);
    }

    int GetMaxConcurrency() const override {
        sage::JobSystem* js = Engine();
        return js ? js->ThreadCount() : 1;
    }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn,
                        JPH::uint32 numDependencies) override {
        JPH::uint32 index;
        for (;;) {
            index = m_jobs.ConstructObject(name, color, this, fn, numDependencies);
            if (index != Jobs::cInvalidObjectIndex) break;
            // Свободных записей нет — их освобождают идущие задачи: подождать.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        Job* job = &m_jobs.Get(index);
        JobHandle handle(job);  // ссылка до постановки: задача может завершиться сразу
        if (numDependencies == 0) QueueJob(job);
        return handle;
    }

    void QueueJob(Job* job) override {
        sage::JobSystem* js = Engine();
        // Без системы задач (или после её остановки) — не ставим: барьер
        // выполнит задачу сам, ожидая её (см. JobSystemWithBarrier::Wait).
        if (!js || !js->IsRunning()) return;
        // Ссылка Jolt живёт в замыкании: выполнилась задача или была отклонена
        // при остановке — Release случится ровно один раз, с разрушением
        // замыкания.
        JPH::Ref<Job> ref(job);
        sage::jobs::JobOptions o("jolt", sage::jobs::Category::Physics, sage::jobs::Lane::Worker,
                                 sage::jobs::Priority::High);
        js->Run([ref]() { ref->Execute(); }, o);
    }

    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
    }

    void FreeJob(Job* job) override { m_jobs.DestructObject(job); }

private:
    static sage::JobSystem* Engine() {
        sage::EngineContext* ctx = sage::EngineContext::CurrentOrNull();
        return ctx ? &ctx->Jobs() : nullptr;
    }

    using Jobs = JPH::FixedSizeFreeList<Job>;
    Jobs m_jobs;
};

} // namespace

std::unique_ptr<JPH::JobSystem> CreateJoltJobSystem(unsigned maxJobs, unsigned maxBarriers) {
    return std::make_unique<EngineJobSystem>(maxJobs, maxBarriers);
}

} // namespace sage::physics
