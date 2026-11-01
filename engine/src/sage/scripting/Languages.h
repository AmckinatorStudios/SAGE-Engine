#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/scripting/LanguageBackend.h"
#include "sage/scripting/ScriptLint.h"
#include "sage/vars/Table.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// РЕЕСТР ЯЗЫКОВ — всё, что движок и редактор знают о языке скриптов, в одной
// записи.
//
// Что было. Исполнение языка уже было сменной деталью (LanguageBackend), а
// ВСЁ ОСТАЛЬНОЕ — нет: поля для инспектора, методы для связей сигналов,
// проверка, слот ассета «скрипт», значок в браузере файлов и подключение к
// Play-режиму и к плееру выбирали язык цепочками `if (ext == ".lua")`.
// Второй язык означал правку каждого из этих мест.
//
// Что теперь. Язык — запись: расширения, фабрика исполнения и инструменты
// разбора БЕЗ запуска (поля, методы, проверка). Хосты
// подключают все зарегистрированные языки одним вызовом (AttachLanguages),
// инструменты спрашивают реестр. Новый язык — одна регистрация: из C++
// (плагин, игра) или файлом проекта scripting/languages.json для языков во
// внешнем процессе (sage/scripting/ProcessLanguage.h).
//
// Lua и SAGE Lua регистрируются здесь же, как обычные записи.
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Что хост даёт языкам при подключении. Lua работает на состоянии прежнего
// движка (на нём весь API), остальным языкам он даёт универсальные вызовы
// API (ScriptEngine::Api()).
struct LanguageHost {
    ScriptEngine* Engine = nullptr;
    std::filesystem::path ProjectDir;
};

struct Language {
    std::string Id;                       // "lua", "sage-lua", "python"
    std::string Name;                     // для людей: "Lua", "SAGE Lua"
    std::vector<std::string> Extensions;  // в нижнем регистре, с точкой

    // Исполнение. Пусто — файлы языка исполняет бэкенд языка RunsOn (так SAGE
    // Lua — компилятор в Lua — идёт в Lua-бэкенд).
    std::function<std::unique_ptr<LanguageBackend>(const LanguageHost&)> CreateBackend;
    std::string RunsOn;

    // Разбор БЕЗ запуска (инспектор показывает поля, пока игра не запущена).
    // Пусто — у языка нет такого инструмента, и это не ошибка.
    std::function<sage::vars::Table(const std::string& path, const std::string& source)> Fields;
    std::function<std::vector<std::string>(const std::string& path, const std::string& source)> Methods;
    std::function<ScriptLintResult(const std::string& path, const std::string& source)> Check;

    std::string Origin = "engine";        // "engine", "project", "plugin:<имя>"
};

// Зарегистрировать язык. false и причина — имя или расширение уже заняты
// другим языком (тот остаётся в силе). Повтор той же записи заменяет её.
bool RegisterLanguage(Language language, std::string* err = nullptr);
bool UnregisterLanguage(const std::string& id);
int UnregisterLanguages(const std::string& origin);

// Копии записей — реестр могут дописывать из другого потока.
std::vector<Language> Languages();
bool FindLanguage(const std::string& id, Language& out);
bool LanguageForPath(const std::string& path, Language& out);
bool IsScriptPath(const std::string& path);
std::vector<std::string> ScriptExtensions();

// Подключить к системе скриптов исполнение всех зарегистрированных языков.
// Возвращает, сколько бэкендов создано; сбой одной фабрики — в журнал, не
// срывает остальные.
class ScriptingSystem;
int AttachLanguages(ScriptingSystem& system, const LanguageHost& host);

} // namespace sage::scripting
