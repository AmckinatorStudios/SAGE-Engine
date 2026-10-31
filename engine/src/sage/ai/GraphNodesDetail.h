#pragma once
#include <string>
#include <vector>

#include "sage/ai/GraphRuntime.h"
#include "sage/ai/Nodes.h"

// ---------------------------------------------------------------------------
// Помощники описания узлов — общие для GraphNodes_*.cpp и для слоя скриптов,
// который регистрирует узлы мира движка. Короткие имена ради читаемости
// таблиц: строка описания узла должна помещаться в строку.
// ---------------------------------------------------------------------------
namespace sage::ai::nodes {

PinSpec P(const std::string& name, const std::string& type, vars::Value def = {}, const std::string& title = {});
PinSpec X(const std::string& name, const std::string& title = {});   // пин потока
SettingSpec S(const std::string& name, const std::string& kind, vars::Value def = {},
              const std::string& title = {}, std::vector<std::string> choices = {});
NodeType Node(const std::string& id, const std::string& title, const std::string& category,
              const std::string& description);
std::string SettingOf(const GraphNode& n, const std::string& name, const std::string& fallback = {});
int SettingIntOf(const GraphNode& n, const std::string& name, int fallback = 0);
std::string TypeOfValue(const GraphEnv& env, const vars::Value& v);

// Математика, сравнения, логика, строки, списки, структуры, повороты.
void RegisterDataNodes(NodeRegistry& nodes);
// Решение с приоритетами (Decide) — AI Graph.
void RegisterDecisionNodes(NodeRegistry& nodes);

} // namespace sage::ai::nodes
