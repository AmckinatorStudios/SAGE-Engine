#pragma once
#include <string>
#include <vector>

#include "sage/ai/Graph.h"

// ---------------------------------------------------------------------------
// ПРОВЕРКА ГРАФА — одна для редактора и для запуска.
//
// Редактор показывает её список под холстом и обводит узлы с ошибкой; запуск
// не начинает граф с ошибкой (Error), а предупреждения (Warning) только
// показываются. Одно правило в двух местах — граф, «зелёный» в редакторе, не
// падает в игре на том, что редактор мог увидеть заранее.
// ---------------------------------------------------------------------------
namespace sage::ai {

class World;

struct GraphIssue {
    int Node = 0;          // 0 — о графе целиком
    bool Error = true;     // false — предупреждение
    std::string Text;
};

std::vector<GraphIssue> ValidateGraph(const GraphDoc& doc, const World& world);

} // namespace sage::ai
