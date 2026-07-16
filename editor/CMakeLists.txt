# ============================================================================
#  SageEditor — редактор на ImGui поверх движка. Отдельный исполняемый файл:
#  линкует sage::engine (как и любая игра) плюс imgui (интеграция ImGui держится
#  в редакторе, чтобы движок и игры не тянули imgui без необходимости).
# ============================================================================
add_executable(SageEditor
    src/main.cpp
    src/EditorLayer.cpp
)

target_link_libraries(SageEditor PRIVATE sage::engine imgui)
target_include_directories(SageEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Ассеты редактора (его шейдеры превью) — рядом с бинарником.
add_custom_command(TARGET SageEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SageEditor>/assets
    COMMENT "Копирование ассетов редактора рядом с бинарником")
