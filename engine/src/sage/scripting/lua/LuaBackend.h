#pragma once
#include <memory>

#include "sage/scripting/LanguageBackend.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// LUA — ПЕРВЫЙ ЯЗЫК, А НЕ ЧАСТЬ ЯДРА.
//
// Всё, что знает про Lua и sol2, живёт за этим заголовком. Движок берёт бэкенд
// одной строкой и дальше разговаривает с ним через LanguageBackend:
//
//      scripting.AddBackend(sage::scripting::MakeLuaBackend({ &legacyEngine }));
//
// СОВМЕСТИМОСТЬ — ДЕТАЛЬ БЭКЕНДА, А НЕ ЯДРА. Старый ScriptEngine (sage.*,
// глобальные Vec3/Spawn/Find, уровневые скрипты, таймеры, корутины) — это
// набор привязок ЯЗЫКА LUA. Поэтому он подключается сюда, в бэкенд языка, а не
// в архитектуру: новый Lua-скрипт пишется в новом стиле и видит весь прежний
// API как обычные глобальные имена, а движок ни в одном месте про них не
// знает. Interop == nullptr — чистое состояние Lua только с новым API (так
// работают тесты и так будет работать любой второй язык).
// ---------------------------------------------------------------------------
namespace sage::scripting {

struct LuaBackendConfig {
    // Состояние Lua, на котором уже зарегистрирован прежний API движка.
    // nullptr — бэкенд заводит своё чистое состояние.
    ScriptEngine* Interop = nullptr;
};

std::unique_ptr<LanguageBackend> MakeLuaBackend(const LuaBackendConfig& config = {});

} // namespace sage::scripting
