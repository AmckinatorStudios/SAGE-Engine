#include "sage/jobs/ParallelFor.h"

#include <atomic>

namespace sage::jobs {

namespace {
std::atomic<ParallelForBackend> g_for{nullptr};
std::atomic<ParallelismBackend> g_threads{nullptr};
} // namespace

void SetParallelBackend(ParallelForBackend forBackend, ParallelismBackend threadsBackend) {
    g_for.store(forBackend, std::memory_order_release);
    g_threads.store(threadsBackend, std::memory_order_release);
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t, std::size_t)>& body,
                 const ParallelOptions& options) {
    if (count == 0 || options.Token.IsCancelled()) return;
    if (ParallelForBackend fn = g_for.load(std::memory_order_acquire)) {
        fn(count, body, options);
        return;
    }
    body(0, count);
}

int Parallelism() {
    ParallelismBackend fn = g_threads.load(std::memory_order_acquire);
    return fn ? fn() : 1;
}

} // namespace sage::jobs
