#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sage/scripting/Languages.h"

// ---------------------------------------------------------------------------
// ЯЗЫК ВО ВНЕШНЕМ ПРОЦЕССЕ — любой интерпретатор, рантайм или свой язык игры
// без единой строки C++.
//
// Язык описывается в файле проекта scripting/languages.json:
//
//   {"languages": [{
//       "id": "python", "name": "Python", "extensions": [".py"],
//       "command": ["python3", "{project}/scripting/sage_python.py"],
//       "call_timeout": 2.0
//   }]}
//
// Движок запускает команду один раз на систему скриптов и разговаривает с ней
// строками JSON через stdin/stdout (одна строка — одно сообщение). Значения —
// тот же JSON, что у сцены: {"kind":"vec3","value":{"x":..}} (vars/ValueJson.h).
//
// Движок -> процесс (на каждый запрос ровно один ответ):
//   {"op":"fields","path","source"}           -> {"fields":{"имя":значение}}
//   {"op":"create","id","path","source","entity"}
//                                             -> {"ok":true,"hooks":[..],"methods":[..]}
//   {"op":"set_fields","id","fields":{..}}    -> {"ok":true}
//   {"op":"call","id","hook","dt"|"other"|"name"} -> {"ok":true}
//   {"op":"invoke","id","method","args":[..]} -> {"ok":true,"results":[..]}
//   {"op":"destroy","id"} / {"op":"reset"}    -> {"ok":true}
//   ошибка — {"ok":false,"error":"текст","line":N}
//
// Процесс -> движок (пока движок ждёт ответа):
//   {"api":"sage.math.Clamp","args":[..]}     -> {"results":[..]} | {"error":".."}
//   {"log":"строка"}                          -> (без ответа)
//
// API движка процессу доступно ЦЕЛИКОМ через каталог (sage/lang/ApiRegistry.h,
// ScriptEngine::Api()): всё, что зарегистрировано переносимым, — по полному
// имени или псевдониму, без привязок под этот язык.
//
// БЕЗОПАСНОСТЬ И РЕСУРСЫ. Чужой процесс не может уронить движок: ответ ждётся
// не дольше call_timeout (зависший процесс завершается, ошибка — в консоль
// скриптов), падение процесса — ошибка вызова, следующий скрипт запускает его
// заново. Команду задаёт только файл проекта — то же доверие, что к его
// скриптам.
// ---------------------------------------------------------------------------
namespace sage::scripting {

struct ProcessLanguageSpec {
    std::string Id;
    std::string Name;
    std::vector<std::string> Extensions;
    std::vector<std::string> Command;     // {project} — папка проекта
    std::filesystem::path ProjectDir;
    double StartTimeout = 10.0;           // запуск и первый ответ, секунды
    double CallTimeout = 2.0;             // один вызов, секунды
};

std::unique_ptr<LanguageBackend> MakeProcessBackend(const ProcessLanguageSpec& spec, const LanguageHost& host);

// Описание языка реестра для такого процесса (исполнение и поля инспектора).
Language MakeProcessLanguage(const ProcessLanguageSpec& spec);

// Прочитать scripting/languages.json проекта и зарегистрировать его языки
// (источник "project"; прежние языки проекта снимаются). Нет файла — ноль и
// не ошибка. problems — по строке на каждую ошибку описания.
int LoadProjectLanguages(const std::filesystem::path& projectDir, std::vector<std::string>* problems = nullptr);

} // namespace sage::scripting
