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
    test_zip.cpp
    test_animation.cpp
    test_hierarchy.cpp
    test_scripting.cpp
    test_robustness.cpp
    test_config.cpp
    test_paths.cpp
    test_ui.cpp
    test_vars_events.cpp
    test_input.cpp
    test_eventbus.cpp
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
    test_glb.cpp
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
    test_scripting_system.cpp
    test_project_startscene.cpp
    test_fbx.cpp
    test_audio.cpp
    test_clipfile.cpp
    test_components_mesh_animation.cpp
    test_sky.cpp
    test_bench.cpp
    test_posteffect.cpp
    test_editorstate.cpp
    test_pathscope.cpp
    test_hotkeyscope.cpp
    test_copyname.cpp
    test_nineslice.cpp
    test_propertyclip.cpp
    test_interface.cpp
    test_savegame.cpp
    test_colorpicker.cpp
    ../editor/src/PathScope.cpp
    ../editor/src/HotkeyScope.cpp
    ../editor/src/EditorSelection.cpp
    ../editor/src/EditorHistory.cpp
    ../editor/src/Project.cpp
    # Выбор цвета и то, на чём он стоит (подписи, отступы меню, тема).
    ../editor/src/ui/ColorPicker.cpp
    ../editor/src/ui/UI.cpp
    ../editor/src/ui/UIStyle.cpp
    ../editor/src/EditorTheme.cpp
    ../editor/src/EditorIcons.cpp
    ../editor/src/EditorPrefs.cpp
    ../editor/src/Localization.cpp
)

target_include_directories(sage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                              ${CMAKE_CURRENT_SOURCE_DIR}/../editor/src)
# tinygltf — ради проверки разреженных аксессоров: она сверяет то, что отдаёт
# общий читатель (assets/import/GltfAccessor.h), а он работает с tinygltf::Model.
target_link_libraries(sage_tests PRIVATE sage::engine nlohmann_json::nlohmann_json tinygltf imgui imguizmo)

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
#  exit_probe — программа, которая просто ЗАВЕРШАЕТСЯ (и обязана завершиться
#  чисто).
#
#  Падение при выходе случается ПОСЛЕ main, когда разрушаются статические
#  объекты движка: обычным тестом его не поймать — прогон к тому моменту уже
#  закончен и отчёт напечатан. Поэтому проверка вынесена в отдельный файл, а
#  smoke-тест смотрит КОД ВОЗВРАТА процесса (см. tests/exit_probe.cpp).
# ============================================================================
add_executable(sage_exit_probe exit_probe.cpp)
target_link_libraries(sage_exit_probe PRIVATE sage::engine)

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
