# SagePlayer — универсальный рантайм игр, собранных в редакторе (см.
# src/PlayerLayer.h). Структурно это «игра» на движке (sage_add_game), только
# контент приходит не из кода, а из проекта редактора (project.sageproj).
sage_add_game(
    NAME SagePlayer
    SOURCES
        src/main.cpp
        src/PlayerLayer.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
# Чтение project.sageproj (имя игры) — nlohmann приватен у движка.
target_link_libraries(SagePlayer PRIVATE nlohmann_json::nlohmann_json)
