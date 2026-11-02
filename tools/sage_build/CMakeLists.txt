# sage_build — сборка игры из проекта без редактора (CI, выпуск игры): та же
# sage::assets::BuildGame, что у «File > Build Game».
add_executable(sage_build main.cpp)
target_link_libraries(sage_build PRIVATE sage::engine nlohmann_json::nlohmann_json)
if (WIN32)
    sage_windows_manifest(sage_build)
endif()
