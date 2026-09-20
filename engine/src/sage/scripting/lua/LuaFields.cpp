#include "sage/scripting/lua/LuaInternal.h"

#include <cctype>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ СКРИПТА: `Player.public = { ... }`.
//
//     Player.public = {
//         MoveSpeed = field.number(5.0, 0.0, 20.0),
//         Camera    = field.entity(),
//         Animator  = field.component("Animation"),
//         Health    = 100,
//         CanJump   = true,
//     }
//
// РАЗБОР ТЕКСТОМ, А НЕ ЗАПУСКОМ ФАЙЛА, и это главное решение здесь.
// Инспектор показывает переменные, ПОКА ИГРА НЕ ЗАПУЩЕНА: он — то, чем
// настраивают уровень перед запуском. Выполнить ради списка переменных чужой
// файл нельзя: скрипт на верхнем уровне может открыть сокет, начать грузить
// уровень или уронить редактор ошибкой в коде, до которого в игре дело дошло
// бы через час.
//
// Цена честная и записана здесь: `Player.public = MakeFields()` не разберётся.
// И это правильно: объявление обязано быть видно глазами в файле, иначе
// инспектор показывает одно, а игра делает другое.
// ---------------------------------------------------------------------------
namespace sage::scripting::lua {

namespace {

using sage::vars::Kind;
using sage::vars::Value;
using sage::vars::Var;

struct Reader {
    const std::string& S;
    size_t I = 0;

    explicit Reader(const std::string& s) : S(s) {}

    bool Done() const { return I >= S.size(); }
    char Peek() const { return I < S.size() ? S[I] : '\0'; }

    void Skip() {
        while (I < S.size()) {
            if (std::isspace((unsigned char)S[I])) { ++I; continue; }
            if (S.compare(I, 2, "--") == 0) { // комментарии в объявлении пишут всегда
                if (S.compare(I, 4, "--[[") == 0) {
                    const size_t end = S.find("]]", I + 4);
                    I = (end == std::string::npos) ? S.size() : end + 2;
                } else {
                    const size_t end = S.find('\n', I);
                    I = (end == std::string::npos) ? S.size() : end + 1;
                }
                continue;
            }
            break;
        }
    }

    bool Eat(char c) {
        Skip();
        if (Peek() != c) return false;
        ++I;
        return true;
    }

    std::string Name() {
        Skip();
        const size_t start = I;
        while (I < S.size() && (std::isalnum((unsigned char)S[I]) || S[I] == '_')) ++I;
        return S.substr(start, I - start);
    }

    bool String(std::string& out) {
        Skip();
        const char q = Peek();
        if (q != '"' && q != '\'') return false;
        ++I;
        out.clear();
        while (I < S.size() && S[I] != q) {
            if (S[I] == '\\' && I + 1 < S.size()) {
                ++I;
                switch (S[I]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    default: out += S[I]; break;
                }
                ++I;
                continue;
            }
            out += S[I++];
        }
        if (I < S.size()) ++I;
        return true;
    }

    bool Number(double& out, bool& integral) {
        Skip();
        const size_t start = I;
        if (Peek() == '-' || Peek() == '+') ++I;
        bool digits = false, dot = false;
        while (I < S.size()) {
            if (std::isdigit((unsigned char)S[I])) { digits = true; ++I; continue; }
            if (S[I] == '.' && !dot) { dot = true; ++I; continue; }
            if ((S[I] == 'e' || S[I] == 'E') && digits) {
                ++I;
                if (I < S.size() && (S[I] == '-' || S[I] == '+')) ++I;
                dot = true;
                continue;
            }
            break;
        }
        if (!digits) { I = start; return false; }
        out = std::atof(S.substr(start, I - start).c_str());
        integral = !dot;
        return true;
    }

    bool Keyword(const char* word) {
        Skip();
        const size_t n = std::string(word).size();
        if (S.compare(I, n, word) != 0) return false;
        const size_t after = I + n;
        if (after < S.size() && (std::isalnum((unsigned char)S[after]) || S[after] == '_'))
            return false;
        I = after;
        return true;
    }

