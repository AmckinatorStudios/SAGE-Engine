#pragma once
#include <string>
#include <vector>

#include "sage/core/Problems.h"

// ---------------------------------------------------------------------------
// ГРУППЫ ПРОБЛЕМ для вкладки «Проблемы» консоли — без ImGui, чтобы правила
// проверялись тестом (tests/test_problems.cpp).
//
// Группа — первопричина и её последствия (см. sage/core/Problems.h). Сверху —
// самые серьёзные, среди равных — свежие: открыв вкладку после неудачного
// запуска, человек должен увидеть то, что сломалось только что, а не
// предупреждение получасовой давности.
// ---------------------------------------------------------------------------
namespace sage::editor::problems {

struct Filter {
    bool Errors = true;     // ошибки и аварии
    bool Warnings = true;
    bool Info = true;
    std::string Text;       // подстрока: заголовок, файл, объект (без учёта регистра)
};

struct Group {
    const sage::Problem* Root = nullptr;
    std::vector<const sage::Problem*> Effects;   // последствия, по порядку прихода
    sage::Severity Worst = sage::Severity::Info; // самое серьёзное в группе
};

// all должен жить, пока живут группы (в них указатели на его элементы).
std::vector<Group> Build(const std::vector<sage::Problem>& all, const Filter& f);
bool Passes(const sage::Problem& p, const Filter& f);

} // namespace sage::editor::problems
