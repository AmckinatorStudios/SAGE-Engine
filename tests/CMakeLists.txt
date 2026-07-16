# ---------------------------------------------------------------------
# sage_engine_tests — юнит-тесты ядра движка на doctest (однозаголовочный,
# минимальный build-overhead — тот же FetchContent-паттерн, что уже
# используется для sol2/json выше в корневом CMakeLists.txt). Тестируются
# только части ядра, не требующие GL-контекста/окна (EventBus, будущие
# физика/сцена и т.п.) — GL-зависимые вещи по-прежнему проверяются
# headless-скриншот-тестами через xvfb (см. scripts/build_linux.sh).
# ---------------------------------------------------------------------
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

set(SAGE_TEST_SOURCES
    test_main.cpp
    core/EventBusTests.cpp
)

add_executable(sage_engine_tests ${SAGE_TEST_SOURCES})
target_link_libraries(sage_engine_tests PRIVATE sage_engine doctest::doctest)
target_compile_options(sage_engine_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(sage_engine_tests)
