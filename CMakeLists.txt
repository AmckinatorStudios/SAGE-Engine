cmake_minimum_required(VERSION 3.16)
project(SAGEEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Имя игры, которая сейчас собирается на движке. Один и тот же движок
# (SAGEEngine) может использоваться для нескольких игр — меняя эту
# переменную (или передавая -DGAME_NAME=... в cmake), меняется только
# имя итогового исполняемого файла, весь код движка остаётся общим.
if (NOT GAME_NAME)
    set(GAME_NAME "TheBoat")
endif()

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
# У Lua нет официальной поддержки CMake, поэтому качаем исходники и собираем
# сами как обычную библиотеку (аналогично glad ниже).
FetchContent_Declare(
  lua_src
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
)
FetchContent_Populate(lua_src)
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
FetchContent_Declare(
  sol2_src
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG v3.3.0
)
FetchContent_Populate(sol2_src)
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

# ---------- Исходники движка ----------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

add_executable(SAGEEngine ${ENGINE_SOURCES})
set_target_properties(SAGEEngine PROPERTIES OUTPUT_NAME "${GAME_NAME}")

target_include_directories(SAGEEngine PRIVATE src)

# В Debug-сборке включаем GL_CHECK (см. src/core/GLDebug.h) — проверку
# glGetError после обёрнутых вызовов. В Release макрос ничего не делает.
target_compile_definitions(SAGEEngine PRIVATE $<$<CONFIG:Debug>:SAGE_DEBUG>)

target_link_libraries(SAGEEngine PRIVATE
    glfw
    glad
    glm::glm
    tinyobjloader
    nlohmann_json::nlohmann_json
    sol2
    stb
    tinygltf
)

if (WIN32)
    target_link_libraries(SAGEEngine PRIVATE opengl32)
elseif(UNIX)
    target_link_libraries(SAGEEngine PRIVATE GL dl pthread X11)
endif()

# Копируем папку assets рядом с исполняемым файлом
add_custom_command(TARGET SAGEEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:SAGEEngine>/assets)
