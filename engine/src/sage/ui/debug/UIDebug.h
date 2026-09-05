#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIContext.h"
#include "sage/ui/layout/UILayoutSolver.h"
#include "sage/ui/render/UIRenderList.h"

// ---------------------------------------------------------------------------
// ОТЛАДКА И ПРОФАЙЛЕР ИНТЕРФЕЙСА (§109–114 ТЗ).
//
// «НИКАКОЙ МАГИИ» (§114) — это не лозунг, а требование к инструментам. На
// каждый из четырёх вопросов система обязана отвечать конкретной цепочкой:
//   • откуда взялось положение элемента → UIExplainPosition;
//   • почему его не видно            → UIExplainVisibility;
//   • почему он выше другого         → UIExplainOrder;
//   • откуда взялась тень            → UIExplainEffects.
// Ответы — текстом, потому что читает их человек, а не программа.
//
// СТОИМОСТЬ ВИДНА (§113). Профайлер показывает не только «сколько узлов», но и
// сколько это стоило: батчи, проходы масок, проходы эффектов, промежуточные
// цели. Разработчик, включивший размытие на десяти панелях, обязан увидеть
// десять проходов, а не гадать, почему стало медленно.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIDocument;

struct UIProfile {
    UILayoutStats Layout;
    UIRenderStats Render;
    double InputMs = 0.0;
    double TotalMs = 0.0;

    std::string Summary() const;
};

// Отладочные слои поверх интерфейса: границы, якоря, опорные точки, маски,
// области попадания, номера слоёв, границы батчей. Дорисовываются в тот же
// список команд — то есть тем же конвейером, а не «поверх, как получится».
void UIAppendDebugOverlay(const UIDocument& doc, const UILayoutSolver& layout,
                          const UIContext& ctx, UIRenderList& list);

// --- Объяснения ---------------------------------------------------------------
std::string UIExplainPosition(const UIDocument& doc, const UILayoutSolver& layout,
                              UINodeId id);
std::string UIExplainVisibility(const UIDocument& doc, const UILayoutSolver& layout,
                                UINodeId id);
std::string UIExplainOrder(const UIDocument& doc, const UILayoutSolver& layout, UINodeId id);
std::string UIExplainEffects(const UIDocument& doc, UINodeId id);

// Дерево документа текстом — для консоли, лога и отчёта об ошибке (§111).
std::string UIDumpTree(const UIDocument& doc, const UILayoutSolver* layout = nullptr);

// --- Проверки целостности (§135) ---------------------------------------------
//
// Не «ассерты на всякий случай», а список того, что реально ломается: цикл в
// иерархии, ссылка на несуществующего родителя, узел без прямоугольника, маска
// без формы, экземпляр префаба без источника.
struct UIValidationIssue {
    UINodeId Node = kUIInvalidNode;
    std::string Message;
    bool Fatal = false;
};
std::vector<UIValidationIssue> UIValidate(const UIDocument& doc);

} // namespace sage::ui
