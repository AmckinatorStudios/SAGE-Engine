# ============================================================================
#  Sandbox — минимальный showcase движка SAGE поверх sage::engine (не игра с
#  геймплеем). Референс «как сделать игру» + единственная запускаемая fixture
#  для headless smoke-тестов CI (см. README, раздел "games/sandbox").
# ============================================================================
sage_add_game(
    NAME Sandbox
    SOURCES
        src/main.cpp
        src/SandboxLayer.cpp
    ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
