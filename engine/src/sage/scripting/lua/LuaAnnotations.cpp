#include "sage/scripting/lua/LuaInternal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// АННОТАЦИИ INSPECTOR: `---@export`, `---@export_range 0 20`, `---@header`…
//
//     ---@header "Player Settings"
//     ---@group "Movement"
//     ---@export_range 0 20
//     ---@tooltip "Movement speed of the player"
//     move_speed = 5.0
//     ---@end_group
//
// РАЗБОР ТЕКСТОМ, не запуском файла (причины — в начале LuaFields.cpp):
// инспектор показывает поля до запуска игры. Аннотация — обычный комментарий
// Lua, поэтому файл с ними запускается как раньше, а редактор кода видит
// `---@` как свои (LuaLS) и не ругается.
//
// СВЯЗЫВАНИЕ. Аннотации переменной — подряд идущие строки комментариев прямо
// над ней: между ними и переменной нет ни пустой строки, ни кода. Так
// аннотация, оторванная правкой от переменной, не прилипнет к соседней молча,
// а будет названа ошибкой. `header`, `group`, `end_group` ни к чему не
// привязаны — действуют на все переменные ниже себя.
//
// Незнакомые `---@имя` (LuaLS: type, param, class…) не трогаются; ошибкой
// считается только неизвестное `---@export_…` — опечатка в нашем имени.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

using sage::scripting::FieldDiagnostic;
using sage::vars::Kind;
using sage::vars::Value;
using sage::vars::Var;

struct Arg {
    bool IsString = false;
    bool IsNumber = false;
    std::string Text;
    double Number = 0.0;
};

struct Annotation {
    int Line = 0;           // 1-based
    std::string Name;       // без "---@"
    std::vector<Arg> Args;
    bool Consumed = false;
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> SplitLines(const std::string& src) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= src.size()) {
        size_t end = src.find('\n', at);
        if (end == std::string::npos) end = src.size();
        out.push_back(src.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

bool IsKnown(const std::string& n) {
    static const char* kNames[] = {"export", "export_range", "export_enum", "export_color", "export_object",
                                   "export_resource", "tooltip", "header", "group", "end_group"};
    for (const char* k : kNames)
        if (n == k) return true;
    return false;
}
bool IsStandalone(const std::string& n) { return n == "header" || n == "group" || n == "end_group"; }
bool IsExport(const std::string& n) { return n.compare(0, 6, "export") == 0 && n != "export_"; }

// `---@имя арг арг…` -> Annotation. Аргументы: "строки" (с \" внутри), числа,
// слова.
bool ParseAnnotation(const std::string& line, int number, Annotation& out) {
    const std::string t = Trim(line);
    if (t.compare(0, 4, "---@") != 0) return false;
    size_t i = 4;
    const size_t nameStart = i;
    while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) ++i;
    out.Line = number;
    out.Name = t.substr(nameStart, i - nameStart);
    out.Args.clear();
    while (i < t.size()) {
        while (i < t.size() && IsSpace(t[i])) ++i;
        if (i >= t.size()) break;
        if (t.compare(i, 2, "--") == 0) break;   // хвостовой комментарий
        Arg a;
        if (t[i] == '"' || t[i] == '\'') {
            const char q = t[i++];
            a.IsString = true;
            while (i < t.size() && t[i] != q) {
                if (t[i] == '\\' && i + 1 < t.size()) ++i;
                a.Text += t[i++];
            }
            if (i < t.size()) ++i;
        } else {
            const size_t s = i;
            while (i < t.size() && !IsSpace(t[i])) ++i;
            a.Text = t.substr(s, i - s);
            char* end = nullptr;
            a.Number = std::strtod(a.Text.c_str(), &end);
            a.IsNumber = end && *end == '\0' && !a.Text.empty();
        }
        out.Args.push_back(std::move(a));
    }
    return true;
}

void Report(std::vector<FieldDiagnostic>* d, int line, bool error, std::string text) {
    if (!d) return;
    FieldDiagnostic f;
    f.Line = line;
    f.Error = error;
    f.Message = std::move(text);
    d->push_back(std::move(f));
}

std::string Quote(const std::string& s) { return "'" + s + "'"; }

} // namespace

bool IsStandardStyleSource(const std::string& source) {
    static const char* kHooks[] = {"start", "update", "fixed_update", "late_update",
                                   "on_enable", "on_disable", "on_destroy"};
    size_t at = 0;
    while (at < source.size()) {
        size_t end = source.find('\n', at);
        if (end == std::string::npos) end = source.size();
        const std::string line = Trim(source.substr(at, end - at));
        at = end + 1;
        if (line.compare(0, 4, "---@") == 0) {
            Annotation a;
            if (ParseAnnotation(line, 0, a) && (IsKnown(a.Name) || IsExport(a.Name))) return true;
            continue;
        }
        if (line.compare(0, 9, "function ") == 0) {
            size_t i = 9;
            while (i < line.size() && IsSpace(line[i])) ++i;
            size_t j = i;
            while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_')) ++j;
            const std::string name = line.substr(i, j - i);
            while (j < line.size() && IsSpace(line[j])) ++j;
            if (j < line.size() && line[j] == '(')
                for (const char* h : kHooks)
                    if (name == h) return true;
        }
    }
    return false;
}

