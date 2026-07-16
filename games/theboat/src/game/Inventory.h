#pragma once
#include "Items.h"
#include <array>

// Инвентарь: просто счётчик каждого типа предмета (без слотов и стаков —
// для альфы этого достаточно) + хотбар из 9 фиксированных слотов, из
// которых игрок выбирает "что в руке".
class Inventory {
public:
    static constexpr int HotbarSize = 9;

    Inventory() {
        // Фиксированная раскладка хотбара альфы: сначала блоки, потом
        // инструменты и расходники. Пустые слоты показываются серыми.
        m_hotbar = {
            ItemType::PlankBlock,
            ItemType::WoodBlock,
            ItemType::SailBlock,
            ItemType::StoveBlock,
            ItemType::BarrelBlock,
            ItemType::FishingRod,
            ItemType::RawFish,
            ItemType::CookedFish,
            ItemType::FreshWater,
        };
    }

    int Count(ItemType type) const { return m_counts[(size_t)type]; }
    bool Has(ItemType type, int amount = 1) const { return Count(type) >= amount; }

    void Add(ItemType type, int amount = 1) { m_counts[(size_t)type] += amount; }

    bool Remove(ItemType type, int amount = 1) {
        if (!Has(type, amount)) return false;
        m_counts[(size_t)type] -= amount;
        return true;
    }

    ItemType HotbarItem(int slot) const {
        return (slot >= 0 && slot < HotbarSize) ? m_hotbar[slot] : ItemType::None;
    }

    int SelectedSlot() const { return m_selectedSlot; }
    void SelectSlot(int slot) {
        if (slot >= 0 && slot < HotbarSize) m_selectedSlot = slot;
    }
    void ScrollSlot(int delta) {
        m_selectedSlot = ((m_selectedSlot + delta) % HotbarSize + HotbarSize) % HotbarSize;
    }

    ItemType SelectedItem() const { return m_hotbar[m_selectedSlot]; }

private:
    std::array<int, (size_t)ItemType::Count> m_counts{}; // zero-initialized
    std::array<ItemType, HotbarSize> m_hotbar{};
    int m_selectedSlot = 0;
};
