# ============================================================================
#  sage_tests — модульные тесты движка (математика/сериализация/анимация).
#  Быстрые, БЕЗ GL-контекста (в отличие от headless smoke-тестов в
#  scripts/ci_smoke_test.sh). Линкует sage::engine, регистрируется в CTest.
#  Собирается всегда; запуск — `ctest` или напрямую `./sage_tests`.
# ============================================================================
add_executable(sage_tests
    main.cpp
    test_math.cpp
    test_serialization.cpp
    test_animation.cpp
)

target_include_directories(sage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sage_tests PRIVATE sage::engine nlohmann_json::nlohmann_json)

add_test(NAME sage_tests COMMAND sage_tests)
