#include "sage/render/TextureStreamer.h"
#include "sage/assets/Pack.h"
#include "sage/render/AlphaBleed.h"
#include "sage/core/ImageDecodeBudget.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <cmath>
#include <stb_image.h>

namespace sage::render {
namespace {
size_t Workspace(const TextureStreamer::Limits& l) {
    // Packed stored + expanded input, capped stb scratch, output + bleed scratch.
    return l.InputBytes * 2 + l.DecoderBytes + l.DecodedBytes * 4;
}
void Validate(const TextureStreamer::Limits& l) {
    constexpr size_t cap = size_t(1) << 30;
    if (!l.InputBytes || l.InputBytes > cap || !l.DecodedBytes || l.DecodedBytes > cap ||
        !l.DecoderBytes || l.DecoderBytes > cap || !l.Requests || l.Requests > 4096 ||
        !l.PathBytes || l.PathBytes > 65536 || l.Dimension < 1 || l.Dimension > 32768 ||
        l.InputBytes > SIZE_MAX / 8 || l.DecoderBytes > SIZE_MAX / 8 ||
        l.DecodedBytes > SIZE_MAX / 8 || l.CpuBytes < Workspace(l))
        throw std::invalid_argument("TextureStreamer: invalid/below-workspace limits");
}
bool Decode(const std::string& path, const TextureStreamer::Limits& limits,
            std::vector<unsigned char>& rgba, int& width, int& height) {
    rgba.clear(); width = height = 0;
    std::vector<uint8_t> input;
    if (!assets::vfs::ReadFile(path, input, limits.InputBytes) || input.empty() || input.size() > INT_MAX)
        return false;
    image_memory::Scope memory(limits.DecoderBytes);
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(input.data(), int(input.size()), &w, &h, &channels) ||
        w < 1 || h < 1 || w > limits.Dimension || h > limits.Dimension ||
        size_t(w) > limits.DecodedBytes / 4 / size_t(h)) return false;
    const size_t bytes = size_t(w) * size_t(h) * 4;
    stbi_set_flip_vertically_on_load_thread(true);
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(input.data(), int(input.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!pixels || width != w || height != h) { width = height = 0; return false; }
    rgba.assign(pixels.get(), pixels.get() + bytes);
    return true;
}
} // namespace

struct TextureStreamer::Impl {
    enum class State { Free, Queued, Running, Ready, Consuming };
    struct Slot {
        State Status = State::Free;
        uint64_t Sequence = 0;
        std::string File;
        bool Bleed = false;
        Decoded Result;
    };
    Limits Config;
    std::vector<Slot> Slots; // preallocated: publishing failure cannot allocate
    std::mutex Mutex;
    std::condition_variable Cv;
    std::thread Worker;
    bool Stopping = false;
    uint64_t Sequence = 0;
    Stats Counters;
    explicit Impl(const Limits& l) : Config(l), Slots(l.Requests) {}
    Slot* First(State state) {
        Slot* first = nullptr;
        for (auto& s : Slots)
            if (s.Status == state && (!first || s.Sequence < first->Sequence)) first = &s;
        return first;
    }
    void Run() noexcept {
        std::unique_lock<std::mutex> lock(Mutex);
        for (;;) {
            Cv.wait(lock, [&] {
                return Stopping || (First(State::Queued) &&
                    Workspace(Config) <= Config.CpuBytes - Counters.CpuBytes);
            });
            if (Stopping) return;
            Slot& slot = *First(State::Queued);
            slot.Status = State::Running;
            Counters.CpuBytes += Workspace(Config);
            Counters.PeakCpuBytes = std::max(Counters.PeakCpuBytes, Counters.CpuBytes);
            lock.unlock();
            try {
                auto& d = slot.Result;
                d.Ok = Decode(slot.File, Config, d.Pixels, d.Width, d.Height);
                if (d.Ok && slot.Bleed) BleedTransparentColor(d.Pixels, d.Width, d.Height);
            } catch (...) {
                // Includes bad_alloc/non-standard exceptions. No allocation or
                // logging on this path: the existing slot becomes a failure.
                slot.Result.Ok = false;
            }
            if (!slot.Result.Ok) {
                std::vector<unsigned char>().swap(slot.Result.Pixels);
                slot.Result.Width = slot.Result.Height = 0;
            }
            lock.lock();
            Counters.CpuBytes -= Workspace(Config);
            Counters.CpuBytes += slot.Result.Pixels.capacity();
            slot.Status = State::Ready;
            ++Counters.Ready;
        }
    }
};
TextureStreamer::TextureStreamer() : TextureStreamer(Limits{}) {}
TextureStreamer::TextureStreamer(const Limits& limits) : m_limits(limits) { Validate(limits); }
TextureStreamer::~TextureStreamer() { Stop(); }
void TextureStreamer::Configure(const Limits& limits) {
    Validate(limits);
    if (Pending()) throw std::logic_error("TextureStreamer: configure with pending work");
    Stop(); m_limits = limits;
}
bool TextureStreamer::Enqueue(const std::string& key, const std::string& file, bool bleed) {
    if (key.size() > m_limits.PathBytes || file.size() > m_limits.PathBytes) return false;
    if (!m_impl) {
        auto impl = std::make_unique<Impl>(m_limits);
        impl->Worker = std::thread([p = impl.get()] { p->Run(); });
        m_impl = std::move(impl);
    }
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    auto* slot = m_impl->First(Impl::State::Free);
    if (!slot) return false;
    // Copy bounded strings instead of retaining oversized caller capacity.
    slot->Result.Key = key; slot->File = file; slot->Bleed = bleed;
    slot->Sequence = ++m_impl->Sequence;
    slot->Status = Impl::State::Queued;
    ++m_impl->Counters.Pending;
    m_impl->Cv.notify_one();
    return true;
}
size_t TextureStreamer::PumpReady(const PumpBudget& budget,
                                 const std::function<void(const Decoded&)>& consume) {
    if (m_pumping) throw std::logic_error("TextureStreamer: recursive pump");
    if (!std::isfinite(budget.Milliseconds) || budget.Milliseconds < 0)
        throw std::invalid_argument("TextureStreamer: invalid pump time");
    if (!m_impl || !budget.Results || !budget.Bytes || !budget.Milliseconds) return 0;
    m_pumping = true;
    struct Guard { bool& Flag; ~Guard() { Flag = false; } } guard{m_pumping};
    const auto start = std::chrono::steady_clock::now();
    size_t count = 0, bytes = 0;
    while (count < budget.Results &&
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
               < budget.Milliseconds) {
        Impl::Slot* slot;
        {
            std::lock_guard<std::mutex> lock(m_impl->Mutex);
            slot = m_impl->First(Impl::State::Ready);
            if (!slot || slot->Result.Pixels.size() > budget.Bytes - bytes) break;
            slot->Status = Impl::State::Consuming;
            --m_impl->Counters.Ready;
        }
        const size_t size = slot->Result.Pixels.size();
        const size_t allocation = slot->Result.Pixels.capacity();
        auto release = [&] {
            std::vector<unsigned char>().swap(slot->Result.Pixels);
            slot->Result.Ok = false;
            std::lock_guard<std::mutex> lock(m_impl->Mutex);
            m_impl->Counters.CpuBytes -= allocation;
            --m_impl->Counters.Pending;
            slot->Status = Impl::State::Free;
            m_impl->Cv.notify_one();
        };
        try { consume(slot->Result); } catch (...) { release(); throw; }
        release(); bytes += size; ++count;
    }
    return count;
}
TextureStreamer::Stats TextureStreamer::GetStats() const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lock(m_impl->Mutex);
    return m_impl->Counters;
}
int TextureStreamer::Pending() const { return int(GetStats().Pending); }
void TextureStreamer::Stop() {
    if (m_pumping) throw std::logic_error("TextureStreamer: stop during pump");
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lock(m_impl->Mutex);
        m_impl->Stopping = true;
    }
    m_impl->Cv.notify_all();
    if (m_impl->Worker.joinable()) m_impl->Worker.join();
    m_impl.reset();
}
bool TextureStreamer::DecodeImageFile(const std::string& path, std::vector<unsigned char>& rgba,
                                      int& width, int& height) {
    // Compatibility synchronous path retains support for larger old assets.
    auto limits = Limits{};
    limits.InputBytes = INT_MAX; limits.DecodedBytes = SIZE_MAX;
    limits.DecoderBytes = SIZE_MAX; limits.Dimension = INT_MAX;
    return Decode(path, limits, rgba, width, height);
}

bool TextureStreamer::DecodeImageFile(const std::string& path, const Limits& limits,
                                      std::vector<unsigned char>& rgba, int& width, int& height) {
    return Decode(path, limits, rgba, width, height);
}

bool TextureStreamer::ProbeImageFile(const std::string& path, size_t inputLimit, int& width, int& height) {
    std::vector<uint8_t> input;
    int channels = 0;
    image_memory::Scope memory(Limits{}.DecoderBytes);
    return assets::vfs::ReadFile(path, input, inputLimit) && !input.empty() && input.size() <= INT_MAX &&
           stbi_info_from_memory(input.data(), int(input.size()), &width, &height, &channels) &&
           width > 0 && height > 0;
}
} // namespace sage::render
