# ============================================================================
#  SageHelpers.cmake — вспомогательные функции сборки SAGE Engine.
# ============================================================================

# sage_windows_manifest(<target>)
#
# Встраивает в windows-исполняемый файл манифест приложения SAGE
# (cmake/windows/sage.manifest). Ради одной его строки: activeCodePage=UTF-8,
# без которой узкие функции Windows (fopen, а значит и std::ifstream от
# std::string) читают наши UTF-8 пути как CP1251 и не открывают ни одного файла
# по пути с кириллицей. Подробности — в комментарии внутри самого манифеста.
#
# Вызывается для КАЖДОГО исполняемого файла: манифест — свойство процесса, и
# редактору он нужен ровно так же, как плееру и любой игре. На не-Windows
# ничего не делает.
function(sage_windows_manifest target)
    if (NOT WIN32)
        return()
    endif()
    set(SAGE_WINDOWS_MANIFEST "${SAGE_ENGINE_ROOT}/cmake/windows/sage.manifest")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_manifest.rc")
    configure_file("${SAGE_ENGINE_ROOT}/cmake/windows/sage_manifest.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()

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

    # WIN32 — приложение с ОКНОМ, а не консольное. Без этого рядом с игрой на
    # Windows вылезает чёрное окно терминала, которое нельзя закрыть отдельно
    # от самой игры: закроешь — вместе с ней закроется и игра.
    #
    # Редактор этот флаг себе поставил давно (editor/CMakeLists.txt), а всё,
    # что движок отдаёт РАЗРАБОТЧИКУ ИГРЫ, оставалось консольным — включая
    # SagePlayer, то есть каждую игру, собранную кнопкой File > Build Game.
    # Получалось, что инструмент делает себе лучше, чем своему потребителю, а
    # обойти это автор игры мог только собственным add_executable, то есть
    # отказавшись от хелпера.
    #
    # Вывод от этого не теряется: движок пишет его в лог рядом с бинарником
    # (Log::Init в main игры). На Linux флаг игнорируется — ветвление не нужно.
    add_executable(${GAME_NAME} WIN32 ${GAME_SOURCES})
    sage_windows_manifest(${GAME_NAME})
    target_link_libraries(${GAME_NAME} PRIVATE sage::engine)
    target_include_directories(${GAME_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if (GAME_ASSETS)
        add_custom_command(TARGET ${GAME_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${GAME_ASSETS} $<TARGET_FILE_DIR:${GAME_NAME}>/assets
            COMMENT "Копирование ассетов ${GAME_NAME} рядом с бинарником")
    endif()

    # Дефолтный шрифт движка (TrueType) — рядом с любой игрой, чтобы UIRenderer
    # рисовал текст настоящим шрифтом с кириллицей без ассетов в самой игре
    # (путь по умолчанию assets/fonts/sage-default.ttf). Идёт ПОСЛЕ копирования
    # игровых ассетов, чтобы игра могла положить/переопределить свой шрифт.
    add_custom_command(TARGET ${GAME_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${GAME_NAME}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SAGE_ENGINE_ROOT}/engine/assets/fonts/sage-default.ttf
                $<TARGET_FILE_DIR:${GAME_NAME}>/assets/fonts/sage-default.ttf
        COMMENT "Копирование дефолтного шрифта движка рядом с ${GAME_NAME}")
endfunction()

# sage_add_editor_plugin(NAME <lib-name> SOURCES <files...>)
#
# Объявляет плагин редактора SageEditor (v1, см. editor/src/PluginAPI.h) как
# динамическую библиотеку (MODULE — не линкуется напрямую ни во что, только
# грузится в рантайме через PluginManager). Даёт только заголовки ImGui и
# PluginAPI.h из editor/src — сознательно НЕ линкует статическую библиотеку
# imgui (в отличие от sage_add_game): у ImGui глобальное состояние (GImGui)
# живёт в статической переменной, и если плагин слинкует libimgui.a сам, он
# получит СВОЮ копию ImGui с отдельным GImGui — первый же ImGui::Begin() в
# плагине упадёт (обращение к неинициализированному контексту). Вместо этого
# вызовы ImGui:: остаются неразрешёнными символами в .so и резолвятся в
# рантайме против уже загруженного ImGui хоста SageEditor (который для этого
# собирается с ENABLE_EXPORTS, см. editor/CMakeLists.txt). По той же причине
# плагин НЕ линкует sage::engine: контракт плагина v1 не пробрасывает
# Scene&/entt::registry& через границу библиотеки (ABI-риск между
# компилятором/STL хоста и плагина).
#
# Собранная библиотека копируется в plugins/ рядом с бинарником SageEditor —
# туда же, где PluginManager её ищет по умолчанию (переопределяется
# SAGE_PLUGINS_DIR).
function(sage_add_editor_plugin)
    cmake_parse_arguments(PLUGIN "" "NAME" "SOURCES" ${ARGN})

    if (NOT PLUGIN_NAME)
        message(FATAL_ERROR "sage_add_editor_plugin: обязателен параметр NAME")
    endif()
    if (NOT PLUGIN_SOURCES)
        message(FATAL_ERROR "sage_add_editor_plugin(${PLUGIN_NAME}): не заданы SOURCES")
    endif()

    add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES})
    # Только заголовки ImGui (см. комментарий выше) — НЕ target_link_libraries(imgui),
    # чтобы не затащить в плагин статическую копию libimgui.a.
    target_include_directories(${PLUGIN_NAME} PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
        ${SAGE_ENGINE_ROOT}/editor/src)
    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")

    # Windows-линковщик (в отличие от Linux) не позволяет оставить символы
    # ImGui:: неразрешёнными в .dll — линкуем на import-библиотеку
    # SageEditor.exe (см. WINDOWS_EXPORT_ALL_SYMBOLS в editor/CMakeLists.txt).
    if (WIN32)
        target_link_libraries(${PLUGIN_NAME} PRIVATE SageEditor)
    endif()

    add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SageEditor>/plugins
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PLUGIN_NAME}> $<TARGET_FILE_DIR:SageEditor>/plugins/
        COMMENT "Копирование плагина ${PLUGIN_NAME} в plugins/ редактора")
endfunction()
