#pragma once
#include <glm/glm.hpp>
#include <cstdint>

// Тип блока. uint8_t — чтобы чанк 16x16x16 занимал мало памяти (4096 байт на чанк).
enum class BlockType : uint8_t {
    Air = 0,   // пустота — блок не рендерится и не считается твёрдым
    Grass,
    Dirt,
    Stone,
    Wood,   // бревно/мачта
    Plank,  // доски — основной строительный блок корабля
    Sail,   // парус (белая ткань)
    Stove,  // печка — на ней жарится рыба
    Barrel, // бочка — декор/хранилище пресной воды
    Count // служебное значение — количество типов
};

// Раньше цвет блока брался отсюда напрямую. Теперь реальная детализация
// приходит из текстурного атласа (см. GetBlockAtlasIndex), а это просто
// нейтральный тонирующий множитель (белый = без изменений). Оставлено на
// случай, если позже понадобится тонирование (например, цвет травы по биому).
inline glm::vec3 GetBlockColor(BlockType type) {
    return glm::vec3(1.0f);
}

// Сетка тайлов текстурного атласа assets/textures/blocks_atlas.png. Это
// свойство конкретного файла атласа (сколько тайлов в нём нарезано), а не
// движка вокселей — вынесено сюда, рядом с индексами тайлов, а не оставлено
// локальной константой в коде построения меша (см. Chunk.cpp), чтобы при
// перенарезке атласа менять эти два числа нужно было только в одном месте.
constexpr int kBlockAtlasCols = 4;
constexpr int kBlockAtlasRows = 3;

// Индекс тайла блока в текстурном атласе assets/textures/blocks_atlas.png
// (сетка kBlockAtlasCols x kBlockAtlasRows). Один и тот же тайл используется
// на всех 6 гранях блока — разные текстуры для верха/боков/низа (как трава в
// Minecraft) можно добавить позже, расширив эту функцию до
// GetBlockAtlasIndex(type, faceNormal).
inline int GetBlockAtlasIndex(BlockType type) {
    switch (type) {
        case BlockType::Grass:  return 0;
        case BlockType::Dirt:   return 1;
        case BlockType::Stone:  return 2;
        case BlockType::Wood:   return 3;
        case BlockType::Plank:  return 4;
        case BlockType::Sail:   return 5;
        case BlockType::Stove:  return 6;
        case BlockType::Barrel: return 7;
        default: return 2; // нет такого блока — используем камень как безопасный запасной вариант
    }
}

// Отображаемое имя блока (ASCII — для встроенного debug/UI-шрифта движка,
// который не поддерживает кириллицу)
inline const char* GetBlockName(BlockType type) {
    switch (type) {
        case BlockType::Air:    return "Air";
        case BlockType::Grass:  return "Grass";
        case BlockType::Dirt:   return "Dirt";
        case BlockType::Stone:  return "Stone";
        case BlockType::Wood:   return "Wood";
        case BlockType::Plank:  return "Plank";
        case BlockType::Sail:   return "Sail";
        case BlockType::Stove:  return "Stove";
        case BlockType::Barrel: return "Barrel";
        default: return "?";
    }
}

inline bool IsSolid(BlockType type) {
    return type != BlockType::Air;
}
