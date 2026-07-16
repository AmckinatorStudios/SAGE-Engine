# ============================================================================
#  sage_engine — переиспользуемое, УНИВЕРСАЛЬНОЕ ядро движка (окно, ввод,
#  рендер, сцена/ECS, сериализация, UI, скриптинг, аудио). НЕ знает ничего ни
#  о вокселях, ни о конкретной игре: код движка нигде не включает game/ или
#  voxel/, а публичный API отдаётся строго под префиксом <sage/...> — игры и
#  редактор физически не могут дотянуться до внутренностей мимо этой границы.
#
#  Новые .cpp движка добавляйте в этот список (явный список вместо GLOB —
#  сборка детерминирована и не зависит от переконфигурации CMake).
# ============================================================================
set(SAGE_ENGINE_SOURCES
    src/sage/audio/AudioEngine.cpp
    src/sage/audio/miniaudio_impl.cpp
    src/sage/core/GLDebug.cpp
    src/sage/core/Log.cpp
    src/sage/core/Stats.cpp
    src/sage/core/Window.cpp
    src/sage/render/DebugOverlay.cpp
    src/sage/render/Mesh.cpp
    src/sage/render/Model.cpp
    src/sage/render/ModelLoader.cpp
    src/sage/render/Screenshot.cpp
    src/sage/render/Shader.cpp
    src/sage/render/Skybox.cpp
    src/sage/render/Texture.cpp
    src/sage/scene/SceneSerializer.cpp
    src/sage/scripting/ScriptEngine.cpp
    src/sage/ui/UIRenderer.cpp
)

add_library(sage_engine STATIC ${SAGE_ENGINE_SOURCES})
add_library(sage::engine ALIAS sage_engine)

# src — публичный include root движка: заголовки видны как "sage/<module>/...".
# generated — для сгенерированного sage/core/Version.h. Оба PUBLIC: цели,
# линкующие движок (игры, редактор), наследуют эти пути автоматически и не
# могут включить ничего движкового мимо префикса sage/.
target_include_directories(sage_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# В Debug включаем GL_CHECK (см. sage/core/GLDebug.h); в Release макрос пуст.
target_compile_definitions(sage_engine PUBLIC $<$<CONFIG:Debug>:SAGE_DEBUG>)

# PUBLIC — то, что торчит в публичных заголовках движка (окно/ввод/математика/
# ECS/скриптинг/текстуры): линкующая цель должна видеть их транзитивно.
target_link_libraries(sage_engine PUBLIC
    glfw
    glad
    glm::glm
    sol2
    stb
    EnTT::EnTT
)
# PRIVATE — используется только в .cpp движка (загрузка моделей/сериализация/
# аудио), в публичных заголовках не появляется.
target_link_libraries(sage_engine PRIVATE
    tinyobjloader
    tinygltf
    nlohmann_json::nlohmann_json
    miniaudio
)

if (WIN32)
    target_link_libraries(sage_engine PUBLIC opengl32)
elseif (UNIX)
    # m — miniaudio (математика микшера); dl/pthread — GLFW и miniaudio
    # (последняя грузит ALSA/PulseAudio через dlopen в рантайме).
    target_link_libraries(sage_engine PUBLIC GL dl pthread m X11)
endif()
