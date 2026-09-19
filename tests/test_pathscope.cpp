// ===========================================================================
//  ГРАНИЦА ПУТИ: лежит ли файл внутри папки.
//
//  Правило маленькое, а цена ошибки большая в обе стороны. Пустит лишнего —
//  в слот материала ляжет картинка из «Загрузок», и ссылка наружу сломается
//  при сборке игры и переносе проекта на другую машину. Не пустит своего —
//  диалог запрёт человека в папке, из которой он не сможет выбрать ничего.
//
//  Тонкость ровно одна, и глазами она не видна: сравнивать НАЧАЛО СТРОКИ
//  нельзя. У «/home/user/assets2» то же начало, что у «/home/user/assets».
// ===========================================================================
#include "TestFramework.h"

#include "../editor/src/PathScope.h"

namespace scope = sage::editor::pathscope;
namespace fs = std::filesystem;

TEST(PathScope_a_file_inside_the_folder_passes) {
    const fs::path root = "/proj/assets";
    CHECK_TRUE(scope::Within(root, "/proj/assets/textures/wall.png"));
    // Сама граница — тоже «внутри»: иначе диалог не пускал бы в собственный
    // корень, то есть не открывался бы вовсе.
    CHECK_TRUE(scope::Within(root, "/proj/assets"));
}

TEST(PathScope_a_file_outside_the_folder_does_not) {
    const fs::path root = "/proj/assets";
    CHECK_FALSE(scope::Within(root, "/proj"));              // выше границы
    CHECK_FALSE(scope::Within(root, "/home/user/wall.png")); // совсем в стороне
}

TEST(PathScope_a_sibling_with_the_same_prefix_does_not) {
    // ГЛАВНАЯ проверка: «assets2» начинается с «assets», и сравнение подстрокой
    // пустило бы в соседнюю папку.
    CHECK_FALSE(scope::Within("/proj/assets", "/proj/assets2/wall.png"));
    CHECK_FALSE(scope::Within("/proj/assets", "/proj/assets_old/wall.png"));
}

TEST(PathScope_dot_dot_does_not_sneak_out) {
    // «Внутри плюс два шага вверх» — это снаружи, как бы ни выглядела запись.
    // Набрать такое можно прямо в поле имени файла.
    CHECK_FALSE(scope::Within("/proj/assets", "/proj/assets/../../secret.png"));
    // А вот шаг вверх и обратно — это та же папка.
    CHECK_TRUE(scope::Within("/proj/assets", "/proj/assets/tex/../wall.png"));
}

TEST(PathScope_an_empty_root_lets_everything_through) {
    // Пустая граница — «границы нет»: так ходит по диску диалог импорта, и
    // запереть его было бы нечем внести файл в проект.
    CHECK_TRUE(scope::Within("", "/anywhere/at/all.png"));
}
