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
    main.cpp
    RenderTestHarness.cpp
)

target_include_directories(sage_render_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SAGE_ENGINE_ROOT}/external/stb
)
target_link_libraries(sage_render_tests PRIVATE sage::engine glfw)

# Эталоны лежат рядом с исходниками и коммитятся: их diff в ревью — это и есть
# «картинка изменилась вот так».
target_compile_definitions(sage_render_tests PRIVATE
    SAGE_RENDER_REFERENCES="${CMAKE_CURRENT_SOURCE_DIR}/references")

add_test(NAME sage_render_tests
         COMMAND sage_render_tests --references ${CMAKE_CURRENT_SOURCE_DIR}/references)
