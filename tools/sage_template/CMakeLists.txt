# Упаковщик шаблонов — маленькая программа для сборки релиза и для скриптов
# (см. main.cpp).
#
# НЕ ЛИНКУЕТ sage::engine, и это существенно. Движок тянет за собой GLFW, а тот
# при сборке требует заголовки X11 — на машине с окнами они есть, на сборочном
# раннере релиза нет, и настройка падала на «Could NOT find X11» ещё до первой
# строки кода. Упаковщику окно не нужно вовсе: ему нужен формат пакета и
# сжатие, то есть пять файлов ниже.
#
# Заодно это делает инструмент пригодным на голом сервере без графики —
# ровно там, где шаблоны и собирают.
add_executable(sage_template
    main.cpp
    ${SAGE_ENGINE_ROOT}/engine/src/sage/assets/Pack.cpp
    ${SAGE_ENGINE_ROOT}/engine/src/sage/assets/format/Blob.cpp
    ${SAGE_ENGINE_ROOT}/engine/src/sage/core/Log.cpp
    ${SAGE_ENGINE_ROOT}/engine/src/sage/core/CrashHandler.cpp
    ${SAGE_ENGINE_ROOT}/engine/src/sage/core/Paths.cpp
)
target_include_directories(sage_template PRIVATE
    ${SAGE_ENGINE_ROOT}/engine/src
    ${CMAKE_BINARY_DIR}/generated)          # Version.h
target_link_libraries(sage_template PRIVATE miniz nlohmann_json::nlohmann_json)
if (WIN32)
    # dbghelp — обработчик падений внутри Log/CrashHandler (тот же набор, что и
    # у движка); sage_windows_manifest даёт UTF-8 путям в аргументах.
    target_link_libraries(sage_template PRIVATE dbghelp)
    sage_windows_manifest(sage_template)
endif()
