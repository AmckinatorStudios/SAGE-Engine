cmake_minimum_required(VERSION 3.18)
project(SAGEEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Одноконфигурационные генераторы (Makefiles/Ninja) БЕЗ явного CMAKE_BUILD_TYPE
# собирают без оптимизаций (-O0) — на слабых машинах это в разы медленнее и
# легко получить случайно, просто набрав `cmake -B build`. Дефолт — Release;
# явный выбор пользователя (в т.ч. Debug) уважается.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Тип сборки (Release по умолчанию: без явного выбора получилась бы -O0)" FORCE)
    message(STATUS "SAGE: CMAKE_BUILD_TYPE не задан — использую Release")
endif()

# Хелперы сборки (sage_add_game и т.п.)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(SageHelpers)

# ============================================================================
#  Архитектура сборки (см. README):
#    engine/        — переиспользуемое ядро → статическая библиотека sage::engine
#    editor/        — редактор на ImGui     → exe SageEditor (линкует движок)
#    games/<name>/  — конкретные игры       → exe (линкуют движок, независимы)
#  Верхний уровень объявляет ВСЕ сторонние зависимости, чтобы и движок, и
#  редактор, и игры тянули один и тот же набор без дублирования.
# ============================================================================

# Версия движка — единый источник истины (файл VERSION в корне). Запекается в
# генерируемый заголовок sage/core/Version.h (кладётся в generated/, который
# движок отдаёт как PUBLIC include — заголовок виден и играм, и редактору).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" SAGE_ENGINE_VERSION)
string(STRIP "${SAGE_ENGINE_VERSION}" SAGE_ENGINE_VERSION)
configure_file(
    ${CMAKE_SOURCE_DIR}/engine/src/sage/core/Version.h.in
    ${CMAKE_BINARY_DIR}/generated/sage/core/Version.h
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

# GLFW не должен сам подтягивать заголовок OpenGL: единственный поставщик
# GL-символов в проекте — glad. Без этого порядок включения (glad раньше GLFW)
# становился хрупким — стоило какому-то заголовку включить GLFW первым (напр.
# InputSystem.h), и glad падал с «OpenGL header already included». С
# GLFW_INCLUDE_NONE GLFW даёт только оконный/вводный API, а GL берётся из glad
# в тех файлах, где он реально нужен. Определение INTERFACE — наследуется
# движком, imgui и играми автоматически.
target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

# ---------- GLM (математика) ----------
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# ---------- EnTT (ECS) ----------
# Header-only. Движок оборачивает entt в sage/ecs — но линкуем PUBLIC, чтобы
# заголовки сцены/компонентов, торчащие в публичном API, компилировались и у игр.
FetchContent_Declare(
  entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.13.2
)
FetchContent_MakeAvailable(entt)

# ---------- tinyobjloader (загрузка .obj моделей) ----------
FetchContent_Declare(
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v2.0.0rc13
)
FetchContent_MakeAvailable(tinyobjloader)

# ---------- ufbx (загрузка .fbx: меши, скелет, скины, анимации) ----------
# Одно-файловая C-библиотека (ufbx.c + ufbx.h) — как Lua, берём только исходники
# (SOURCE_SUBDIR на несуществующую папку) и собираем сами.
FetchContent_Declare(
  ufbx_src
  GIT_REPOSITORY https://github.com/ufbx/ufbx.git
  GIT_TAG v0.20.0
  SOURCE_SUBDIR do-not-add-to-build
)
FetchContent_MakeAvailable(ufbx_src)
add_library(ufbx STATIC ${ufbx_src_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx PUBLIC ${ufbx_src_SOURCE_DIR})
if (UNIX)
    target_link_libraries(ufbx PUBLIC m)
endif()

# ---------- nlohmann/json (сериализация сцен) ----------
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# ---------- Lua (язык скриптов) ----------
# У Lua нет CMake-сборки в репозитории — берём только исходники и компилируем
# сами (SOURCE_SUBDIR указывает на несуществующую поддиректорию: «только скачай,
# не вызывай add_subdirectory» — замена устаревшей FetchContent_Populate).
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

# ---------- sol2 (C++ обёртка над Lua) ----------
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
add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)

# ---------- stb_image (текстуры) ----------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE external/stb)

# ---------- tinygltf (загрузка .gltf/.glb) ----------
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE external/tinygltf)

# ---------- miniaudio (звук) ----------
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE external/miniaudio)

