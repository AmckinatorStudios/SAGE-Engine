# Упаковщик шаблонов — маленькая программа для сборки релиза и для скриптов
# (см. main.cpp). Линкует движок ради формата пакета: второй разбор того же
# двоичного формата рядом означал бы два описания, которые однажды разойдутся.
add_executable(sage_template main.cpp)
target_link_libraries(sage_template PRIVATE sage::engine nlohmann_json::nlohmann_json)
