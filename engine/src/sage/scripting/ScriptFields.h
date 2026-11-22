#pragma once
#include <string>
#include <vector>

#include "sage/vars/Table.h"

// ---------------------------------------------------------------------------
// ПУБЛИЧНЫЕ ПОЛЯ СКРИПТА БЕЗ ЗАПУЩЕННОЙ ИГРЫ.
//
// Инспектор обязан показать переменные скрипта, когда игра НЕ идёт: рантайма
// нет, бэкендов нет, сцена — документ. Поэтому разбор объявления доступен
// отдельной функцией, не требующей ничего, кроме текста файла.
//
// Язык выбирается по расширению — ровно так же, как его выбирает ScriptRuntime.
// Новый язык дописывает сюда одну строку; больше редактор о языках не знает
// ничего.
// ---------------------------------------------------------------------------
namespace sage::scripting {

sage::vars::Table ParseFields(const std::string& path, const std::string& source);

// Найденная при разборе ошибка объявления поля (например, неверная аннотация
// `---@export_range` в Lua). Строка 1-based; Error=false — предупреждение.
struct FieldDiagnostic {
    int Line = 0;
    bool Error = true;
    std::string Message;
};

// Ошибки объявления полей — тем же разбором, без запуска. Пусто, если язык
// ошибок объявления не знает или их нет.
std::vector<FieldDiagnostic> ParseFieldProblems(const std::string& path, const std::string& source);

// Разбор ошибок объявления + их показ: в консоль и во вкладку «Проблемы»
// (Category "Script", Code "script.annotation"). Дёшево зовётся каждый кадр:
// файл с тем же текстом повторно не сообщается, а исправленный — снимается.
void ReportFieldProblems(const std::string& path, const std::string& source, bool force = false);

// Методы, которые скрипт объявляет, — для выбора «Target.Method» в связи
// инспектора. Тоже ТЕКСТОМ: связь настраивают, пока игра не запущена. Хуки
// жизненного цикла (Start, Update, OnCollisionEnter…) в список не входят —
// звать их связью значит ломать порядок кадра, который им обещан.
std::vector<std::string> ParseMethods(const std::string& path, const std::string& source);

// Сигналы, которые скрипт шлёт своему или чужому объекту (`self:emit("hit")`,
// `door:emit('opened', data)`), — подсказки для списка событий инспектора: такие
// события движок заранее не знает, но по тексту скриптов проекта их видно.
// Глобальные `Events.emit(...)` сюда не входят: связь слушает сигнал ОБЪЕКТА.
// Порядок — порядок первого появления, без повторов. Язык не важен: и Lua, и
// SAGE Lua пишут вызов одинаково.
std::vector<std::string> ParseEmittedSignals(const std::string& source);

// События, которые скрипт ОБЪЯВЛЯЕТ (`events.create(object, "player_died")`,
// прежнее `self:define_event("Died")`), — по тексту,
// как и методы: инспектор показывает их до запуска игры и без вызова emit.
std::vector<std::string> ParseDefinedEvents(const std::string& source);

// Методы Lua-файла по тексту — инструмент языка Lua в реестре языков.
std::vector<std::string> ParseLuaMethods(const std::string& source);

} // namespace sage::scripting
