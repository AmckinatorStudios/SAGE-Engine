# Voxel World — блочная песочница на своём рендерере SAGE (см. src/VoxelPipeline.h):
# свой граф кадра, свои шейдеры, свет по блокам, вычислительный шейдер.
sage_add_game(
    NAME VoxelWorld
    SOURCES
        src/main.cpp
        src/VoxelLayer.cpp
        src/VoxelPipeline.cpp
        src/World.cpp
        src/ChunkMesher.cpp
        src/BlockAtlas.cpp
        src/Player.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
