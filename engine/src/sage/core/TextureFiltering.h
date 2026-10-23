#pragma once
#include <string>

// ФИЛЬТРАЦИЯ, А НЕ «ЖАНР КАРТИНКИ».
//
// Движок не знает и не может знать, нарисована картинка по пикселю или снята
// фотоаппаратом: он знает, КАК её фильтровать. Пока настройка называлась
// жанром, она врала о себе — и её не находили там, где она нужна (мелкая
// иконка, которую надо показать резкой), и включали там, где она ни при чём.
// Один тип на весь движок: материал, картинка интерфейса, шрифт, небо,
// частицы — везде один и тот же выбор и одни и те же слова в файлах.
//
//   Smooth  — сглаживание (с мипмапами там, где картинка уменьшается);
//   Nearest — ближайший пиксель: чёткие квадраты при увеличении.
namespace sage {

enum class TextureFiltering { Smooth, Nearest };

// Как пишется в файл. СЛОВОМ, а не числом: файлы правят руками, и «nearest»
// понятно без таблицы.
inline const char* TextureFilteringKey(TextureFiltering f) {
    return f == TextureFiltering::Nearest ? "nearest" : "smooth";
}

// Непонятное слово оставляет прежнее значение (и возвращает false): опечатка в
// файле не должна тихо менять вид.
inline bool TextureFilteringFromKey(const std::string& key, TextureFiltering& out) {
    if (key == "nearest") { out = TextureFiltering::Nearest; return true; }
    if (key == "smooth") { out = TextureFiltering::Smooth; return true; }
    return false;
}

} // namespace sage
