#pragma once
#include <string>

#include "ScriptApiCommon.h"
#include "sage/ai/World.h"

// ---------------------------------------------------------------------------
// Общее для привязок SAGE AI к Lua (ScriptApi_AI*.cpp): ручка выполнения,
// прокси переменных и перевод значений. Внутренний заголовок — рядом с
// реализацией, игре его включать незачем.
// ---------------------------------------------------------------------------
namespace sage::scripting::aidetail {

struct AiRunRef {
    sage::ai::RunPtr R;
};
struct AiVars {
    sage::ai::BlackboardPtr B;
};

// Значение Lua -> значение ИИ: объект (старый, новый API, экземпляр self),
// таблица (список или структура), простое значение. what — для ошибки.
sage::vars::Value AiValue(ScriptEngine& se, const sol::object& o, const std::string& what);
// Номер объекта-хозяина из чего угодно, чем скрипт обозначает объект.
int OwnerId(const sol::object& o);

} // namespace sage::scripting::aidetail
