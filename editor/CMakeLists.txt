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
    src/ui/UIStyle.cpp
    src/ui/UI.cpp
    src/ui/Commands.cpp
    src/ui/CommandPalette.cpp
    src/EditorIcons.cpp
    src/AssetSlot.cpp
    src/ModelMaterialImport.cpp
    src/ProjectTemplates.cpp
    src/TemplateStore.cpp
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
    src/panels/TemplatesPanel.cpp
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
# Манифест с activeCodePage=UTF-8 — см. sage_windows_manifest.
sage_windows_manifest(SageEditor)

set_target_properties(SageEditor PROPERTIES ENABLE_EXPORTS ON)
if (WIN32)
    target_link_options(SageEditor PRIVATE -Wl,--export-all-symbols)
endif()

# Ассеты редактора (его шейдеры превью) — рядом с бинарником.
add_custom_command(TARGET SageEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SageEditor>/assets
    COMMENT "Копирование ассетов редактора рядом с бинарником")

# ТЕМЫ ОФОРМЛЕНИЯ — рядом с бинарником, в themes/. Читаются при запуске, а не
# вкомпилированы: добавить своё оформление можно, ничего не собирая (см.
# editor/themes/README.txt и EditorTheme.h).
add_custom_command(TARGET SageEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/themes $<TARGET_FILE_DIR:SageEditor>/themes
    COMMENT "Копирование тем оформления рядом с бинарником")

# ШАБЛОНЫ РЯДОМ С БИНАРНИКОМ БОЛЬШЕ НЕ КОПИРУЮТСЯ.
#
# Здесь стояла копия games/showcase в templates/showcase — и она ехала в архиве
# релиза к каждому скачавшему. Готовый проект это СОДЕРЖИМОЕ, а не часть
# программы: он не нужен большинству, растёт со временем и обновляется отдельно
# от редактора. Теперь такие шаблоны ставятся в работающем редакторе — из
# каталога по сети или файлом .sagetemplate с диска (см. src/TemplateStore.h), а
# папку templates/ редактор заводит сам при первой установке.
#
# Витрина при этом никуда не делась: сборка релиза пакует games/showcase в
# showcase.sagetemplate и кладёт его в тот же релиз отдельным файлом
# (.github/workflows/windows.yml), откуда редактор её и скачивает.

# Плагины редактора (см. src/PluginAPI.h) — каждый sage_add_editor_plugin()
# ниже собирается отдельной динамической библиотекой и копируется в plugins/
# рядом с SageEditor; должны идти ПОСЛЕ add_executable(SageEditor) выше — их
# POST_BUILD команда ссылается на $<TARGET_FILE_DIR:SageEditor>.
add_subdirectory(plugins/example_stats)
