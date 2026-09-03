# ============================================================================
#  SageEditor — редактор на ImGui поверх движка. Отдельный исполняемый файл:
#  линкует sage::engine (как и любая игра) плюс imgui (интеграция ImGui держится
#  в редакторе, чтобы движок и игры не тянули imgui без необходимости).
# ============================================================================
# WIN32 — приложение с ОКНОМ, а не консольное. Без этого рядом с редактором
# на Windows вылезает чёрное окно терминала, которое нельзя закрыть отдельно
# от самой программы. Весь вывод и без консоли пишется в sage_editor.log
# рядом с бинарником (см. Log::Init в src/main.cpp).
#
# На Linux/macOS флаг игнорируется, поэтому ветвления здесь не нужно — тот же
# приём, что и у Director 3D (см. Director3D/CMakeLists.txt).
add_executable(SageEditor WIN32
    src/main.cpp
    src/EditorLayer.cpp
    src/EditorLayer_Project.cpp
    src/EditorLayer_Play.cpp
    src/EditorLayer_Edit.cpp
    src/EditorLayer_Assets.cpp
    src/EditorLayer_Ui.cpp
    src/EditorSelfTest.cpp
    src/EditorSceneRenderer.cpp
    src/EditorTheme.cpp
    src/EditorIcons.cpp
    src/AssetSlot.cpp
    src/ModelMaterialImport.cpp
    src/ProjectTemplates.cpp
    src/ProjectTemplateCover.cpp
    src/Project.cpp
    src/PluginManager.cpp
    src/RecentProjects.cpp
    src/Localization.cpp
    src/panels/ConsolePanel.cpp
    src/panels/HierarchyPanel.cpp
    src/panels/InspectorPanel.cpp
    src/panels/InspectorPanel_Material.cpp
    src/panels/InspectorPanel_Entity.cpp
    src/panels/InspectorPanel_Ui.cpp
    src/panels/ViewportPanel.cpp
    src/panels/GamePanel.cpp
    src/panels/AssetsPanel.cpp
    src/panels/LauncherPanel.cpp
    src/panels/EnvironmentPanel.cpp
    src/EditorPrefs.cpp
    src/UIElementProperties.cpp
    src/VarsEditor.cpp
    src/panels/TopBarPanel.cpp
    src/panels/ViewportTools.cpp
    src/panels/SettingsPanel.cpp
    src/panels/DialogsPanel.cpp
    src/panels/ProfilerPanel.cpp
    src/FileBrowser.cpp
    src/ConfirmDialog.cpp
    src/CodeEditor.cpp
    src/UICanvas.cpp
    src/UILayoutOps.cpp
    src/panels/UIEditorPanel.cpp
    src/AssetPreview.cpp
)

# imguizmo — 3D-гизмо во вьюпорте; nlohmann_json — файлы проектов (.sageproj).
target_link_libraries(SageEditor PRIVATE sage::engine imgui imguizmo nlohmann_json::nlohmann_json)
target_include_directories(SageEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ImGui хранит своё состояние (GImGui) в статической глобальной переменной.
# Плагины (.so) НЕ линкуют libimgui.a сами (см. sage_add_editor_plugin) —
# иначе получили бы СВОЮ копию ImGui с отдельным GImGui и падали бы при
# первом же ImGui::Begin(). Вместо этого их вызовы ImGui:: остаются
# неразрешёнными символами в .so и резолвятся в рантайме (dlopen) против уже
# загруженного ImGui хоста — а для этого хост должен экспортировать свои
# символы (аналог -rdynamic на Linux).
# На Windows недостаточно ENABLE_EXPORTS — DLL плагина не линкуется с
# неразрешёнными символами (в отличие от Linux/.so), поэтому дополнительно
# просим линковщик экспортировать ВСЕ символы SageEditor.exe и сгенерировать
# import-библиотеку (libSageEditor.dll.a), на которую линкуется плагин (см.
# sage_add_editor_plugin в cmake/SageHelpers.cmake). ВАЖНО: CMake-свойство
# WINDOWS_EXPORT_ALL_SYMBOLS применяется только к SHARED-таргетам — для EXE
# оно молча игнорируется (implib получается пустым/не создаётся), поэтому
# флаг линковщика --export-all-symbols передаётся напрямую. На Linux/macOS
# ENABLE_EXPORTS/этот флаг не действуют, там резолвинг ImGui:: символов
# плагина — рантаймовый (dlopen против уже загруженного процесса).
set_target_properties(SageEditor PROPERTIES ENABLE_EXPORTS ON)
if (WIN32)
    target_link_options(SageEditor PRIVATE -Wl,--export-all-symbols)
endif()

# Ассеты редактора (его шейдеры превью) — рядом с бинарником.
add_custom_command(TARGET SageEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SageEditor>/assets
    COMMENT "Копирование ассетов редактора рядом с бинарником")

# ГОТОВЫЕ ПРОЕКТЫ-ШАБЛОНЫ — рядом с бинарником, в templates/<id>.
#
# Копией, а не второй версией в editor/assets: источник у витрины ОДИН
# (games/showcase), она собирается, гоняется автопрогоном и правится как
# обычный проект. Держать её вторую копию внутри редактора значило бы
# обновлять обе руками — и однажды забыть.
#
# docs/ (скриншоты для README витрины) не копируем: человеку, который создал
# из неё проект, они не нужны, а весят они больше всего остального вместе.
add_custom_command(TARGET SageEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SageEditor>/templates/showcase
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/games/showcase/project.sageproj
            $<TARGET_FILE_DIR:SageEditor>/templates/showcase/project.sageproj
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/games/showcase/scenes
            $<TARGET_FILE_DIR:SageEditor>/templates/showcase/scenes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/games/showcase/assets
            $<TARGET_FILE_DIR:SageEditor>/templates/showcase/assets
    COMMENT "Копирование шаблона-витрины в templates/ редактора")

# Плагины редактора (см. src/PluginAPI.h) — каждый sage_add_editor_plugin()
# ниже собирается отдельной динамической библиотекой и копируется в plugins/
# рядом с SageEditor; должны идти ПОСЛЕ add_executable(SageEditor) выше — их
# POST_BUILD команда ссылается на $<TARGET_FILE_DIR:SageEditor>.
add_subdirectory(plugins/example_stats)
