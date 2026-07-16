# ============================================================================
#  The Boat — игра поверх движка SAGE (пример «игры на движке»). Линкует
#  библиотеку sage::engine и добавляет только СВОЙ код: точку входа, игровую
#  логику (src/game/*) и воксельный модуль (src/voxel/*) — воксели здесь это
#  специфика КОНКРЕТНОЙ игры (корабль из блоков), а не часть движка. Другая
#  игра на SAGE просто не подключала бы voxel вовсе.
# ============================================================================
sage_add_game(
    NAME TheBoat
    SOURCES
        src/main.cpp
        src/TheBoatLayer.cpp
        src/game/PlayerController.cpp
        src/voxel/Chunk.cpp
        src/voxel/VoxelMesh.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
