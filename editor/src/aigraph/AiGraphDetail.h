#pragma once
#include <string>

#include "imgui.h"

#include "sage/ai/Types.h"
#include "sage/vars/Value.h"

class EditorHost;

// ---------------------------------------------------------------------------
// Помощники окон SAGE AI Graph (AiGraphPanel*.cpp, AiDebugPanel.cpp).
// ---------------------------------------------------------------------------
namespace sage::editor::aigraph {

// Поле строки фиксированной длины поверх std::string.
bool InputStr(const char* id, std::string& text, const char* hint = nullptr);
// Поле значения по типу графа: число, вектор, объект, поворот… Тип, у которого
// нет своего поля, показывается строкой — либо полем плагина (graph_pin).
bool ValueField(EditorHost& host, const char* id, const std::string& type, sage::vars::Value& value,
                const sage::ai::TypeRegistry& types);
// Цвет пина и провода по типу.
ImU32 TypeColor(const sage::ai::TypeRegistry& types, const std::string& type);
// Перевод строки из реестра узлов (заголовок, раздел, описание): английский
// ключ — русский текст, если он есть в каталоге.
const char* Tr(const std::string& key);
// Раздел палитры «Math/Vectors» — по частям: «Математика/Векторы».
std::string TrPath(const std::string& category);
// Строка не длиннее max символов (а не байт): обрезка по байтам резала
// русскую букву пополам, и у пина появлялся «битый» знак.
std::string ClipUtf8(const std::string& s, size_t max);

} // namespace sage::editor::aigraph