    // Пропускает значение любого вида: одна незнакомая запись не должна съесть
    // остальные переменные.
    void SkipValue() {
        Skip();
        int depth = 0;
        while (!Done()) {
            const char c = Peek();
            if (c == '{' || c == '(') { ++depth; ++I; continue; }
            if (c == '}' && depth == 0) return;
            if (c == '}' || c == ')') {
                --depth;
                ++I;
                if (depth <= 0) return;
                continue;
            }
            if (c == '"' || c == '\'') { std::string t; String(t); continue; }
            if (depth == 0 && (c == ',' || c == ';' || c == '\n')) return;
            ++I;
        }
    }
};

bool Scalar(Reader& r, Value& out) {
    std::string text;
    if (r.String(text)) { out = Value(text); return true; }
    if (r.Keyword("true")) { out = Value(true); return true; }
    if (r.Keyword("false")) { out = Value(false); return true; }
    double n = 0.0;
    bool integral = false;
    if (r.Number(n, integral)) {
        out = integral ? Value((int)n) : Value((float)n);
        return true;
    }
    return false;
}

// field.<вид>(...) — типизированное поле.
bool FieldCall(Reader& r, Var& var) {
    const size_t mark = r.I;
    r.Skip();
    if (!r.Keyword("field")) { r.I = mark; return false; }
    if (!r.Eat('.')) { r.I = mark; return false; }
    const std::string kind = r.Name();
    if (!r.Eat('(')) { r.I = mark; return false; }

    // Аргументы по порядку: значение по умолчанию, затем границы (у чисел) или
    // уточнение вида (у ссылок и файлов).
    std::vector<Value> args;
    std::string text;
    while (true) {
        r.Skip();
        if (r.Eat(')') || r.Done()) break;
        Value v;
        if (Scalar(r, v)) args.push_back(v);
        else if (r.Eat('{')) { // описание { label = "...", tooltip = "..." }
            while (!r.Done()) {
                r.Skip();
                if (r.Eat('}')) break;
                const std::string key = r.Name();
                if (key.empty()) { ++r.I; continue; }
                if (!r.Eat('=')) { r.SkipValue(); continue; }
                if (key == "label" && r.String(text)) var.Label = text;
                else if (key == "tooltip" && r.String(text)) var.Tooltip = text;
                else r.SkipValue();
                r.Skip();
                if (!r.Eat(',')) r.Eat(';');
            }
        } else {
            r.SkipValue();
        }
        r.Skip();
        if (!r.Eat(',')) r.Eat(';');
    }

    auto arg = [&args](size_t i) { return i < args.size() ? args[i] : Value(); };

    if (kind == "number") {
        var.Data = Value(args.empty() ? 0.0f : arg(0).AsFloat());
        var.Min = args.size() > 1 ? arg(1).AsFloat() : 0.0f;
        var.Max = args.size() > 2 ? arg(2).AsFloat() : 0.0f;
    } else if (kind == "integer" || kind == "int") {
        var.Data = Value(args.empty() ? 0 : arg(0).AsInt());
        var.Min = args.size() > 1 ? arg(1).AsFloat() : 0.0f;
        var.Max = args.size() > 2 ? arg(2).AsFloat() : 0.0f;
    } else if (kind == "boolean" || kind == "bool") {
        var.Data = Value(args.empty() ? false : arg(0).AsBool());
    } else if (kind == "string" || kind == "text") {
        var.Data = Value(args.empty() ? std::string() : arg(0).AsString());
    } else if (kind == "entity" || kind == "object") {
        var.Data = Value::Default(Kind::Entity);
    } else if (kind == "component") {
        // Ссылка на КОМПОНЕНТ — та же ссылка на объект, но с уточнением: слот
        // в инспекторе не примет объект без такого компонента, а скрипт
        // получит сразу компонент.
        var.Data = Value::Default(Kind::Entity);
        var.Hint = args.empty() ? std::string() : arg(0).AsString();
    } else if (kind == "color") {
        var.Data = Value::Default(Kind::Color);
    } else if (kind == "vector2" || kind == "vec2") {
        var.Data = Value::Default(Kind::Vec2);
    } else if (kind == "vector3" || kind == "vec3") {
        var.Data = Value::Default(Kind::Vec3);
    } else if (kind == "asset" || kind == "file") {
        var.Data = Value::Default(Kind::Asset);
        var.Hint = args.empty() ? std::string() : arg(0).AsString();
    } else {
        // Неизвестный вид поля — заводим переменную как есть: автор её
        // объявил, и потерять её молча хуже, чем показать со значением по
        // умолчанию.
        return true;
    }
    return true;
}

// Старая форма описания: { 10, min = 0, max = 100, label = "Урон" }.
bool Described(Reader& r, Var& var) {
    if (!r.Eat('{')) return false;
    bool haveValue = false;
    std::string declaredKind;
    while (true) {
        r.Skip();
        if (r.Eat('}') || r.Done()) break;
        const size_t mark = r.I;
        const std::string key = r.Name();
        if (!key.empty() && r.Eat('=')) {
            if (key == "min" || key == "max") {
                double n = 0.0;
                bool integral = false;
                if (r.Number(n, integral)) (key == "min" ? var.Min : var.Max) = (float)n;
                else r.SkipValue();
            } else if (key == "label" || key == "tooltip" || key == "kind" || key == "of") {
                std::string text;
                if (r.String(text)) {
                    if (key == "label") var.Label = text;
                    else if (key == "tooltip") var.Tooltip = text;
                    else if (key == "of") var.Hint = text;
                    else declaredKind = text;
                } else {
                    r.SkipValue();
                }
            } else if (key == "value" || key == "default") {
                Value v;
                if (Scalar(r, v)) { var.Data = v; haveValue = true; }
                else r.SkipValue();
            } else {
                r.SkipValue();
            }
        } else {
            r.I = mark;
            Value v;
            if (Scalar(r, v)) { var.Data = v; haveValue = true; }
            else r.SkipValue();
        }
        r.Skip();
        if (!r.Eat(',')) r.Eat(';');
    }
    if (!declaredKind.empty()) {
        Kind k = Kind::Bool;
        if (sage::vars::ParseKind(declaredKind, k))
            var.Data = haveValue ? Value::Convert(var.Data, k) : Value::Default(k);
    }
    return true;
}

// Ищет `<что-то>.public = {` или `Vars = {` (прежняя форма) на верхнем уровне.
// Возвращает позицию сразу после '=' или npos.
size_t FindDeclaration(const std::string& source) {
    static const char* kKeys[] = {".public", "Vars"};
    for (const char* key : kKeys) {
        const std::string k = key;
        size_t from = 0;
        while (true) {
            const size_t at = source.find(k, from);
            if (at == std::string::npos) break;
            from = at + k.size();
            if (k == "Vars") {
                if (at > 0 && (std::isalnum((unsigned char)source[at - 1]) ||
                               source[at - 1] == '_' || source[at - 1] == '.' ||
                               source[at - 1] == ':'))
                    continue;
            }
            size_t j = from;
            while (j < source.size() && std::isspace((unsigned char)source[j])) ++j;
            if (j >= source.size() || source[j] != '=') continue;
            if (j + 1 < source.size() && source[j + 1] == '=') continue; // сравнение, не присваивание
            return j + 1;
        }
    }
    return std::string::npos;
}

} // namespace

sage::vars::Table ParsePublicFields(const std::string& source) {
    sage::vars::Table out;
    const size_t at = FindDeclaration(source);
    if (at == std::string::npos) return out;

    Reader r(source);
    r.I = at;
    if (!r.Eat('{')) return out;

    while (true) {
        r.Skip();
        if (r.Eat('}') || r.Done()) break;

        const std::string name = r.Name();
        if (name.empty() || !r.Eat('=')) {
            // Не «имя = значение»: массивная часть таблицы публичной
            // переменной не задаёт.
            r.SkipValue();
            r.Skip();
            if (!r.Eat(',')) r.Eat(';');
            continue;
        }

        Var var;
        var.Name = name;
        var.Declared = true;
        r.Skip();
        if (!FieldCall(r, var)) {
            if (r.Peek() == '{') {
                if (!Described(r, var)) break;
            } else if (!Scalar(r, var.Data)) {
                // Значение — выражение или вызов: переменную ЗАВОДИМ (автор её
                // объявил, и в инспекторе она нужна), но со значением по
                // умолчанию.
                r.SkipValue();
            }
        }
        out.Put(var);

        r.Skip();
        if (!r.Eat(',')) r.Eat(';');
    }
    return out;
}

// --- `field` В САМОМ LUA -----------------------------------------------------
//
// Объявление разбирается текстом, но файл ведь ещё и ВЫПОЛНЯЕТСЯ — значит,
// `field.number(...)` обязан существовать, иначе первая же строка объявления
// уронит скрипт. Возвращается описание-таблица: значение всё равно приходит из
// сцены (ApplyFields), а описание пригодится тому, кто захочет прочитать
// объявление уже из Lua.
void RegisterFields(Backend& backend) {
    sol::state& lua = backend.Lua();
    sol::table field = lua.create_table();

    auto make = [&lua](const char* kind) {
        return [&lua, kind](sol::variadic_args args) {
            sol::table t = lua.create_table();
            t["__field"] = kind;
            if (args.size() > 0) t["default"] = sol::object(args[0]);
            if (args.size() > 1) t["min"] = sol::object(args[1]);
            if (args.size() > 2) t["max"] = sol::object(args[2]);
            return t;
        };
    };
    field.set_function("number", make("number"));
    field.set_function("integer", make("integer"));
    field.set_function("boolean", make("boolean"));
    field.set_function("string", make("string"));
    field.set_function("entity", make("entity"));
    field.set_function("component", make("component"));
    field.set_function("color", make("color"));
    field.set_function("vector2", make("vector2"));
    field.set_function("vector3", make("vector3"));
    field.set_function("asset", make("asset"));
    lua["field"] = field;
}

} // namespace sage::scripting::lua
