#include "sage/vars/ScriptVars.h"

#include <cctype>

namespace sage::vars {

namespace {

// --- Крошечный разборщик литералов Lua --------------------------------------
//
// Ровно столько, сколько нужно объявлению: числа, строки, true/false, вложенная
// таблица с ключами. Ни выражений, ни вызовов — см. заголовок: объявление
// обязано быть данными.
struct Reader {
    const std::string& S;
    size_t I = 0;

    explicit Reader(const std::string& s) : S(s) {}

    bool Done() const { return I >= S.size(); }
    char Peek() const { return I < S.size() ? S[I] : '\0'; }

    void Skip() {
        while (I < S.size()) {
            if (std::isspace((unsigned char)S[I])) { ++I; continue; }
            // Комментарии Lua: в объявлении их пишут почти всегда.
            if (S.compare(I, 2, "--") == 0) {
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
        out = std::stod(S.substr(start, I - start));
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

    // Пропускает значение любого вида — нужно, чтобы неразобранная запись не
    // сбивала разбор следующих: одна незнакомая строка не должна съедать
    // остальные переменные.
    void SkipValue() {
        Skip();
        if (Peek() == '{') {
            int depth = 0;
            do {
                if (Peek() == '{') ++depth;
                else if (Peek() == '}') --depth;
                else if (Peek() == '"' || Peek() == '\'') { std::string t; String(t); continue; }
                ++I;
                Skip();
            } while (!Done() && depth > 0);
            return;
        }
        while (!Done() && Peek() != ',' && Peek() != '}' && Peek() != '\n') ++I;
    }
};

// Одно скалярное значение объявления. false — значение не литеральное.
bool ScalarValue(Reader& r, Value& out) {
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

// Таблица-описание: { 10, min = 0, max = 100, label = "Урон", kind = "entity" }
bool DescribedVar(Reader& r, Var& var) {
    if (!r.Eat('{')) return false;
    bool haveValue = false;
    std::string declaredKind;
    while (true) {
        r.Skip();
        if (r.Eat('}')) break;
        if (r.Done()) return false;

        const size_t mark = r.I;
        const std::string key = r.Name();
        if (!key.empty() && r.Eat('=')) {
            if (key == "min" || key == "max") {
                double n = 0.0;
                bool integral = false;
                if (r.Number(n, integral)) {
                    (key == "min" ? var.Min : var.Max) = (float)n;
                } else {
                    r.SkipValue();
                }
            } else if (key == "label" || key == "tooltip" || key == "kind") {
                std::string text;
                if (r.String(text)) {
                    if (key == "label") var.Label = text;
                    else if (key == "tooltip") var.Tooltip = text;
                    else declaredKind = text;
                } else {
                    r.SkipValue();
                }
            } else if (key == "value" || key == "default") {
                Value v;
                if (ScalarValue(r, v)) { var.Data = v; haveValue = true; }
                else r.SkipValue();
            } else {
                r.SkipValue();
            }
        } else {
            // Позиционный элемент — это значение по умолчанию.
            r.I = mark;
            Value v;
            if (ScalarValue(r, v)) { var.Data = v; haveValue = true; }
            else r.SkipValue();
        }
        r.Skip();
        if (!r.Eat(',')) r.Eat(';');
    }
    // Вид, названный явно, сильнее угаданного по значению: `{ kind = "entity" }`
    // без значения — это ссылка, а не «пустая строка».
    if (!declaredKind.empty()) {
        Kind k = Kind::Bool;
        if (ParseKind(declaredKind, k))
            var.Data = haveValue ? Value::Convert(var.Data, k) : Value::Default(k);
    }
    return true;
}

} // namespace

Table ParseDeclaration(const std::string& source) {
    Table out;

    // Ищем присваивание Vars = { ... } на верхнем уровне. Первое: второе
    // объявление в том же файле — ошибка автора, и молча слить их значило бы
    // показать в инспекторе то, чего в игре не будет.
    size_t at = std::string::npos;
    for (size_t i = 0; i + 4 <= source.size(); ++i) {
        if (source.compare(i, 4, "Vars") != 0) continue;
        if (i > 0 && (std::isalnum((unsigned char)source[i - 1]) || source[i - 1] == '_' ||
                      source[i - 1] == '.' || source[i - 1] == ':'))
            continue;
        size_t j = i + 4;
        while (j < source.size() && std::isspace((unsigned char)source[j])) ++j;
        if (j >= source.size() || source[j] != '=') continue;
        at = j + 1;
        break;
    }
    if (at == std::string::npos) return out;

    Reader r(source);
    r.I = at;
    if (!r.Eat('{')) return out;

    while (true) {
        r.Skip();
        if (r.Eat('}') || r.Done()) break;

        const std::string name = r.Name();
        if (name.empty() || !r.Eat('=')) {
            // Не «имя = значение» — пропускаем запись целиком: массивная часть
            // таблицы публичной переменной не задаёт.
            r.SkipValue();
            r.Skip();
            if (!r.Eat(',')) r.Eat(';');
            continue;
        }

        Var var;
        var.Name = name;
        var.Declared = true;
        r.Skip();
        if (r.Peek() == '{') {
            if (!DescribedVar(r, var)) break;
        } else if (!ScalarValue(r, var.Data)) {
            // Значение — выражение или вызов: переменную ЗАВОДИМ (автор её
            // объявил, и в инспекторе она нужна), но со значением по умолчанию.
            r.SkipValue();
        }
        out.Put(var);

        r.Skip();
        if (!r.Eat(',')) r.Eat(';');
    }
    return out;
}

} // namespace sage::vars
