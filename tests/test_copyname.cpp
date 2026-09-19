// ===========================================================================
//  ИМЯ ДЛЯ КОПИИ.
//
//  К имени просто дописывалось « Copy», и это работает ровно один раз. Копия
//  копии становилась «Куб Copy Copy», её копия — «Куб Copy Copy Copy»: через
//  пять нажатий Ctrl+D имя переставало помещаться в строку списка, а читать
//  его было нельзя — единственное полезное слово тонуло в хвосте из
//  одинаковых.
// ===========================================================================
#include "TestFramework.h"

#include "sage/scene/CopyName.h"

#include <set>
#include <string>

namespace {
// Занятые имена — обычным множеством: правило о строках и проверяется
// строками, сцена ему не нужна.
auto TakenIn(const std::set<std::string>& names) {
    return [&names](const std::string& n) { return names.count(n) != 0; };
}
} // namespace

TEST(CopyName_the_first_copy_is_just_Copy) {
    const std::set<std::string> taken = {"Куб"};
    CHECK_EQ(sage::scene::CopyName("Куб", TakenIn(taken)), std::string("Куб Copy"));
}

TEST(CopyName_the_next_copies_are_numbered_not_stacked) {
    // ГЛАВНАЯ проверка: основа НЕ растёт. Копия «Куб Copy» — это «Куб Copy 2»,
    // а не «Куб Copy Copy».
    std::set<std::string> taken = {"Куб", "Куб Copy"};
    CHECK_EQ(sage::scene::CopyName("Куб Copy", TakenIn(taken)), std::string("Куб Copy 2"));

    taken.insert("Куб Copy 2");
    CHECK_EQ(sage::scene::CopyName("Куб Copy 2", TakenIn(taken)), std::string("Куб Copy 3"));

    // И от исходника тоже: копировать можно и оригинал, номер всё равно
    // берётся первый свободный.
    CHECK_EQ(sage::scene::CopyName("Куб", TakenIn(taken)), std::string("Куб Copy 3"));
}

TEST(CopyName_old_stacked_names_are_shortened) {
    // Имена, накопленные прежним правилом, лежат в уже сохранённых сценах —
    // их копии тоже обязаны получаться короткими.
    const std::set<std::string> taken = {"Куб Copy Copy Copy"};
    CHECK_EQ(sage::scene::CopyName("Куб Copy Copy Copy", TakenIn(taken)),
             std::string("Куб Copy"));
}

TEST(CopyName_a_number_inside_the_name_is_not_a_copy_number) {
    // «Ящик 2» — это имя, а не «вторая копия ящика»: основу трогать нельзя.
    const std::set<std::string> taken = {"Ящик 2"};
    CHECK_EQ(sage::scene::CopyName("Ящик 2", TakenIn(taken)), std::string("Ящик 2 Copy"));
}

TEST(CopyName_an_empty_slot_needs_no_number) {
    // Ничего не занято — номер не нужен вовсе.
    CHECK_EQ(sage::scene::CopyName("Лампа", TakenIn({})), std::string("Лампа Copy"));
}
