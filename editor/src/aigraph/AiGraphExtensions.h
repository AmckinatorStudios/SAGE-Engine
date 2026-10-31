#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// РАСШИРЕНИЯ РЕДАКТОРА ГРАФА — то, что плагин редактора (editor.lua)
// встраивает в окно SAGE AI Graph, не трогая его кода:
//
//   editor.graph_pin("Target", function(v) ... return v end)  — своё поле для
//       значения типа в окне свойств узла (вместо «только показать»);
//   editor.graph_tool("Разложить", function(graph) ... end)  — своя кнопка на
//       панели инструментов графа.
//
// Владелец (id плагина) нужен, чтобы выгрузка плагина убирала ровно его
// расширения: функции Lua живут в состоянии плагина и после выгрузки
// висели бы в пустоте.
// ---------------------------------------------------------------------------
namespace sage::editor::aigraph {

struct PinEditorExt {
    std::string Owner;
    std::string Type;
    // Нарисовать поле (виджетами ImGui / ui.*) и вернуть true, если значение изменено.
    std::function<bool(sage::vars::Value&)> Edit;
};

// Что видит инструмент: открытый граф и выделенные узлы.
struct ToolContext {
    std::string Path;
    std::string Name;
    std::vector<int> Selected;
};

struct ToolExt {
    std::string Owner;
    std::string Title;
    std::function<void(const ToolContext&)> Run;
};

void AddPinEditor(PinEditorExt ext);
void AddTool(ToolExt ext);
void RemoveOwner(const std::string& owner);
const PinEditorExt* FindPinEditor(const std::string& type);
const std::vector<ToolExt>& Tools();

} // namespace sage::editor::aigraph
