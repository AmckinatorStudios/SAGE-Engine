#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>

// stb uses these allocators for every image. Decoders can install a per-thread
// budget, whose allocations must be freed before the scope goes away.
namespace sage::image_memory {
struct Budget { size_t Limit, Used = 0; };
inline thread_local Budget* Current = nullptr;
struct Scope {
    Budget Local;
    Budget* Previous;
    explicit Scope(size_t limit) : Local{limit}, Previous(Current) { Current = &Local; }
    ~Scope() { Current = Previous; }
};
struct alignas(std::max_align_t) Header { size_t Size; Budget* Owner; };
inline void* Allocate(size_t bytes) {
    if (bytes > std::numeric_limits<size_t>::max() - sizeof(Header) ||
        (Current && bytes + sizeof(Header) > Current->Limit - Current->Used)) return nullptr;
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + bytes));
    if (!h) return nullptr;
    h->Size = bytes; h->Owner = Current;
    if (Current) Current->Used += bytes + sizeof(Header);
    return h + 1;
}
inline void Free(void* ptr) {
    if (!ptr) return;
    auto* h = static_cast<Header*>(ptr) - 1;
    if (h->Owner) h->Owner->Used -= h->Size + sizeof(Header);
    std::free(h);
}
inline void* Reallocate(void* ptr, size_t bytes) {
    if (!ptr) return Allocate(bytes);
    if (!bytes) { Free(ptr); return nullptr; }
    auto* h = static_cast<Header*>(ptr) - 1;
    void* next = Allocate(bytes);
    if (!next) return nullptr;
    std::memcpy(next, ptr, std::min(h->Size, bytes));
    Free(ptr);
    return next;
}
} // namespace sage::image_memory
