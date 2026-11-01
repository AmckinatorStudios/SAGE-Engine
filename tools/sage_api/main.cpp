// ---------------------------------------------------------------------------
// sage_api — каталог API скриптов (sage/lang/ApiRegistry.h) в файл.
//
//   sage_api --json <файл>    весь каталог: имена, псевдонимы, типы, описания
//   sage_api --lua <файл>     подсказки LuaLS/EmmyLua
//   sage_api --conflicts      конфликты имён (код возврата 1, если есть)
//
// Каталог наполняет само состояние скриптов, поэтому типы в выводе — те, что
// выведены компилятором из сигнатур C++, а не угаданные по тексту. Им
// пользуется scripts/gen_script_api.py: подсказка и таблица проверки типов
// берут типы отсюда.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "sage/lang/ApiRegistry.h"
#include "sage/scripting/ScriptEngine.h"

int main(int argc, char** argv) {
    ScriptEngine engine;   // регистрирует весь API и пишет его в каталог
    const sage::lang::ApiRegistry& api = sage::lang::ApiRegistry::Instance();
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--json" || arg == "--lua") && i + 1 < argc) {
            std::ofstream out(argv[++i], std::ios::binary);
            if (!out) {
                std::fprintf(stderr, "sage_api: не открыть %s\n", argv[i]);
                return 2;
            }
            out << (arg == "--json" ? api.ToJson() : api.ToLuaHints());
        } else if (arg == "--conflicts") {
            for (const std::string& c : api.Conflicts()) {
                std::printf("%s\n", c.c_str());
                rc = 1;
            }
        } else {
            std::fprintf(stderr, "использование: sage_api [--json файл] [--lua файл] [--conflicts]\n");
            return 2;
        }
    }
    return rc;
}
