#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sage/lang/SageCheck.h"
#include "sage/lang/SageDiagnostics.h"
#include "sage/vars/Table.h"

// ---------------------------------------------------------------------------
// SAGE Lua: КОМПИЛЯТОР — вход в конвейер.
//
//     исходник .slua
//        -> лексер (SageLexer)       токены
//        -> парсер (SageParser)      дерево (SageAst)
//        -> проверка (SageCheck)     типы, классы, аннотации, импорты
//        -> генератор (SageCodegen)  Lua на тех же строках, что исходник
//        -> runtime (SagePrelude)    классы, события, задачи, API движка
//
// Компилирует движок сам, при загрузке скрипта: отдельного шага сборки у
// человека нет — сохранил файл .slua, и Play подхватил его, как и .lua.
// ---------------------------------------------------------------------------
namespace sage::lang {

struct CompileOptions {
    ModuleLoader Loader;   // import из других файлов; пусто — только модули SAGE/*
};

struct CompileResult {
    bool Ok = false;
    std::string Lua;
    std::string MainClass;
    std::vector<Diagnostic> Diagnostics;
    ProgramInfo Program;
    std::shared_ptr<ModuleCache> Modules;

    const Diagnostic* FirstError() const {
        for (const Diagnostic& d : Diagnostics)
            if (d.IsError()) return &d;
        return nullptr;
    }
    // Описание классов файла — то, что видит редактор (поля, типы, метаданные).
    std::string DescribeClasses() const;
};

CompileResult Compile(const std::string& path, const std::string& source, const CompileOptions& options = {});

// То же с памятью: повторная компиляция того же текста отдаёт готовый
// результат. Скрипт компонента компилируется на КАЖДЫЙ экземпляр — сотня
// врагов с одним скриптом не должна сто раз гонять проверку типов.
std::shared_ptr<const CompileResult> CompileCached(const std::string& path, const std::string& source,
                                                   const CompileOptions& options = {});
void ClearCompileCache();

// Поля инспектора главного компонента файла: @field(editable = true).
sage::vars::Table FieldsOf(const CompileResult& r);
// Публичные методы главного компонента — для связей сигналов в инспекторе.
std::vector<std::string> MethodsOf(const CompileResult& r);

// Lua-код runtime SAGE (ставится один раз на состояние Lua).
const char* PreludeSource();

// Поиск модуля для import "Game/Player": относительно файла, затем по
// каталогам поиска (assets/scripts, Scripts, Modules, корень).
bool ResolveModulePath(const std::string& spec, const std::string& fromFile,
                       const std::vector<std::string>& searchDirs, std::string& resolved);

// Корни поиска модулей проекта (папка открытого проекта). Ставит система
// скриптинга при смене проекта; пусто — только папка файла и рабочий каталог.
void SetModuleRoots(const std::vector<std::string>& roots);
// Загрузчик import для движка: ищет и читает через vfs — в собранной игре
// модули лежат в .sagepak, а не на диске.
ModuleLoader ProjectModuleLoader();
// Компиляция файла так, как её делает движок: с памятью и загрузчиком проекта.
std::shared_ptr<const CompileResult> CompileForEngine(const std::string& path, const std::string& source);

} // namespace sage::lang
