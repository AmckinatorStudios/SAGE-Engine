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
    test_hierarchy.cpp
    test_scripting.cpp
    test_robustness.cpp
    test_config.cpp
    test_paths.cpp
    test_ui.cpp
    test_ui_core.cpp
    test_vars_events.cpp
    test_framegraph.cpp
    test_lod.cpp
    test_resources.cpp
    test_jobsystem.cpp
    test_net.cpp
    test_builtinphysics.cpp
    test_character.cpp
    test_physics.cpp
    test_tween.cpp
    test_decals.cpp
    test_gi.cpp
    test_picking.cpp
    test_assetformat.cpp
    test_import.cpp
    test_modelmaterial.cpp
    test_submesh.cpp
    test_texformat.cpp
    test_texturegen.cpp
    test_rhi.cpp
    test_pack.cpp
    test_components.cpp
    test_assetcache.cpp
    test_lighting.cpp
    test_scripting_lifecycle.cpp
    test_fbx.cpp
    test_bench.cpp
)

target_include_directories(sage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sage_tests PRIVATE sage::engine nlohmann_json::nlohmann_json)

add_test(NAME sage_tests COMMAND sage_tests)

# ============================================================================
#  crash_probe — программа, которая падает нарочно.
#
#  Обработчик падений нельзя проверить обычным тестом: он по замыслу доводит
#  падение до конца и убивает процесс. Поэтому падение вынесено в отдельный
#  исполняемый файл, а smoke-тест запускает его и проверяет отчёт.
# ============================================================================
add_executable(sage_crash_probe crash_probe.cpp)
target_link_libraries(sage_crash_probe PRIVATE sage::engine)

# ============================================================================
#  sage_render_tests — проверка КАРТИНКИ по эталонным кадрам. Требует GL, поэтому
#  собирается по флагу и запускается в CI под xvfb (см. tests/render).
# ============================================================================
option(SAGE_BUILD_RENDER_TESTS "Собрать тесты эталонных кадров (нужен OpenGL)" ON)
if (SAGE_BUILD_RENDER_TESTS)
    add_subdirectory(render)
endif()

# ============================================================================
#  sage_bench — сколько стоят горячие места движка.
#
#  Отдельная программа, а не тест, и это принципиально: тест обязан отвечать
#  «работает/нет», а замер печатает число. Замер, оформленный тестом, начинает
#  падать от загрузки машины сборщика — и ему перестают верить ровно тогда,
#  когда он впервые скажет правду.
# ============================================================================
add_executable(sage_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/../bench/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../bench/Cases.cpp
)
target_include_directories(sage_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../bench)
target_link_libraries(sage_bench PRIVATE sage::engine nlohmann_json::nlohmann_json)
