#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Блоки мира. Номер блока — байт в массиве чанка; всё, что рендеру и свету
// нужно знать о блоке, — в таблице ниже, а не в ветках по номеру.
// ---------------------------------------------------------------------------
namespace voxel {

enum Block : uint8_t {
    Air = 0,
    Turf,      // дёрн: трава сверху, земля по бокам
    Soil,      // земля
    Granite,   // камень
    Dune,      // песок
    Water,
    Trunk,     // ствол
    Foliage,   // листва (вырез по альфе)
    Boards,    // доски
    Lumen,     // светокамень — источник света
    Frost,     // снег
    Clay,      // кирпич
    Crystal,   // стекло
    Basalt,    // тёмный камень глубин
    Moss,      // мшистый камень
    Ember,     // тлеющий блок — тёплый слабый свет
    BlockCount
};

// Как блок рисуется — от этого зависит проход и сортировка.
enum class DrawKind : uint8_t {
    None,    // воздух
    Opaque,  // обычный блок
    Cutout,  // вырез по альфе (листва): непрозрачный проход, discard в шейдере
    Glass,   // прозрачный, но с глубиной (стекло)
    Liquid,  // вода: смешивание, без записи глубины
};

struct BlockInfo {
    const char* Name;     // для интерфейса
    DrawKind Draw;
    uint8_t Emission;     // свет блока 0..15
    uint8_t LightCost;    // сколько теряет свет при проходе сквозь (0 — глухой)
    // Номера плиток атласа: верх, низ, бока.
    uint8_t TileTop, TileBottom, TileSide;
    bool Solid;           // твёрдый для игрока
};

const BlockInfo& Info(uint8_t block);

// Проходит ли свет (воздух, вода, листва, стекло).
inline bool Transparent(uint8_t b) { return Info(b).LightCost > 0; }

// Плитки атласа (16×16 пикселей, сетка 8×8).
enum Tile : uint8_t {
    TileTurfTop = 0, TileTurfSide, TileSoil, TileGranite, TileDune, TileWater,
    TileTrunkSide, TileTrunkTop, TileFoliage, TileBoards, TileLumen, TileFrost,
    TileFrostSide, TileClay, TileCrystal, TileBasalt, TileMoss, TileEmber,
    TileCount
};

constexpr int kAtlasGrid = 8;   // плиток по стороне
constexpr int kTilePixels = 16; // пикселей у плитки

} // namespace voxel
