#pragma once
#include <nlohmann/json.hpp>

#include "sage/scene/Signals.h"
#include "sage/vars/ScriptVars.h"

// ---------------------------------------------------------------------------
// ЗНАЧЕНИЯ И СВЯЗИ В JSON — общий словарь для всех, кто пишет данные сцены.
//
// Переменные скрипта (sage::vars::Table) и связи сигналов
// (sage::signals::Link) попадают в файл из ДВУХ мест: из компонентов сцены и
// из интерфейса, который умеет жить отдельным ресурсом (.sageui). Пока перевод лежал внутри сериализатора сцен,
// второму месту оставалось повторить его у себя — то есть завести второе
// описание одного формата, которое разойдётся с первым на третьем добавленном
// типе значения, и молча.
// ---------------------------------------------------------------------------
namespace sage::scene {

nlohmann::json ValueToJson(const sage::vars::Value& v);
sage::vars::Value ValueFromJson(const nlohmann::json& j);

nlohmann::json VarsToJson(const sage::vars::Table& table);
void VarsFromJson(const nlohmann::json& in, sage::vars::Table& table);

nlohmann::json LinksToJson(const std::vector<sage::signals::Link>& links);
void LinksFromJson(const nlohmann::json& in, std::vector<sage::signals::Link>& out);

// Связи СТАРОГО формата (список "events" у части interactable, до сигналов):
// «когда триггер — послать событие и/или позвать Target.Method». Переводятся в
// связи сигналов ДОБАВЛЕНИЕМ к out: кнопка из старой сцены обязана делать то
// же, что делала, а не молча перестать работать.
void LinksFromLegacyBindings(const nlohmann::json& in, std::vector<sage::signals::Link>& out);

} // namespace sage::scene
