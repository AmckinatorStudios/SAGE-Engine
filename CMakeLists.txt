cmake_minimum_required(VERSION 3.18)
project(SAGEEngine LANGUAGES C CXX)

# Ресурсы Windows (RC) — ради манифеста приложения с activeCodePage=UTF-8,
# см. cmake/windows/sage.manifest и sage_windows_manifest() в SageHelpers.
# Отдельным языком, потому что по умолчанию CMake его не включает, а без него
# .rc-файл в списке исходников просто игнорируется — молча.
if (WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NOMINMAX — не «на всякий случай», а обязательное условие сборки под Windows.
# <windows.h> определяет min и max как ФУНКЦИОНАЛЬНЫЕ МАКРОСЫ, а препроцессор не
# знает про области видимости: `std::min(a, b)` превращается в мусор ещё до
# компилятора, и MSVC ругается загадочным «C2589: '(': illegal token on right
# side of '::'» — на строку, где никакой ошибки нет. Windows.h приходит сюда
# транзитивно (glad, dbghelp в обработчике падений), то есть отравить может
# любой файл, никогда о Windows не слышавший. Определяем ОДИН раз на весь
# проект, до объявления зависимостей: точечные #define в отдельных .cpp
# лечили бы симптом ровно до следующего include.
if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Рантайм MSVC — динамический (/MD), ЯВНО и для всех целей сразу.
#
# Смешивать /MT и /MD в одной программе нельзя: у статического и динамического
# рантайма своя куча и своё состояние локали, поэтому компоновщик отказывается
# сводить их вместе. Оставлять это «по умолчанию» — значит зависеть от того, что
# ни одна зависимость не переопределит настройку у себя; ровно так Jolt и ломал
# сборку игры под Windows (см. ниже, USE_STATIC_MSVC_RUNTIME_LIBRARY).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Одноконфигурационные генераторы (Makefiles/Ninja) БЕЗ явного CMAKE_BUILD_TYPE
# собирают без оптимизаций (-O0) — на слабых машинах это в разы медленнее и
# легко получить случайно, просто набрав `cmake -B build`. Дефолт — Release;
# явный выбор пользователя (в т.ч. Debug) уважается.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Тип сборки (Release по умолчанию: без явного выбора получилась бы -O0)" FORCE)
    message(STATUS "SAGE: CMAKE_BUILD_TYPE не задан — использую Release")
endif()

# Корень репозитория движка. НЕ CMAKE_SOURCE_DIR: движок умеет подключаться в
# чужой проект через add_subdirectory (см. Director 3D) — тогда CMAKE_SOURCE_DIR
# указывает на корень ТОГО проекта, и все пути к ассетам/заголовкам движка
# уехали бы. CACHE INTERNAL — значение видно вложенным каталогам и хелперам.
set(SAGE_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Корень репозитория SAGE Engine")

# Хелперы сборки (sage_add_game и т.п.)
list(APPEND CMAKE_MODULE_PATH ${SAGE_ENGINE_ROOT}/cmake)
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
file(READ "${SAGE_ENGINE_ROOT}/VERSION" SAGE_ENGINE_VERSION)
string(STRIP "${SAGE_ENGINE_VERSION}" SAGE_ENGINE_VERSION)
configure_file(
    ${SAGE_ENGINE_ROOT}/engine/src/sage/core/Version.h.in
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

# ---------- miniz (deflate для своих бинарных форматов) ----------
# Один C-файл без зависимостей. Нужен контейнеру sage::assets (см.
# engine/src/sage/assets/format/Blob.h): свои форматы жмут куски deflate'ом
# поверх собственной упаковки. Писать свой энтропийный кодек ради этого не
# стоит — основную экономию даёт квантование, а универсальное сжатие лишь
# добирает остаток, и здесь важнее, чтобы оно было заведомо правильным.
#
# Берём ТОЛЬКО исходники и компилируем сами (SOURCE_SUBDIR указывает на
# несуществующий каталог — «скачай, но не зови add_subdirectory»), как уже
# сделано для Lua и sol2. Причина не в стиле: CMakeLists самого miniz начинается
# с `cmake_minimum_required(VERSION 3.0)`, а CMake 4 совместимость с <3.5 убрал
# совсем — конфигурация падает с ошибкой ещё до компиляции. Так и сломалась
# сборка игры под Windows: на моей машине CMake старее и всё проходило.
#
# Чинить это флагом CMAKE_POLICY_VERSION_MINIMUM можно, но он глобальный и
# распространился бы на все остальные зависимости, пряча их будущие проблемы.
# Два C-файла проще собрать самим, чем зависеть от чужого CMake, который в
# любой момент опять разойдётся с версией инструмента.
FetchContent_Declare(
  miniz_src
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  SOURCE_SUBDIR do-not-add-to-build
)
FetchContent_MakeAvailable(miniz_src)
# miniz.h включает miniz_export.h, который обычно генерирует CMake самого miniz
# (GenerateExportHeader). Для СТАТИЧЕСКОЙ сборки в нём нет ничего, кроме пустого
# MINIZ_EXPORT: экспортировать из статической библиотеки нечего.
set(MINIZ_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/miniz)
file(WRITE ${MINIZ_GENERATED_DIR}/miniz_export.h
"#ifndef MINIZ_EXPORT_H\n#define MINIZ_EXPORT_H\n#define MINIZ_EXPORT\n#endif\n")
add_library(miniz STATIC
    ${miniz_src_SOURCE_DIR}/miniz.c
    ${miniz_src_SOURCE_DIR}/miniz_tdef.c
    ${miniz_src_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_src_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${miniz_src_SOURCE_DIR} ${MINIZ_GENERATED_DIR})
# PIC: движок линкуется в MODULE-библиотеки плагинов редактора.
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
# ПРОВЕРКИ ТИПОВ ВКЛЮЧЕНЫ НАМЕРЕННО, и это не «на всякий случай».
#
# По умолчанию sol2 аргументы из Lua НЕ ПРОВЕРЯЕТ: функция, объявленная как
# (GameObject&), получив из скрипта число, приводит это число к указателю и
# разыменовывает его. То есть ОПЕЧАТКА В СКРИПТЕ роняет процесс — редактор с
# несохранённой сценой или игру у игрока. Проверено: SetMeshCube(5) из Lua
# давал SIGSEGV. Функция, объявленная как (int), получив сущность, наоборот,
# молча получала ноль — DestroyObject(entity) не удалял ничего и не жаловался.
#
# С проверками то же самое становится обычной ошибкой Lua с внятным текстом,
# которую движок ловит (все вызовы идут через protected_function) и пишет в
# консоль с именем файла и строкой. Цена — проверка типа на аргумент вызова;
# для скриптового API, который зовут десятки раз за кадр, это ничто по
# сравнению с падением.
target_compile_definitions(sol2 INTERFACE SOL_ALL_SAFETIES_ON=1)

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

# ---------- Vulkan (второй графический бэкенд) ----------
#
# НИ SDK, НИ БИБЛИОТЕКИ ДЛЯ ЛИНКОВКИ НЕ ТРЕБУЕТСЯ, и это принципиально. Обычный
# путь — find_package(Vulkan) и линковка с vulkan-1.lib из LunarG SDK — означал
# бы, что собрать движок нельзя без установки полутора гигабайт стороннего
# пакета. Вместо этого:
#
#   • заголовки берём у Khronos напрямую (header-only, ~2 МБ);
#   • сам загрузчик (vulkan-1.dll / libvulkan.so.1) открываем в РАНТАЙМЕ через
#     volk. Тогда бинарник запускается и на машине без Vulkan вообще — просто
#     сообщает, что бэкенд недоступен, и откатывается на OpenGL, вместо отказа
#     запускаться с «не найден vulkan-1.dll».
#
# Выключается -DSAGE_VULKAN=OFF: сборка без сети и окружения, где второй бэкенд
# не нужен, не должны за него платить.
option(SAGE_VULKAN "Собрать графический бэкенд Vulkan (второй, рядом с OpenGL)" ON)
if (SAGE_VULKAN)
    FetchContent_Declare(
      vulkan_headers
      GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
      GIT_TAG v1.3.290
    )
    FetchContent_MakeAvailable(vulkan_headers)

    # volk — загрузчик точек входа Vulkan (MIT, два файла). Берём только
    # исходники и компилируем сами: свой CMake volk'а ищет SDK, который мы как
    # раз и не хотим требовать.
    FetchContent_Declare(
      volk_src
      GIT_REPOSITORY https://github.com/zeux/volk.git
      GIT_TAG 1.3.270
      SOURCE_SUBDIR do-not-configure-volk
    )
    FetchContent_MakeAvailable(volk_src)
    # VMA — распределитель видеопамяти от AMD (MIT, один заголовок).
    #
    # Своего здесь быть не должно. Vulkan даёт сырое vkAllocateMemory с
    # ограничением maxMemoryAllocationCount (на десктопе обычно 4096): выделять
    # блок под КАЖДУЮ текстуру нельзя, кончится на средней сцене. Значит нужен
    # субаллокатор с пулами по типам памяти, выравниванием, дефрагментацией
    # линейных и нелинейных ресурсов — три сотни строк, которые ошибаются молча
    # и проявляются на чужой видеокарте через полгода. VMA это и есть, только
    # написанный теми, кто пишет драйвер.
    FetchContent_Declare(
      vma_src
      GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
      GIT_TAG v3.1.0
      SOURCE_SUBDIR do-not-configure-vma
    )
    FetchContent_MakeAvailable(vma_src)
    add_library(vma INTERFACE)
    target_include_directories(vma INTERFACE ${vma_src_SOURCE_DIR}/include)
    target_link_libraries(vma INTERFACE Vulkan-Headers)
    # Заголовок VMA включает vulkan.h сам. Без VK_NO_PROTOTYPES он притащит
    # обычные прототипы, и сборка потребует линковки с vulkan-1.lib — то есть
    # ровно тот SDK, ради избавления от которого взят volk.
    target_compile_definitions(vma INTERFACE VK_NO_PROTOTYPES)

    # glslang — компиляция GLSL в SPIR-V В РАНТАЙМЕ.
    #
    # Офлайн-компиляции недостаточно, и это не вопрос удобства. Движок позволяет
    # писать СВОИ шейдеры материалов (см. docs/custom_shaders.md) — они приходят
    # из проекта пользователя и компилируются при загрузке. Заранее их не
    # собрать: их ещё не существует, когда собирается движок.
    #
    # Собираем минимально: без бинарников, без тестов, без HLSL и без
    # оптимизатора SPIRV-Tools. Оптимизатор — самая долгая часть сборки, а
    # выигрыш от него на наших шейдерах меньше, чем стоимость его присутствия в
    # каждом прогоне CI.
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      glslang
      GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
      GIT_TAG 14.3.0
    )
    FetchContent_MakeAvailable(glslang)

    add_library(volk STATIC ${volk_src_SOURCE_DIR}/volk.c)
    target_include_directories(volk PUBLIC ${volk_src_SOURCE_DIR})
    target_link_libraries(volk PUBLIC Vulkan-Headers)
    if (WIN32)
        target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
    endif()
    if (UNIX AND NOT APPLE)
        target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(volk PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

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
    # Предупреждения Jolt НЕ должны быть ошибками.
    #
    # Jolt собирается с /W4 /WX (или -Wall -Werror), и это его право — но
    # ломается на этом НАШ выпуск. Windows SDK 10.0.26100 выдаёт C4865 внутри
    # СВОЕГО же wingdi.h, /WX превращает это в ошибку, и сборка для Windows
    # падает целиком — на коде, которого нет ни в движке, ни в Jolt.
    #
    # Чужой код нам чинить нечем, а зависеть от того, что в следующей версии
    # SDK предупреждение исчезнет, — значит снова остаться без сборки в самый
    # неподходящий момент. Собственный код движка при этом продолжает собираться
    # со своими предупреждениями: это послабление касается ТОЛЬКО зависимости.
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    # Рантайм MSVC — динамический, как у всего остального.
    #
    # Jolt по умолчанию включает USE_STATIC_MSVC_RUNTIME_LIBRARY и собирает СЕБЯ
    # с /MT. Смешивать статический и динамический рантайм в одной программе
    # нельзя: получаются две копии кучи и две копии состояния локали, а
    # компоновщик честно отказывается — «mismatch detected for RuntimeLibrary»
    # и десятки LNK2005 на дублирующихся символах. Именно на этом падала сборка
    # игры под Windows, уже после того как весь код скомпилировался.
    #
    # Выравниваем на динамический, а не на статический: /MT у всех целей потянул
    # бы за собой ту же несовместимость с любой другой зависимостью, собранной
    # обычным способом, — а плагины редактора это ещё и .dll, которым отдельная
    # копия кучи противопоказана.
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jolt)
    if (TARGET Jolt)
        message(STATUS "SAGE: физический бэкенд Jolt включён (v5.3.0)")
        # На случай, если версия Jolt не знает про ENABLE_ALL_WARNINGS: снимаем
        # флаг «предупреждения как ошибки» с самой цели.
        if (MSVC)
            target_compile_options(Jolt PRIVATE /WX-)
        else()
            target_compile_options(Jolt PRIVATE -Wno-error)
        endif()
    else()
        message(WARNING "SAGE: Jolt запрошен, но цель не создана — собираю без него")
    endif()
endif()

# ============================================================================
#  Модули проекта
#
#  Движок собирается ВСЕГДА; всё остальное (рантайм-плеер, редактор, примеры,
#  тесты) — переключаемо. Дефолт ON: обычная сборка репозитория движка ничем
#  не отличается от прежней. Выключение нужно СТОРОННЕМУ приложению, которое
#  подключает движок к себе через add_subdirectory (например, инструмент
#  анимации Director 3D) — ему нужна только библиотека sage::engine плюс
#  общие зависимости (imgui/imguizmo/json), а не чужие исполняемые файлы.
# ============================================================================
option(SAGE_BUILD_RUNTIME "Собрать рантайм-плеер SagePlayer" ON)
option(SAGE_BUILD_EDITOR  "Собрать редактор SageEditor" ON)
option(SAGE_BUILD_SAMPLES "Собрать игры-примеры (sandbox, testgame)" ON)
option(SAGE_BUILD_TESTS   "Собрать модульные тесты движка" ON)

add_subdirectory(engine)

if (SAGE_BUILD_RUNTIME)
    add_subdirectory(runtime)
endif()

if (SAGE_BUILD_SAMPLES)
    add_subdirectory(games/sandbox)
    add_subdirectory(games/testgame)
endif()

# Модульные тесты (математика/сериализация/анимация) — быстрые, без GL.
# Гоняются через CTest или напрямую ./tests/sage_tests.
if (SAGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Редактор — часть стандартной сборки движка. Проверка EXISTS оставлена как
# защита на случай частичного checkout, а не как признак опциональности.
if (SAGE_BUILD_EDITOR AND EXISTS "${SAGE_ENGINE_ROOT}/editor/CMakeLists.txt")
    add_subdirectory(editor)
endif()
