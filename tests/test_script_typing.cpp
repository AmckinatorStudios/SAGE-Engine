#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "sage/lang/SageApi.h"
#include "sage/lang/SageCompiler.h"

// Проверка скриптов знает сигнатуры API движка (таблица из Bind, SageApi.h):
// неверный тип или лишний аргумент виден до запуска игры, с файлом и строкой.
// Строгий режим ещё и ловит опечатки в именах — с «возможно, имелось в виду».

namespace {

sage::lang::CompileResult Lint(const std::string& path, const std::string& src) {
    sage::lang::CompileOptions o;
    o.Lint = true;
    return sage::lang::Compile(path, src, o);
}

size_t Count(const sage::lang::CompileResult& r, bool errors) {
    size_t n = 0;
    for (const auto& d : r.Diagnostics) n += d.IsError() == errors ? 1 : 0;
    return n;
}

bool Mentions(const sage::lang::CompileResult& r, const std::string& text) {
    for (const auto& d : r.Diagnostics)
        if (d.Format().find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST(ScriptTyping_api_table_knows_engine_functions) {
    const sage::lang::ApiFunction* f = sage::lang::FindApiFunction("sage.noise.Perlin");
    CHECK_TRUE(f != nullptr);
    CHECK_TRUE(sage::lang::FindApiFunction("noise.Perlin") != nullptr);   // короткое имя модуля
    CHECK_TRUE(sage::lang::FindApiFunction("perlin") != nullptr);         // слой стиля SAGE
    CHECK_TRUE(sage::lang::FindApiFunction("math.Clamp") == nullptr);     // math — всегда Lua
    CHECK_TRUE(sage::lang::IsKnownGlobal("Vec3"));
    CHECK_TRUE(sage::lang::IsKnownGlobal("ipairs"));
    CHECK_FALSE(sage::lang::IsKnownGlobal("perlni"));
}

TEST(ScriptTyping_wrong_argument_type_to_an_api_function_is_an_error) {
    auto r = sage::lang::Compile("a.lua", "local h = perlin(\"x\")\n");
    CHECK_FALSE(r.Ok);
    CHECK_TRUE(Mentions(r, "a.lua:1"));
    CHECK_TRUE(Mentions(r, "perlin"));
    // Полное имя модуля проверяется так же.
    auto full = sage::lang::Compile("b.lua", "local n = sage.noise.Perlin(1, \"y\")\n");
    CHECK_FALSE(full.Ok);
    CHECK_TRUE(Mentions(full, "Perlin"));
}

TEST(ScriptTyping_wrong_argument_count_is_an_error) {
    auto many = sage::lang::Compile("a.lua", "local h = perlin(1, 2, 3, 4)\n");
    CHECK_FALSE(many.Ok);
    CHECK_TRUE(Mentions(many, "передано 4"));
    auto few = sage::lang::Compile("b.lua", "local n = sage.noise.Perlin(1)\n");
    CHECK_FALSE(few.Ok);
    CHECK_TRUE(Mentions(few, "не хватает аргумента y"));
}

TEST(ScriptTyping_return_type_of_an_api_function_flows_into_the_script) {
    // perlin возвращает число, а не вектор: .x у него — ошибка до запуска.
    auto r = sage::lang::Compile("a.lua", "local h = perlin(1).x\n");
    CHECK_FALSE(r.Ok);
    CHECK_TRUE(Mentions(r, "нет членов"));
}

TEST(ScriptTyping_correct_calls_and_lenient_types_pass) {
    auto r = sage::lang::Compile("a.lua",
                                 "local h = perlin(1, 2) + remap(0.5, 0, 1, 10, 20)\n"
                                 "local m = grid(8, 8, 16, 16, function(x, z) return x + z end)\n"
                                 "log(\"h = \" .. h)\n"
                                 "log(42)\n");   // число туда, где ждут строку, Lua склеит
    CHECK_TRUE(r.Ok);
    CHECK_EQ(r.Diagnostics.size(), size_t(0));
}

TEST(ScriptTyping_script_own_names_win_over_the_api) {
    // Своя функция perlin (даже объявленная НИЖЕ места вызова) и своя
    // таблица noise — не функции движка: их сигнатура не навязывается.
    auto own = sage::lang::Compile("a.lua",
                                   "function Update() return perlin(\"seed\") end\n"
                                   "function perlin(s) return #s end\n");
    CHECK_TRUE(own.Ok);
    auto local = sage::lang::Compile("b.lua", "local noise = {Perlin = function(s) end}\nnoise.Perlin(\"x\")\n");
    CHECK_TRUE(local.Ok);
}

TEST(ScriptTyping_lint_warns_about_a_typo_and_suggests_the_name) {
    auto r = Lint("a.lua",
                  "local speed = 5\n"
                  "function Update(dt)\n"
                  "    local h = perlni(dt)\n"
                  "    return h * sped\n"
                  "end\n");
    CHECK_TRUE(r.Ok);   // предупреждения не мешают запуску
    CHECK_EQ(Count(r, false), size_t(2));
    CHECK_TRUE(Mentions(r, "a.lua:3"));
    CHECK_TRUE(Mentions(r, "возможно, имелось в виду perlin"));
    CHECK_TRUE(Mentions(r, "возможно, имелось в виду speed"));
    // Без строгого режима — молчит: загрузка игры не шумит.
    auto quiet = sage::lang::Compile("a.lua", "local h = perlni(1)\n");
    CHECK_EQ(quiet.Diagnostics.size(), size_t(0));
}

TEST(ScriptTyping_lint_accepts_globals_the_script_sets_and_engine_names) {
    auto r = Lint("a.lua",
                  "score = 0\n"
                  "function OnHit() score = score + 1 end\n"
                  "local v = Vec3.new(1, 2, 3)\n"
                  "for i, x in ipairs({1, 2}) do print(i, x, math.max(i, x)) end\n"
                  "if Input:IsKeyPressed(\"SPACE\") then OnHit() end\n");
    CHECK_EQ(r.Diagnostics.size(), size_t(0));
}

TEST(ScriptTyping_every_shipped_script_is_clean_in_strict_mode) {
    // Ложное срабатывание хуже молчания: проверку, которая ругается на
    // рабочий код, перестают читать. Все скрипты движка и шаблонов — чистые.
    namespace fs = std::filesystem;
    const fs::path root = fs::path(SAGE_TEST_DATA_DIR) / ".." / "..";
    size_t files = 0;
    for (const char* dir : {"editor/assets", "editor/plugins", "tools"}) {
        if (!fs::exists(root / dir)) continue;
        for (const auto& it : fs::recursive_directory_iterator(root / dir)) {
            const std::string ext = it.path().extension().string();
            if ((ext != ".lua" && ext != ".slua") || it.path().filename() == "sage.lua") continue;
            std::ifstream in(it.path());
            std::stringstream ss;
            ss << in.rdbuf();
            auto r = Lint(it.path().filename().string(), ss.str());
            ++files;
            for (const auto& d : r.Diagnostics) {
                if (d.Format().find("Устаревший") != std::string::npos) continue;   // пример deprecated
                std::printf("       %s\n", d.Format().c_str());
                CHECK_TRUE(false);
            }
        }
    }
    CHECK_TRUE(files > 20);
}

#include "sage/core/Problems.h"
#include "sage/scripting/ScriptLint.h"

TEST(ScriptTyping_check_scripts_reports_to_problems_and_clears_fixed_ones) {
    namespace fs = std::filesystem;
    sage::Problems::Clear();
    const fs::path dir = fs::temp_directory_path() / "sage_lint_project";
    fs::remove_all(dir);
    fs::create_directories(dir / "Assets" / "Scripts");
    fs::create_directories(dir / ".sage");
    auto write = [](const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; };
    write(dir / "Assets/Scripts/enemy.lua", "function Update(dt)\n    local h = perlin(\"x\") + perlni(dt)\nend\n");
    write(dir / "Assets/Scripts/ok.lua", "function Update(dt) return perlin(dt) end\n");
    write(dir / ".sage/cache.lua", "perlin(\"служебное — не проверять\")\n");

    auto r = sage::scripting::LintScripts(dir);
    CHECK_EQ(r.Files, 2);
    CHECK_EQ(r.Errors, 1);
    CHECK_EQ(r.Warnings, 1);
    bool sawTypo = false, sawType = false;
    for (const sage::Problem& p : sage::Problems::Snapshot()) {
        if (p.Code != "script.lint") continue;
        CHECK_EQ(p.File, std::string("Assets/Scripts/enemy.lua"));
        CHECK_EQ(p.Line, 2);
        CHECK_FALSE(p.Excerpt.empty());
        if (p.Level == sage::Severity::Warning)
            for (const std::string& h : p.Hints) sawTypo |= h.find("perlin") != std::string::npos;
        if (p.Level == sage::Severity::Error) sawType = true;
    }
    CHECK_TRUE(sawTypo);
    CHECK_TRUE(sawType);

    // Исправил — проверил снова: находки файла исчезли.
    write(dir / "Assets/Scripts/enemy.lua", "function Update(dt)\n    return perlin(dt)\nend\n");
    r = sage::scripting::LintScripts(dir);
    CHECK_EQ(r.Errors + r.Warnings, 0);
    int left = 0;
    for (const sage::Problem& p : sage::Problems::Snapshot()) left += p.Code == "script.lint";
    CHECK_EQ(left, 0);
    fs::remove_all(dir);
    sage::Problems::Clear();
}
