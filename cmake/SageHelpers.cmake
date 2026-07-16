# ============================================================================
#  SageHelpers.cmake — вспомогательные функции сборки SAGE Engine.
# ============================================================================

# sage_add_game(NAME <exe-name> SOURCES <files...> [ASSETS <dir>])
#
# Объявляет исполняемый файл конкретной игры поверх движка. Игра линкует
# библиотеку sage::engine, получает свой каталог src как include root (чтобы
# её внутренние заголовки включались как "game/...", "voxel/..." и т.п.) и,
# если указан ASSETS, копирует свои ассеты рядом с бинарником после сборки.
#
# Каждая игра самодостаточна: добавить новую — вызвать sage_add_game из её
# games/<name>/CMakeLists.txt. Движок общий, игровой код независим.
function(sage_add_game)
    cmake_parse_arguments(GAME "" "NAME;ASSETS" "SOURCES" ${ARGN})

    if (NOT GAME_NAME)
        message(FATAL_ERROR "sage_add_game: обязателен параметр NAME")
    endif()
    if (NOT GAME_SOURCES)
        message(FATAL_ERROR "sage_add_game(${GAME_NAME}): не заданы SOURCES")
    endif()

    add_executable(${GAME_NAME} ${GAME_SOURCES})
    target_link_libraries(${GAME_NAME} PRIVATE sage::engine)
    target_include_directories(${GAME_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if (GAME_ASSETS)
        add_custom_command(TARGET ${GAME_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${GAME_ASSETS} $<TARGET_FILE_DIR:${GAME_NAME}>/assets
            COMMENT "Копирование ассетов ${GAME_NAME} рядом с бинарником")
    endif()
endfunction()