void ApplyAnnotations(const std::string& source, const std::vector<TopLevelAssign>& assigns,
                      sage::vars::Table& out, std::vector<FieldDiagnostic>* diag) {
    const std::vector<std::string> lines = SplitLines(source);

    // Все аннотации файла.
    std::vector<Annotation> anns;
    for (size_t i = 0; i < lines.size(); ++i) {
        Annotation a;
        if (!ParseAnnotation(lines[i], (int)i + 1, a)) continue;
        if (!IsKnown(a.Name)) {
            if (IsExport(a.Name))
                Report(diag, a.Line, true,
                       "неизвестная аннотация @" + a.Name +
                           ": есть export, export_range, export_enum, export_color, export_object, "
                           "export_resource, tooltip, header, group, end_group");
            continue;   // чужие аннотации (LuaLS) не трогаем
        }
        anns.push_back(std::move(a));
    }

    // Заголовки и группы — отдельным проходом по порядку строк.
    struct Layout { int Line; std::string Header; std::vector<std::string> Groups; };
    std::vector<Layout> states;   // состояние ПОСЛЕ каждой standalone-аннотации
    {
        std::string header;
        std::vector<std::string> groups;
        std::vector<int> groupLines;
        for (Annotation& a : anns) {
            if (!IsStandalone(a.Name)) continue;
            a.Consumed = true;
            if (a.Name == "end_group") {
                if (!a.Args.empty()) Report(diag, a.Line, false, "@end_group без аргументов");
                if (groups.empty()) Report(diag, a.Line, true, "@end_group без открытой @group");
                else { groups.pop_back(); groupLines.pop_back(); }
            } else {
                if (a.Args.size() != 1 || !a.Args[0].IsString || a.Args[0].Text.empty()) {
                    Report(diag, a.Line, true, "@" + a.Name + " ждёт одну строку в кавычках: ---@" + a.Name +
                                                   " \"Название\"");
                } else if (a.Name == "header") {
                    header = a.Args[0].Text;
                } else {
                    groups.push_back(a.Args[0].Text);
                    groupLines.push_back(a.Line);
                }
            }
            states.push_back({a.Line, header, groups});
        }
        for (size_t i = 0; i < groups.size(); ++i)
            Report(diag, groupLines[i], false, "группа " + Quote(groups[i]) + " не закрыта: добавьте ---@end_group");
    }
    auto layoutAt = [&](int line) {
        Layout best{0, {}, {}};
        for (const Layout& s : states)
            if (s.Line < line) best = s;
        return best;
    };

    // Аннотации переменных — к присваиваниям верхнего уровня.
    for (const TopLevelAssign& as : assigns) {
        std::vector<Annotation*> mine;
        for (int idx = as.Line - 2; idx >= 0; --idx) {
            const std::string t = Trim(lines[(size_t)idx]);
            if (t.compare(0, 2, "--") != 0) break;   // пустая строка или код — связь оборвана
            for (Annotation& a : anns)
                if (a.Line == idx + 1 && !IsStandalone(a.Name)) {
                    a.Consumed = true;
                    mine.push_back(&a);
                }
        }
        if (mine.empty()) continue;
        std::reverse(mine.begin(), mine.end());   // в порядке файла

        const Annotation* exp = nullptr;
        const Annotation* tooltip = nullptr;
        for (const Annotation* a : mine) {
            if (a->Name == "tooltip") {
                if (tooltip) Report(diag, a->Line, true, "два @tooltip у переменной " + Quote(as.Name));
                else tooltip = a;
            } else if (exp) {
                Report(diag, a->Line, true,
                       "у переменной " + Quote(as.Name) + " уже есть @" + exp->Name + ": лишняя @" + a->Name);
            } else {
                exp = a;
            }
        }
        if (!exp) {
            if (tooltip)
                Report(diag, tooltip->Line, false,
                       "@tooltip без @export: переменная " + Quote(as.Name) + " не показывается в Inspector");
            continue;
        }

        Var var = as.Var;
        var.Name = as.Name;
        var.Declared = true;
        const std::string& n = exp->Name;
        const std::vector<Arg>& args = exp->Args;
        auto fail = [&](const std::string& why) {
            Report(diag, exp->Line, true, "@" + n + " у переменной " + Quote(as.Name) + ": " + why);
        };
        bool ok = true;

        if (n == "export" || n == "export_range" || n == "export_enum" || n == "export_color") {
            if (!as.Literal) {
                ok = false;
                fail(as.IsNil ? "тип не определить по nil — для ссылок используйте @export_object или "
                                "@export_resource"
                              : "значение должно быть литералом: число, строка, true/false, vector3.new(…) "
                                "или color(…)");
            }
        }
        if (ok && n == "export") {
            if (!args.empty()) Report(diag, exp->Line, false, "@export без аргументов: лишнее проигнорировано");
        } else if (ok && n == "export_range") {
            const Kind k = var.Data.Type();
            if (args.size() != 2 || !args[0].IsNumber || !args[1].IsNumber) {
                fail("нужны два числа: ---@export_range 0 20");
            } else if (k != Kind::Int && k != Kind::Float) {
                fail("диапазон только у числа, а значение — не число");
            } else if (args[0].Number >= args[1].Number) {
                fail("минимум должен быть меньше максимума");
            } else {
                var.Min = (float)args[0].Number;
                var.Max = (float)args[1].Number;
                const double v = k == Kind::Int ? var.Data.AsInt() : var.Data.AsFloat();
                if (v < args[0].Number || v > args[1].Number)
                    Report(diag, as.Line, false,
                           "значение " + Quote(as.Name) + " вне диапазона " + args[0].Text + ".." + args[1].Text);
            }
        } else if (ok && n == "export_enum") {
            bool good = !args.empty();
            for (const Arg& a : args) good = good && a.IsString && !a.Text.empty();
            if (!good) {
                fail("нужен список строк в кавычках: ---@export_enum \"Idle\" \"Walk\" \"Run\"");
            } else if (var.Data.Type() != Kind::String) {
                fail("значение должно быть строкой из списка");
            } else {
                for (const Arg& a : args) var.Options.push_back(a.Text);
                const std::string cur = var.Data.AsString();
                if (std::find(var.Options.begin(), var.Options.end(), cur) == var.Options.end()) {
                    Report(diag, as.Line, false,
                           "значение " + Quote(cur) + " не из списка @export_enum — взято " + Quote(var.Options[0]));
                    var.Data = Value(var.Options[0]);
                }
            }
        } else if (ok && n == "export_color") {
            const Kind k = var.Data.Type();
            if (!args.empty()) Report(diag, exp->Line, false, "@export_color без аргументов: лишнее проигнорировано");
            if (k == Kind::Color) {
            } else if (k == Kind::Vec3) {
                const glm::vec3 c = var.Data.AsVec3();
                var.Data = Value::Convert(Value(glm::vec4(c, 1.0f)), Kind::Color);
            } else {
                fail("значение должно быть цветом: color(1, 1, 1)");
            }
        } else if (n == "export_object") {
            if (!as.IsNil && as.Literal) Report(diag, exp->Line, false, "значение " + Quote(as.Name) + " игнорируется: ссылка задаётся в Inspector");
            if (args.size() > 1 || (args.size() == 1 && !args[0].IsString))
                Report(diag, exp->Line, false, "@export_object: необязательный аргумент — имя компонента в кавычках");
            var.Data = Value::Default(Kind::Entity);
            if (args.size() == 1 && args[0].IsString) var.Hint = args[0].Text;
        } else if (n == "export_resource") {
            if (args.size() != 1 || !args[0].IsString || args[0].Text.empty()) {
                ok = false;
                fail("нужен вид ресурса в кавычках: ---@export_resource \"Animation\"");
            } else {
                const std::string def = (as.Literal && as.Var.Data.Type() == Kind::String) ? as.Var.Data.AsString() : "";
                var.Data = Value::Default(Kind::Asset);
                if (!def.empty()) var.Data = Value(sage::vars::AssetRef{def});
                var.Hint = args[0].Text;
            }
        }
        if (!ok) continue;

        if (tooltip) {
            if (tooltip->Args.size() != 1 || !tooltip->Args[0].IsString)
                Report(diag, tooltip->Line, true, "@tooltip ждёт одну строку в кавычках: ---@tooltip \"Подсказка\"");
            else var.Tooltip = tooltip->Args[0].Text;
        }
        const Layout lay = layoutAt(as.Line);
        var.Header = lay.Header;
        for (size_t i = 0; i < lay.Groups.size(); ++i) var.Group += (i ? "/" : "") + lay.Groups[i];
        if (out.Find(var.Name)) Report(diag, as.Line, false, "переменная " + Quote(var.Name) + " объявлена дважды: действует последняя");
        out.Put(var);
    }

    // Аннотации, которые ни к какой переменной не прилипли.
    for (const Annotation& a : anns)
        if (!a.Consumed)
            Report(diag, a.Line, true,
                   "@" + a.Name + " не связана с переменной: она должна стоять прямо над `имя = значение` на "
                                  "верхнем уровне файла (без пустой строки между ними)");
}

} // namespace sage::scripting::lua