# ---------- Dear ImGui (docking) — UI редактора и in-game отладки ----------
# Вендорится в external/imgui (ядро + бэкенды glfw/opengl3 + std::string-хелперы).
# Собирается как статическая библиотека; линкуется редактором (и движковым
# ImGuiLayer). Использует собственный GL-загрузчик бэкенда opengl3, поэтому не
# зависит от нашего glad.
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_demo.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
    external/imgui/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends
    external/imgui/misc/cpp
)
target_link_libraries(imgui PUBLIC glfw)
# PIC: imgui линкуется и в исполняемый SageEditor, и (косвенно, через
# sage_add_editor_plugin) в MODULE-библиотеки плагинов — статическая
# библиотека без -fPIC не может быть слинкована в .so.
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
elseif (UNIX)
    target_link_libraries(imgui PUBLIC GL dl)
endif()

# ---------- ImGuizmo — 3D-гизмо (перемещение/поворот/масштаб) для редактора ----------
# Вендорится в external/imguizmo; рисует поверх ImGui, поэтому зависит от него.
add_library(imguizmo STATIC external/imguizmo/ImGuizmo.cpp)
target_include_directories(imguizmo PUBLIC external/imguizmo)
target_link_libraries(imguizmo PUBLIC imgui)

# ---------- Jolt Physics (основной физический бэкенд) ----------
# Физика движка — подключаемая (см. sage/physics/PhysicsWorld.h): встроенный
# Simple-бэкенд работает всегда без зависимостей, а Jolt — основной, «взрослый»
# бэкенд. Включён по умолчанию; выключается -DSAGE_PHYSICS_JOLT=OFF (тогда движок
# собирается только со Simple/Null — полезно для быстрой сборки/окружений без сети).
option(SAGE_PHYSICS_JOLT "Собрать физический бэкенд Jolt (основной)" ON)
if (SAGE_PHYSICS_JOLT)
    FetchContent_Declare(
      jolt
      GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
      GIT_TAG v5.3.0
      SOURCE_SUBDIR Build  # CMake-проект Jolt лежит в подкаталоге Build/
    )
    # Собираем ТОЛЬКО библиотеку — без сэмплов/тестов/вьюера/hello-world.
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    # Не позволяем Jolt перезаписывать общие флаги компилятора всего проекта
    # (его Build/CMakeLists.txt по умолчанию правит CMAKE_CXX_FLAGS глобально).
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    # Консервативный SIMD: без AVX/AVX2/AVX512 — код запустится и на CI-раннерах,
    # и в headless-окружениях без этих инструкций (иначе возможен SIGILL).
    set(USE_AVX512 OFF CACHE BOOL "" FORCE)
    set(USE_AVX2 OFF CACHE BOOL "" FORCE)
    set(USE_AVX OFF CACHE BOOL "" FORCE)
    set(USE_F16C OFF CACHE BOOL "" FORCE)
    set(USE_FMADD OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jolt)
    if (TARGET Jolt)
        message(STATUS "SAGE: физический бэкенд Jolt включён (v5.3.0)")
    else()
        message(WARNING "SAGE: Jolt запрошен, но цель не создана — собираю без него")
    endif()
endif()

# ============================================================================
#  Модули проекта
# ============================================================================
add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(games/sandbox)
add_subdirectory(games/testgame)

# Модульные тесты (математика/сериализация/анимация) — быстрые, без GL.
# Собираются всегда; гоняются через CTest или напрямую ./tests/sage_tests.
enable_testing()
add_subdirectory(tests)

# Редактор — часть стандартной сборки движка. Проверка EXISTS оставлена как
# защита на случай частичного checkout, а не как признак опциональности.
if (EXISTS "${CMAKE_SOURCE_DIR}/editor/CMakeLists.txt")
    add_subdirectory(editor)
endif()
