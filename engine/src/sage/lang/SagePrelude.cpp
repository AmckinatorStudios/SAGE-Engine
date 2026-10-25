#include "sage/lang/SageCompiler.h"
#include "sage/core/StaticText.h"
#include "sage/lang/SagePreludeCore.h"
#include "sage/lang/SagePreludeRuntime.h"

// ---------------------------------------------------------------------------
// SAGE Lua: RUNTIME.
//
// То, на чём исполняется сгенерированный код: классы и наследование,
// перечисления и структуры, компоненты и их жизненный цикл, события,
// планировщик задач (async/await, таймеры), Result, обёртки API движка в
// стиле SAGE (Input.isDown, Scene.createEntity, Log.info…).
//
// Написан на Lua, а не на C++, потому что почти весь — про таблицы и
// метатаблицы Lua: на C++ то же самое было бы втрое длиннее и всё равно
// работало бы через стек Lua. Обращения к движку идут через уже существующие
// разделы (Input, Scene, Debug, Physics…) — второй привязки к C++ здесь нет.
//
// Ставится ОДИН раз на состояние Lua (глобальная таблица __sage). Файл .slua
// видит обёртки локальными именами (см. шапку генератора), поэтому обычные
// Lua-скрипты рядом продолжают видеть прежние Input/Scene/Time как были.
// ---------------------------------------------------------------------------
namespace sage::lang {

const char* PreludeSource() {
    static constexpr auto kText = sage::text::Concat(kPreludeCore, kPreludeRuntime);
    return kText.data();
}

} // namespace sage::lang
