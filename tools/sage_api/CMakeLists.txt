# sage_api — каталог API скриптов в файл (см. main.cpp). Линкует движок: каталог
# наполняет само состояние скриптов, окно при этом не открывается.
add_executable(sage_api main.cpp)
target_link_libraries(sage_api PRIVATE sage::engine)
if (WIN32)
    sage_windows_manifest(sage_api)
endif()
