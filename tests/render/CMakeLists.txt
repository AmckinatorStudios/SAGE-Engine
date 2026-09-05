# ============================================================================
#  sage_render_tests — проверка КАРТИНКИ по эталонным кадрам.
#
#  Отдельная цель, а не часть sage_tests: здесь нужен графический контекст, а
#  обычные тесты движка сознательно работают без него (и потому гоняются везде).
#  В CI запускается под xvfb; локально — только если явно попросили, потому что
#  на другой видеокарте эталоны могут разойтись в пределах точности драйвера
#  (подробности — в RenderTestHarness.h).
# ============================================================================
add_executable(sage_render_tests
    main.cpp                 # только порядок прогона
    Fixture.cpp              # общая оснастка: сцена, камера, проход, учёт
    Checks_Frame.cpp         # кадр и пост-обработка
    Checks_Animation.cpp     # морфинг и обратная кинематика
    Checks_Reflections.cpp   # отражения и блик
    Checks_Shadows.cpp       # тени
    Checks_Scene.cpp         # детализация, отсечение, кэш, небо, RHI
    Checks_Materials.cpp     # материал: от файла до пикселей
    Checks_UI.cpp            # интерфейс: части не наезжают и не красят друг друга
    Checks_UICore.cpp        # новая система интерфейса: эталонные сцены (§125)
    Checks_Volumetrics.cpp   # объёмный свет: зерно, полосы, кайма
    RenderTestHarness.cpp    # сравнение картинок и запись эталонов
)

target_include_directories(sage_render_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SAGE_ENGINE_ROOT}/external/stb
)
target_link_libraries(sage_render_tests PRIVATE sage::engine glfw)

# Эталоны лежат рядом с исходниками и коммитятся: их diff в ревью — это и есть
# «картинка изменилась вот так».
target_compile_definitions(sage_render_tests PRIVATE
    SAGE_RENDER_REFERENCES="${CMAKE_CURRENT_SOURCE_DIR}/references"
    # Тестовая модель: сгенерирована tools/make_test_model.py и закоммичена.
    # Путь абсолютный, потому что тест запускают из разных каталогов.
    SAGE_TEST_MODEL="${SAGE_ENGINE_ROOT}/engine/assets/test_model.glb")

add_test(NAME sage_render_tests
         COMMAND sage_render_tests --references ${CMAKE_CURRENT_SOURCE_DIR}/references)
