#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstddef>

namespace sage::render {
// Owner-thread API. Stop cancels backlog and joins only the active bounded
// decode. Restart is allowed. Consumers must not reenter or retain results.
class TextureStreamer {
public:
    struct Limits {
        size_t CpuBytes = 256u << 20;
        size_t InputBytes = 16u << 20;
        size_t DecodedBytes = 16u << 20;
        size_t DecoderBytes = 64u << 20;
        size_t Requests = 64;
        size_t PathBytes = 4096;
        int Dimension = 8192;
    };
    struct PumpBudget {
        size_t Bytes = 16u << 20;
        size_t Results = 8; // failures also count
        double Milliseconds = 2.0; // soft: individual upload is not preemptible
    };
    struct Stats {
        size_t Pending = 0, Ready = 0, CpuBytes = 0, PeakCpuBytes = 0;
    };
    struct Decoded {
        std::string Key;
        std::vector<unsigned char> Pixels;
        int Width = 0, Height = 0;
        bool Ok = false;
    };
    TextureStreamer();
    explicit TextureStreamer(const Limits& limits);
    ~TextureStreamer();
    TextureStreamer(const TextureStreamer&) = delete;
    TextureStreamer& operator=(const TextureStreamer&) = delete;
    // False means backpressure/invalid metadata: retry later.
    bool Enqueue(const std::string& key, const std::string& file, bool bleed);
    // Memory remains charged through consumer execution. Consumer exceptions
    // release that result, then propagate; later work survives.
    size_t PumpReady(const PumpBudget& budget, const std::function<void(const Decoded&)>& consume);
    Stats GetStats() const;
    const Limits& GetLimits() const { return m_limits; }
    // Only with no outstanding requests; invalid configurations throw.
    void Configure(const Limits& limits);
    int Pending() const;
    void Stop();
    static bool DecodeImageFile(const std::string& path, std::vector<unsigned char>& rgba,
                                int& width, int& height);
    static bool DecodeImageFile(const std::string& path, const Limits& limits,
                                std::vector<unsigned char>& rgba, int& width, int& height);
    static bool ProbeImageFile(const std::string& path, size_t inputLimit, int& width, int& height);
private:
    struct Impl;
    Limits m_limits;
    bool m_pumping = false;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sage::render
