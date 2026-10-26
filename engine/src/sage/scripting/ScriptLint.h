#pragma once
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// «ПРОВЕРИТЬ СКРИПТЫ» — строгая проверка всех скриптов проекта до запуска.
//
// Загрузка игры проверяет .slua и молчит об опечатках: предупреждение на
// каждый Play — шум, который перестают читать. Здесь — по просьбе и целиком:
// каждый .lua и .slua проходит проверку типов с сигнатурами API движка
// (sage/lang/SageApi.h) и строгим режимом — perlni(...) получает «возможно,
// имелось в виду perlin», perlin("x") — ошибку типа с файлом и строкой.
//
// Находки идут во вкладку «Проблемы» (Category "Script", Code "script.lint")
// с фрагментом кода и подсказкой. Повторная проверка сначала снимает прежние
// находки файла — исправленное исчезает из списка само.
// ---------------------------------------------------------------------------
namespace sage::scripting {

struct ScriptLintResult {
    int Files = 0;
    int Errors = 0;
    int Warnings = 0;
};

// Один файл: file — имя для сообщений (как его видит проект).
ScriptLintResult LintScriptSource(const std::string& file, const std::string& source);
// Все скрипты папки, кроме служебных (".sage", ".git") и собранных игр (Builds).
ScriptLintResult LintScripts(const std::filesystem::path& root);

} // namespace sage::scripting
