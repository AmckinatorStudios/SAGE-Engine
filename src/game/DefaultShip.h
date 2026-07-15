#pragma once
#include "../voxel/World.h"
#include <glm/glm.hpp>
#include <vector>

// Стартовый корабль The Boat — "модель", собранная вручную из тех же блоков,
// которые доступны игроку в игре. Никакой магии: всё, что здесь поставлено,
// игрок может сломать, забрать в инвентарь и перестроить по-своему.
//
// Компоновка (вид сверху, нос на -Z):
//
//            ~~ нос ~~
//              /||\
//             / || \        мачта (Wood) + парус (Sail)
//   борта -> |  ||  | <-
//            |[стов]|       печка и бочка на палубе
//            | каюта|       каюта у кормы
//            |______|
//             корма
//
// origin — угол корабля (min x/z), deckY — уровень палубы.
struct ShipInfo {
    glm::ivec3 Origin;
    int DeckY;
    glm::vec3 Center;       // центр палубы (для спавна игрока и систем)
    glm::ivec3 StovePos;    // где стоит печка
    std::vector<glm::vec3> LanternPositions;
};

inline ShipInfo BuildDefaultShip(World& world, glm::ivec3 origin) {
    const int W = 9;   // ширина
    const int L = 17;  // длина (нос занимает последние ~4 блока)
    const int hullY = origin.y;      // днище
    const int deckY = origin.y + 2;  // палуба (по ней ходим)

    auto set = [&](int x, int y, int z, BlockType t) {
        world.SetBlock(origin.x + x, y, origin.z + z, t);
    };

    // Ширина корпуса в ряду z: основная часть прямоугольная, к носу сужается
    auto rowHalfWidth = [&](int z) -> int {
        if (z >= 4) return W / 2;                  // основная часть: полная ширина
        return W / 2 - (4 - z);                    // сужение к носу: 4,3,2,1 ряды
    };
    int cx = W / 2; // центральная колонка

    // --- корпус: днище (2 слоя бортов ниже палубы) и сама палуба ---
    for (int z = 0; z < L; ++z) {
        int hw = rowHalfWidth(z);
        if (hw < 0) continue;
        for (int x = cx - hw; x <= cx + hw; ++x) {
            set(x, hullY, z, BlockType::Wood);       // днище — брёвна
            set(x, hullY + 1, z, BlockType::Plank);  // подпалубный слой
            set(x, deckY, z, BlockType::Plank);      // палуба
        }
    }

    // --- фальшборт (ограждение) по периметру палубы, высотой 1 ---
    for (int z = 0; z < L; ++z) {
        int hw = rowHalfWidth(z);
        if (hw < 0) continue;
        set(cx - hw, deckY + 1, z, BlockType::Plank);
        set(cx + hw, deckY + 1, z, BlockType::Plank);
    }
    // корма — закрыть торец
    for (int x = 0; x < W; ++x) set(x, deckY + 1, L - 1, BlockType::Plank);
    // нос — остриё
    set(cx, deckY + 1, 0, BlockType::Plank);

    // --- мачта (Wood) в центре + прямой парус (Sail) ---
    int mastZ = 7;
    for (int y = deckY + 1; y <= deckY + 6; ++y) set(cx, y, mastZ, BlockType::Wood);
    for (int x = cx - 2; x <= cx + 2; ++x) {
        for (int y = deckY + 3; y <= deckY + 5; ++y) {
            if (x == cx) continue; // мачта проходит сквозь парус
            set(x, y, mastZ, BlockType::Sail);
        }
    }

    // --- каюта у кормы (стены Plank, крыша Plank), вход со стороны носа ---
    int cabinZ0 = L - 5, cabinZ1 = L - 2;
    for (int z = cabinZ0; z <= cabinZ1; ++z) {
        for (int x = cx - 2; x <= cx + 2; ++x) {
            bool wall = (z == cabinZ0 || z == cabinZ1 || x == cx - 2 || x == cx + 2);
            // дверной проём по центру передней стены (2 блока высотой)
            bool door = (z == cabinZ0 && x == cx);
            if (wall && !door) {
                set(x, deckY + 1, z, BlockType::Plank);
                set(x, deckY + 2, z, BlockType::Plank);
            }
            set(x, deckY + 3, z, BlockType::Plank); // крыша
        }
    }

    // --- печка и бочка на палубе перед каютой ---
    glm::ivec3 stove = origin + glm::ivec3(cx - 2, deckY + 1, cabinZ0 - 2);
    world.SetBlock(stove.x, stove.y, stove.z, BlockType::Stove);
    world.SetBlock(origin.x + cx + 2, deckY + 1, origin.z + cabinZ0 - 2, BlockType::Barrel);

    ShipInfo info;
    info.Origin = origin;
    info.DeckY = deckY;
    info.Center = glm::vec3(origin.x + cx, deckY + 1, origin.z + L / 2);
    info.StovePos = stove;
    // Фонари: на носу и на крыше каюты
    info.LanternPositions = {
        glm::vec3(origin.x + cx, deckY + 2.5f, origin.z + 1),
        glm::vec3(origin.x + cx, deckY + 4.5f, origin.z + cabinZ0 + 1.5f),
    };
    return info;
}
