# TestGame — боевая тестовая игра SAGE: стресс-тест подсистем движка на
# реальном игровом цикле (см. src/TestGameLayer.h и README «games/testgame»).
sage_add_game(
    NAME TestGame
    SOURCES
        src/main.cpp
        src/TestGameLayer.cpp
        src/GameIcons.cpp
        src/GameComponents.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
# nlohmann приватен у движка, а описание компонента для реестра
# (GameComponents.cpp) пишет JSON — игра подключает его сама.
target_link_libraries(TestGame PRIVATE nlohmann_json::nlohmann_json)
