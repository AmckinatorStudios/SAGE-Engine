#pragma once
#include "../voxel/Block.h"
#include <glm/glm.hpp>

// Все предметы игры The Boat. Предмет — это то, что лежит в инвентаре:
// блоки (можно ставить), ресурсы из мусора (сырьё для крафта),
// инструменты и еда/питьё.
enum class ItemType {
    None = 0,

    // --- блоки (ставятся в мир) ---
    PlankBlock,
    WoodBlock,
    SailBlock,
    StoveBlock,
    BarrelBlock,

    // --- ресурсы (вылавливаются из океана как мусор) ---
    ScrapWood,   // обломки дерева
    ScrapPlastic,
    ScrapMetal,
    Cloth,       // обрывки ткани
    Rope,        // верёвка

    // --- инструменты ---
    FishingRod,

    // --- еда и питьё ---
    RawFish,
    CookedFish,
    FreshWater,  // бутылка пресной воды (результат крафта фильтра)

    Count
};

inline const char* GetItemName(ItemType type) {
    switch (type) {
        case ItemType::PlankBlock:  return "Plank";
        case ItemType::WoodBlock:   return "Wood";
        case ItemType::SailBlock:   return "Sail";
        case ItemType::StoveBlock:  return "Stove";
        case ItemType::BarrelBlock: return "Barrel";
        case ItemType::ScrapWood:   return "Scrap Wood";
        case ItemType::ScrapPlastic:return "Plastic";
        case ItemType::ScrapMetal:  return "Metal";
        case ItemType::Cloth:       return "Cloth";
        case ItemType::Rope:        return "Rope";
        case ItemType::FishingRod:  return "Fishing Rod";
        case ItemType::RawFish:     return "Raw Fish";
        case ItemType::CookedFish:  return "Cooked Fish";
        case ItemType::FreshWater:  return "Fresh Water";
        default: return "-";
    }
}

// Если предмет — блок, возвращает какой BlockType он ставит; иначе Air
inline BlockType GetItemBlock(ItemType type) {
    switch (type) {
        case ItemType::PlankBlock:  return BlockType::Plank;
        case ItemType::WoodBlock:   return BlockType::Wood;
        case ItemType::SailBlock:   return BlockType::Sail;
        case ItemType::StoveBlock:  return BlockType::Stove;
        case ItemType::BarrelBlock: return BlockType::Barrel;
        default: return BlockType::Air;
    }
}

// Обратный маппинг: сломанный блок превращается в предмет-блок
inline ItemType GetBlockItem(BlockType type) {
    switch (type) {
        case BlockType::Plank:  return ItemType::PlankBlock;
        case BlockType::Wood:   return ItemType::WoodBlock;
        case BlockType::Sail:   return ItemType::SailBlock;
        case BlockType::Stove:  return ItemType::StoveBlock;
        case BlockType::Barrel: return ItemType::BarrelBlock;
        // Земные блоки в The Boat не встречаются, но если игрок каким-то
        // образом их сломает — превращаем в обломки дерева, чтобы ничего
        // не пропадало бесследно.
        case BlockType::Grass:
        case BlockType::Dirt:
        case BlockType::Stone: return ItemType::ScrapWood;
        default: return ItemType::None;
    }
}

// Цвет кубика мусора, плавающего в воде (визуал до подбора)
inline glm::vec3 GetItemFloatColor(ItemType type) {
    switch (type) {
        case ItemType::ScrapWood:    return {0.55f, 0.40f, 0.22f};
        case ItemType::ScrapPlastic: return {0.90f, 0.85f, 0.30f};
        case ItemType::ScrapMetal:   return {0.60f, 0.63f, 0.68f};
        case ItemType::Cloth:        return {0.85f, 0.82f, 0.75f};
        case ItemType::Rope:         return {0.72f, 0.60f, 0.38f};
        default: return {1.0f, 0.0f, 1.0f};
    }
}
