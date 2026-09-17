#pragma once
#include <nlohmann/json.hpp>

#include "sage/events/Events.h"
#include "sage/vars/ScriptVars.h"

// ---------------------------------------------------------------------------
// ЗНАЧЕНИЯ И СВЯЗИ В JSON — общий словарь для всех, кто пишет данные сцены.
//
// Переменные скрипта (sage::vars::Table) и связи «когда здесь случилось X —
// сделать Y» (sage::events::Bindings) попадают в файл из ДВУХ мест: из
// компонентов сцены и из компонентов интерфейса, который теперь умеет жить
// отдельным ресурсом (.sageui). Пока перевод лежал внутри сериализатора сцен,
// второму месту оставалось повторить его у себя — то есть завести второе
// описание одного формата, которое разойдётся с первым на третьем добавленном
// типе значения, и молча.
// ---------------------------------------------------------------------------
namespace sage::scene {

nlohmann::json ValueToJson(const sage::vars::Value& v);
sage::vars::Value ValueFromJson(const nlohmann::json& j);

nlohmann::json VarsToJson(const sage::vars::Table& table);
void VarsFromJson(const nlohmann::json& in, sage::vars::Table& table);

nlohmann::json BindingsToJson(const sage::events::Bindings& bindings);
void BindingsFromJson(const nlohmann::json& in, sage::events::Bindings& out);

} // namespace sage::scene
