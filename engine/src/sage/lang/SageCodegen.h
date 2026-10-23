#pragma once
#include <string>

#include "sage/lang/SageAst.h"
#include "sage/lang/SageCheck.h"

namespace sage::lang {

struct CodegenResult {
    std::string Lua;
    std::string MainClass;   // компонент, который файл отдаёт движку (пусто — нет)
    bool HasExports = false;
};

// Превращает проверенное дерево в Lua. Код каждого оператора ложится на ТУ
// ЖЕ строку, что и в исходнике: ошибка Lua во время игры («Player.slua:24:
// attempt to …») указывает на строку .slua без отдельной карты строк.
CodegenResult Generate(const BlockPtr& ast, const ProgramInfo& info, const std::string& path);

} // namespace sage::lang
