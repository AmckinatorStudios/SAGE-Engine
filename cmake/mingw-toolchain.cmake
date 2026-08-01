set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Статически линкуем рантайм MinGW и libstdc++/libgcc, чтобы .exe запускался
# на "чистой" Windows без установки дополнительных DLL.
#
# НЕ распространяется на MODULE (editor/plugins/*.dll): плагин линкуется на
# import-библиотеку SageEditor.exe, чтобы резолвить символы ImGui:: (см.
# sage_add_editor_plugin в cmake/SageHelpers.cmake), а тот .exe уже несёт
# статический рантайм и экспортирует его символы. Если ЕЩЁ и плагин попросит
# статический рантайм, компоновщик получает ДВЕ копии одних и тех же символов
# (`_Unwind_Resume` и т.п.) — и падает на "multiple definition". Поэтому плагин
# остаётся на динамическом рантайме, а нужную DLL (libstdc++-6.dll) кладёт
# рядом шаг упаковки релиза (.github/workflows/windows.yml), а не сборка.
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
