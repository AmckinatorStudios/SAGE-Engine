#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sage::render {

// Политика и учёт текстурной памяти. Класс не знает ни GL, ни Texture: это
// позволяет проверять LRU-решения без графического устройства.
class VRAMBudgetManager {
public:
    struct EvictCandidate {
        size_t Index = 0;
        size_t Bytes = 0;
        uint64_t Tick = 0;
        bool Evictable = false;
    };

    struct DowngradeCandidate {
        size_t Index = 0;
        size_t Bytes = 0;
        uint64_t Tick = 0;
        int Side = 0;
        bool Streamable = false;
    };

    void SetBudgetBytes(size_t bytes) { m_budget = bytes; }
    size_t BudgetBytes() const { return m_budget; }
    size_t ResidentBytes() const { return m_resident; }
    size_t& BudgetRef() { return m_budget; }
    size_t& ResidentRef() { return m_resident; }
    uint64_t& TickRef() { return m_tick; }
    size_t& EvictionsRef() { return m_evictions; }
    size_t& DowngradesRef() { return m_downgrades; }
    uint64_t NextTick() { return ++m_tick; }
    void AddBytes(size_t bytes) { m_resident += bytes; }
    void RemoveBytes(size_t bytes) { m_resident -= bytes > m_resident ? m_resident : bytes; }
    void ResetResidentBytes() { m_resident = 0; }
    void NoteEviction() { ++m_evictions; }
    void NoteDowngrade() { ++m_downgrades; }
    size_t Evictions() const { return m_evictions; }
    size_t Downgrades() const { return m_downgrades; }

    static std::vector<size_t> SelectEvictions(const std::vector<EvictCandidate>& candidates,
                                               size_t currentBytes, size_t budget);
    static std::vector<size_t> SelectDowngrades(const std::vector<DowngradeCandidate>& candidates,
                                                size_t currentBytes, size_t budget);
    static void DownscaleRGBA(const std::vector<unsigned char>& src, int w, int h,
                              std::vector<unsigned char>& dst, int& outW, int& outH);

private:
    size_t m_budget = 0;
    size_t m_resident = 0;
    uint64_t m_tick = 0;
    size_t m_evictions = 0;
    size_t m_downgrades = 0;
};

} // namespace sage::render
