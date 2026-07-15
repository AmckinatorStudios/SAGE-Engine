cmake_minimum_required(VERSION 3.18)
project(SAGEEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Имя игры, которая сейчас собирается на движке. Один и тот же движок
# (библиотека sage_engine ниже) может использоваться для нескольких игр —
# меняя эту переменную (или передавая -DGAME_NAME=... в cmake), меняется
# только имя итогового исполняемого файла, весь код движка остаётся общим.
if (NOT GAME_NAME)
    set(GAME_NAME "TheBoat")
endif()

# Версия движка читается из файла VERSION (единственный источник истины —
# используется и здесь для сборки бинарника, и в scripts/build_*.sh для
# имени пакета) и запекается в бинарник через генерируемый заголовок, чтобы
# запущенная игра тоже знала свою версию (заголовок окна, лог-баннер).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" SAGE_ENGINE_VERSION)
string(STRIP "${SAGE_ENGINE_VERSION}" SAGE_ENGINE_VERSION)
configure_file(
    ${CMAKE_SOURCE_DIR}/src/core/Version.h.in
    ${CMAKE_BINARY_DIR}/generated/core/Version.h
    @ONLY
)

include(FetchContent)

# ---------- GLFW (окно, ввод) ----------
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if (UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(glfw)

# ---------- GLM (математика) ----------
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# ---------- tinyobjloader (загрузка .obj моделей) ----------
FetchContent_Declare(
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v2.0.0rc13
)
FetchContent_MakeAvailable(tinyobjloader)

# ---------- nlohmann/json (сериализация сцен) ----------
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# ---------- Lua (язык скриптов) ----------
# У Lua нет CMake-сборки в репозитории, поэтому нам нужны только его исходники,
# которые мы компилируем сами как обычную библиотеку (аналогично glad ниже).
# SOURCE_SUBDIR указывает на несуществующую поддиректорию — это штатный способ
# сказать FetchContent_MakeAvailable "только скачай, не вызывай add_subdirectory"
# (замена устаревшей FetchContent_Populate, которую удаляют в CMake >= 3.30).
FetchContent_Declare(
  lua_src
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
  SOURCE_SUBDIR do-not-add-to-build
)
FetchContent_MakeAvailable(lua_src)
file(GLOB LUA_SOURCES ${lua_src_SOURCE_DIR}/*.c)
list(REMOVE_ITEM LUA_SOURCES
    ${lua_src_SOURCE_DIR}/lua.c
    ${lua_src_SOURCE_DIR}/luac.c
    ${lua_src_SOURCE_DIR}/ltests.c
    ${lua_src_SOURCE_DIR}/onelua.c
)
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC ${lua_src_SOURCE_DIR})
if (UNIX)
    target_link_libraries(lua54 PUBLIC m)
endif()

# ---------- sol2 (удобная C++ обёртка над Lua C API) ----------
# У sol2 есть свой CMakeLists, но нам нужны только его заголовки, а не его
# CMake-цели (они пытаются искать Lua по-своему и конфликтовали бы с нашим
# ручным lua54). Тот же приём с SOURCE_SUBDIR: только скачать исходники.
FetchContent_Declare(
  sol2_src
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG v3.3.0
  SOURCE_SUBDIR do-not-add-to-build
)
FetchContent_MakeAvailable(sol2_src)
add_library(sol2 INTERFACE)
target_include_directories(sol2 INTERFACE ${sol2_src_SOURCE_DIR}/include)
target_link_libraries(sol2 INTERFACE lua54)

# ---------- glad (загрузчик функций OpenGL) ----------
# Собран как обычная библиотека из локальных файлов в external/glad
add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)

# ---------- stb_image (загрузка PNG/JPG для текстур) ----------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE external/stb)

# ---------- tinygltf (загрузка .gltf/.glb моделей) ----------
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE external/tinygltf)

# ---------- miniaudio (звук: декодирование + устройство + 3D-микс) ----------
# Single-header, public domain. Сам выбирает системный аудио-API (ALSA/Pulse/
# WASAPI/CoreAudio) и грузит его через dlopen в рантайме — на этапе линковки
# аудио-библиотеки не нужны. Реализация разворачивается в src/audio/miniaudio_impl.cpp.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE external/miniaudio)

# ============================================================================
#  sage_engine — переиспользуемое, УНИВЕРСАЛЬНОЕ ЯДРО движка (окно, рендер,
#  камера, сцена, сериализация, UI, скриптинг). НЕ знает ничего ни о вокселях,
#  ни о конкретной игре: код движка нигде не включает src/voxel/ и src/game/
#  (это проверяемая границей архитектура — см. README, раздел "Что внутри").
#  Собирается как статическая библиотека, которую линкует тонкий исполняемый
#  файл игры ниже. Другая игра на SAGE (пусть даже не про воксели) линкует эту
#  же библиотеку и пишет свой main + свой игровой код.
#
#  Новые .cpp движка добавляйте в этот список (явный список вместо GLOB —
#  сборка детерминирована и не зависит от того, переконфигурировали ли CMake).
# ============================================================================
set(SAGE_ENGINE_SOURCES
    src/asset/AssetManager.cpp
    src/audio/AudioEngine.cpp
    src/audio/miniaudio_impl.cpp
    src/core/GLDebug.cpp
    src/core/JobSystem.cpp
    src/core/Log.cpp
    src/core/MainThreadDispatcher.cpp
    src/core/Stats.cpp
    src/core/Window.cpp
    src/render/DebugOverlay.cpp
    src/render/Mesh.cpp
    src/render/Model.cpp
    src/render/ModelLoader.cpp
    src/render/Screenshot.cpp
    src/render/Shader.cpp
    src/render/Skybox.cpp
    src/render/Texture.cpp
    src/scene/SceneSerializer.cpp
    src/scripting/ScriptEngine.cpp
    src/ui/UIRenderer.cpp
)

add_library(sage_engine STATIC ${SAGE_ENGINE_SOURCES})

# src — общий корень включений и для движка, и для игры (заголовки включают
# друг друга относительно него, напр. "render/Shader.h"). generated — для
# сгенерированного core/Version.h. Оба PUBLIC: цели, линкующие движок,
# наследуют эти пути автоматически.
target_include_directories(sage_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# В Debug-сборке включаем GL_CHECK (см. src/core/GLDebug.h) — проверку
# glGetError после обёрнутых вызовов. В Release макрос ничего не делает.
target_compile_definitions(sage_engine PUBLIC $<$<CONFIG:Debug>:SAGE_DEBUG>)

# PUBLIC — то, что торчит в публичных заголовках движка (окно/ввод/математика/
# скриптинг/текстуры): линкующая игра должна их видеть транзитивно.
target_link_libraries(sage_engine PUBLIC
    glfw
    glad
    glm::glm
    sol2
    stb
)
# PRIVATE — используется только в .cpp движка (загрузка моделей/сериализация),
# в публичных заголовках не появляется — игре знать о них не нужно.
target_link_libraries(sage_engine PRIVATE
    tinyobjloader
    tinygltf
    nlohmann_json::nlohmann_json
    miniaudio
)

if (WIN32)
    target_link_libraries(sage_engine PUBLIC opengl32)
elseif(UNIX)
    # m — нужна miniaudio (математика микшера); dl/pthread — и GLFW, и miniaudio
    # (последняя грузит ALSA/PulseAudio через dlopen в рантайме).
    target_link_libraries(sage_engine PUBLIC GL dl pthread m X11)
endif()

# ============================================================================
#  Игра "The Boat" — exe поверх движка: точка входа + игровая логика
#  (src/game/*, src/main.cpp) + воксельный модуль (src/voxel/*). Воксели —
#  это код КОНКРЕТНОЙ ИГРЫ (корабль из блоков), написанный поверх движка, а
#  не часть движка: другая игра на SAGE могла бы не подключать voxel вовсе.
#  Всё, что специфично для The Boat (корабль, рыбалка, крафт, статы, блоки),
#  живёт здесь и линкует sage_engine, но не наоборот.
# ============================================================================
set(GAME_SOURCES
    src/main.cpp
    src/game/PlayerController.cpp
    src/voxel/Chunk.cpp
    src/voxel/VoxelMesh.cpp
)

add_executable(${GAME_NAME} ${GAME_SOURCES})
target_link_libraries(${GAME_NAME} PRIVATE sage_engine)

# Копируем папку assets рядом с исполняемым файлом
add_custom_command(TARGET ${GAME_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${GAME_NAME}>/assets)
