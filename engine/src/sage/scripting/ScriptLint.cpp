#include "sage/scripting/ScriptLint.h"

#include <fstream>
#include <sstream>

#include "sage/core/Problems.h"
#include "sage/lang/SageCompiler.h"

namespace sage::scripting {

namespace fs = std::filesystem;

ScriptLintResult LintScriptSource(const std::string& file, const std::string& source) {
    ScriptLintResult res;
    res.Files = 1;
    // Прежние находки файла сняты до проверки: исправленное не висит в списке.
    sage::Problems::Resolve(file, "script.lint");
    sage::lang::CompileOptions options;
    options.Loader = sage::lang::ProjectModuleLoader();
    options.Lint = true;
    const sage::lang::CompileResult r = sage::lang::Compile(file, source, options);
    for (const sage::lang::Diagnostic& d : r.Diagnostics) {
        sage::Problem p;
        p.Level = d.IsError() ? sage::Severity::Error : sage::Severity::Warning;
        (d.IsError() ? res.Errors : res.Warnings)++;
        p.Category = "Script";
        p.Code = "script.lint";
        p.Title = d.Title;
        p.File = file;
        p.Resource = file;
        p.Line = d.P.Line;
        p.Col = d.P.Col;
        // Первая строка — что не так, остальные — как исправить.
        if (!d.Lines.empty()) p.Detail = d.Lines.front();
        for (size_t k = 1; k < d.Lines.size(); ++k) p.Hints.push_back(d.Lines[k]);
        p.Excerpt = sage::Problems::Excerpt(source, d.P.Line, d.P.Col);
        p.Technical = d.Format();
        sage::Problems::Report(std::move(p));
    }
    return res;
}

ScriptLintResult LintScripts(const fs::path& root) {
    ScriptLintResult total;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if (it->is_directory(ec)) {
            if (name.empty() || name[0] == '.' || name == "Builds") it.disable_recursion_pending();
            continue;
        }
        const std::string ext = p.extension().string();
        // sage.lua — подсказка для редактора кода, а не скрипт игры.
        if ((ext != ".lua" && ext != ".slua") || name == "sage.lua") continue;
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        const ScriptLintResult one = LintScriptSource(p.lexically_relative(root).generic_string(), ss.str());
        total.Files += one.Files;
        total.Errors += one.Errors;
        total.Warnings += one.Warnings;
    }
    return total;
}

} // namespace sage::scripting
