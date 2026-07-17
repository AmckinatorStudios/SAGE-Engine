# TestGame — боевая тестовая игра SAGE: стресс-тест подсистем движка на
# реальном игровом цикле (см. src/TestGameLayer.h и README «games/testgame»).
sage_add_game(
    NAME TestGame
    SOURCES
        src/main.cpp
        src/TestGameLayer.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
