#include "sage/scripting/ScriptAdvice.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>

namespace sage::scripting {

namespace {

std::string LineOf(const std::string& source, int line) {
    if (line <= 0) return {};
    size_t start = 0;
    for (int n = 1; n < line; ++n) {
        start = source.find('\n', start);
        if (start == std::string::npos) return {};
        ++start;
    }
    size_t end = source.find('\n', start);
    std::string l = source.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    return l;
}

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

std::string IndentOf(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(0, i);
}

// Строка без хвостового комментария — чтобы дописанное «then» не оказалось
// внутри «-- комментария».
std::string StripComment(const std::string& line) {
    const size_t at = line.find("--");
    return at == std::string::npos ? line : line.substr(0, at);
}

// Заменить слово целиком (не кусок другого имени).
std::string ReplaceWord(const std::string& line, const std::string& from, const std::string& to) {
    if (from.empty()) return line;
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        const size_t at = line.find(from, i);
        if (at == std::string::npos) break;
        const bool leftOk = at == 0 || !(std::isalnum((unsigned char)line[at - 1]) || line[at - 1] == '_');
        const size_t end = at + from.size();
        const bool rightOk = end >= line.size() || !(std::isalnum((unsigned char)line[end]) || line[end] == '_');
        out += line.substr(i, at - i);
        out += (leftOk && rightOk) ? to : from;
        i = end;
    }
    out += line.substr(i);
    return out;
}

int Distance(const std::string& a, const std::string& b) {
    // Без учёта регистра: «update» вместо «Update» — тоже опечатка, и самая частая.
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const bool same = std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (same ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// Имя из «(method 'X')», «(field 'X')», «(global 'X')», «(local 'X')».
std::string NameIn(const std::string& msg, std::string* kind = nullptr) {
    static const std::regex kName(R"(\((method|field|global|local|upvalue|constant) '([^']+)'\))");
    std::smatch m;
    if (!std::regex_search(msg, m, kName)) return {};
    if (kind) *kind = m.str(1);
    return m.str(2);
}

const char* KindWord(const std::string& kind) {
    if (kind == "method") return "Метод";
    if (kind == "global") return "Глобальное имя";
    if (kind == "local") return "Переменная";
    if (kind == "upvalue") return "Переменная";
    return "Поле";
}

void DidYouMean(Advice& a, const std::string& name, const std::string& source, const std::string& line) {
    const std::string near = ClosestName(name, source);
    if (near.empty()) return;
    a.Hints.insert(a.Hints.begin(), "Возможно, имелось в виду «" + near + "».");
    if (!line.empty()) a.Example = Trim(ReplaceWord(line, name, near));
}

void ExplainLua(const ScriptError& err, const std::string& source, Advice& a) {
    const std::string& msg = err.Message;
    const std::string line = LineOf(source, err.Line);
    const std::string code = Trim(line);
    auto has = [&](const char* s) { return msg.find(s) != std::string::npos; };
    std::string kind;
    const std::string name = NameIn(msg, &kind);
    std::smatch m;

    if (err.What == ScriptError::Kind::Timeout) {
        a.Detail = "Вызов скрипта не закончился вовремя — почти всегда это бесконечный цикл. "
                   "Движок прервал его, чтобы не зависнуть целиком.";
        a.Hints = {"Проверьте условие выхода у while/repeat: меняется ли то, что в нём проверяется?",
                   "Долгую работу разбейте на кадры: часть в каждом Update, или через корутину с wait()."};
        a.Example = "while not done do\n    step()\n    done = checkFinished()   -- условие обязано меняться\nend";
        return;
    }
    if (err.What == ScriptError::Kind::Missing) {
        a.Detail = "Файла скрипта нет там, куда указывает объект.";
        a.Hints = {"Файл переименовали или перенесли мимо редактора — верните его или выберите заново "
                   "в слоте Script инспектора."};
        return;
    }

    // --- Синтаксис (файл не собрался, ничего не запущено) ------------------
    if (std::regex_search(msg, m, std::regex(R"('end' expected \(to close '(\w+)' at line (\d+)\))"))) {
        a.Detail = "Блок «" + m.str(1) + "», начатый на строке " + m.str(2) + ", не закрыт словом end.";
        a.Hints = {"Допишите end в конце блока. У каждого function, if, for, while и do — свой end."};
        const std::string opener = Trim(LineOf(source, std::atoi(m.str(2).c_str())));
        a.Example = (opener.empty() ? m.str(1) + " ..." : opener) + "\n    ...\nend";
        return;
    }
    if (has("'then' expected") || has("'do' expected")) {
        const bool then = has("'then' expected");
        a.Detail = then ? "После условия if (или elseif) нужно слово then."
                        : "После заголовка for/while нужно слово do.";
        a.Hints = {then ? "Допишите then в конце условия." : "Допишите do в конце заголовка цикла."};
        if (!code.empty()) a.Example = Trim(StripComment(code)) + (then ? " then" : " do");
        return;
    }
    if (has("'=' expected")) {
        a.Detail = "Строка выглядит как присваивание или объявление, но знака = нет — часто это опечатка "
                   "в имени или лишнее слово.";
        a.Hints = {"Проверьте написание имён в этой строке.",
                   "Чтобы сравнить, используйте == внутри if: if a == b then."};
        return;
    }
    if (has("'<eof>' expected")) {
        a.Detail = "Лишнее слово end (или закрывающая скобка): блок уже закрыт раньше.";
        a.Hints = {"Удалите лишний end — проверьте, что у каждого function/if/for ровно один end."};
        return;
    }
    if (has("unfinished string")) {
        a.Detail = "Строка в кавычках не закрыта.";
        a.Hints = {"Допишите закрывающую кавычку — той же, которой строка открыта (\" или ')."};
        a.Example = "print(\"Привет\")";
        return;
    }
    if (has("unexpected symbol") || has("syntax error")) {
        a.Detail = "Lua не понял эту строку: в ней символ, который здесь стоять не может.";
        a.Hints = {"Проверьте скобки, запятые и кавычки в этой строке и в конце предыдущей.",
                   "Не перепутаны ли != и ~= (в Lua «не равно» — это ~=)?"};
        if (code.find("!=") != std::string::npos) a.Example = Trim(ReplaceWord(code, "!=", "~="));
        return;
    }

    // --- Выполнение ---------------------------------------------------------
    if (has("attempt to call a nil value")) {
        a.Detail = name.empty()
            ? "Вызывается функция, которой нет (значение nil)."
            : std::string(KindWord(kind)) + " «" + name + "» не существует (nil) — вызвать его нельзя.";
        a.Hints = {"Проверьте написание имени: регистр букв важен (Update и update — разные имена).",
                   "Если это метод другого скрипта — убедитесь, что на объекте висит именно тот скрипт."};
        DidYouMean(a, name, source, line);
        return;
    }
    if (has("attempt to index a nil value")) {
        a.Detail = name.empty()
            ? "У пустого значения (nil) берут поле."
            : std::string(KindWord(kind)) + " «" + name + "» пустое (nil), а у него берут поле.";
        a.Hints = {"Проверьте, что значение задано, прежде чем к нему обращаться.",
                   "Объект, найденный по имени (Scene.find), может не найтись — проверяйте результат."};
        DidYouMean(a, name, source, line);
        if (a.Example.empty() && !name.empty() && !code.empty())
            a.Example = "if " + name + " then\n    " + code + "\nend";
        return;
    }
    if (has("attempt to perform arithmetic on")) {
        const bool nil = has("nil value");
        a.Detail = nil ? (name.empty() ? "В вычислении участвует пустое значение (nil)."
                                       : "«" + name + "» пустое (nil), а с ним считают.")
                       : "С этим значением нельзя считать (это не число).";
        a.Hints = {nil ? "Задайте начальное значение, например в Start."
                       : "Строку в число переводит tonumber(text)."};
        if (nil && !name.empty())
            a.Example = (kind == "field" ? "self." + name + " = self." + name : name + " = " + name) + " or 0\n" + code;
        else if (!nil)
            a.Example = "local n = tonumber(text) or 0";
        return;
    }
    if (has("attempt to concatenate")) {
        a.Detail = name.empty() ? "Склеивать строкой (..) можно только строки и числа."
                                : "«" + name + "» нельзя склеить со строкой: это nil или таблица.";
        a.Hints = {"Оберните значение в tostring(...)."};
        if (!name.empty() && !code.empty()) a.Example = Trim(ReplaceWord(code, name, "tostring(" + name + ")"));
        return;
    }
    if (std::regex_search(msg, m, std::regex(R"(attempt to compare (\w+) with (\w+))"))) {
        a.Detail = "Сравниваются значения разных типов: " + m.str(1) + " и " + m.str(2) + ".";
        a.Hints = {"Проверьте, что оба значения — числа (tonumber) и ни одно не nil."};
        return;
    }
    if (std::regex_search(msg, m, std::regex(R"(bad argument #(\d+) to '([^']+)' \((\w+) expected, got (\w+)\))"))) {
        a.Detail = "Функция " + m.str(2) + " ждёт аргументом №" + m.str(1) + " значение " + m.str(3) +
                   ", а получила " + m.str(4) + ".";
        a.Hints = {m.str(3) == "number" ? "Число из строки — tonumber(text)." : "Проверьте, что передаёте."};
        return;
    }
    if (has("stack overflow")) {
        a.Detail = "Функция вызывает саму себя без конца (рекурсия без выхода).";
        a.Hints = {"Проверьте условие, при котором рекурсия заканчивается.",
                   "Частый случай: в обработчике изменения свойства меняют то же свойство."};
        return;
    }
    if (has("not enough memory")) {
        a.Detail = "Скрипту не хватило памяти — обычно таблица растёт без предела.";
        a.Hints = {"Проверьте, что из списков и таблиц удаляется то, что больше не нужно."};
    }
}

void ExplainSage(const ScriptError& err, const std::string& source, Advice& a) {
    std::string all = err.Title;
    for (const std::string& d : err.Details) all += "\n" + d;
    const std::string line = LineOf(source, err.Line);
    auto has = [&](const char* s) { return all.find(s) != std::string::npos; };
    std::smatch m;

    if (has("Ожидалось значение number") && has("Получено значение string")) {
        a.Detail = "Нужно число, а дана строка.";
        a.Hints = {"Переведите строку в число: tonumber(text).", "Или исправьте тип в объявлении."};
        a.Example = "local speed: number = tonumber(text) or 0";
        return;
    }
    if (has("Ожидалось значение string") && has("Получено значение number")) {
        a.Detail = "Нужна строка, а дано число.";
        a.Hints = {"Переведите число в строку: tostring(value)."};
        a.Example = "local label: string = tostring(score)";
        return;
    }
    if (std::regex_search(all, m, std::regex(R"(нет (поля|метода) '([^']+)')"))) {
        a.Detail = "Такого " + std::string(m.str(1) == "поля" ? "поля" : "метода") + " у этого типа нет.";
        a.Hints = {"Проверьте написание: регистр букв важен."};
        DidYouMean(a, m.str(2), source, line);
        return;
    }
    if (std::regex_search(all, m, std::regex(R"(неизвестный тип (\w+))"))) {
        a.Detail = "Тип «" + m.str(1) + "» нигде не объявлен и не импортирован.";
        a.Hints = {"Встроенные типы: number, string, boolean, any, Vec3, GameObject.",
                   "Свой тип — объявите class/struct или подключите файл: import \"Game/" + m.str(1) + "\"."};
        DidYouMean(a, m.str(1), source, line);
        return;
    }
    if (err.Title.find("Ошибка импорта") != std::string::npos) {
        a.Detail = "Подключаемый файл не нашёлся или в нём ошибка.";
        a.Hints = {"Путь импорта — от папки assets/scripts проекта, без расширения: import \"Game/Player\".",
                   "Если файл на месте — откройте его: ошибка может быть в нём самом."};
        return;
    }
    if (err.Title.find("Ошибка вызова") != std::string::npos) {
        a.Detail = "Функцию вызвали не с теми аргументами, что она объявляет.";
        a.Hints = {"Сверьте число и типы аргументов с объявлением функции."};
        return;
    }
    if (err.Title.find("Ошибка аннотации") != std::string::npos) {
        a.Detail = "Аннотация (@export, @range, @event…) записана неверно или стоит не там.";
        a.Hints = {"Аннотация пишется строкой выше объявления поля или метода.",
                   "Пример правильной записи — ниже."};
        a.Example = "@export\n@range(0, 10)\nspeed: number = 5";
        return;
    }
    if (err.Title.find("Синтаксическая ошибка") != std::string::npos) {
        // Сообщение лексера/разбора — тем же разбором, что у Lua.
        ScriptError lua = err;
        lua.Message = all;
        ExplainLua(lua, source, a);
        if (a.Detail.empty()) {
            a.Detail = "Строка не разобралась: в ней символ или слово, которых здесь быть не может.";
            a.Hints = {"Проверьте скобки, кавычки и ключевые слова в этой строке и в конце предыдущей."};
        }
    }
}

} // namespace

std::string ClosestName(const std::string& wrong, const std::string& source) {
    if (wrong.size() < 2) return {};
    std::map<std::string, int> seen;   // имя -> сколько раз встретилось
    static const std::regex kIdent(R"([A-Za-z_][A-Za-z0-9_]*)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), kIdent); it != std::sregex_iterator(); ++it)
        ++seen[it->str()];
    // Хуки движка и частые имена API — их пишут, не видя в файле правильного.
    for (int h = 0; h < (int)Hook::Count; ++h) seen[HookName((Hook)h)] += 2;
    for (const char* api : {"transform", "position", "rotation", "scale", "gameObject", "Vec3", "print",
                            "Scene", "Input", "Time", "Tween", "find", "Destroy", "Instantiate"})
        seen[api] += 1;

    std::string best;
    int bestDist = 3, bestSeen = 0;
    for (const auto& [name, count] : seen) {
        if (name == wrong) continue;
        const int d = Distance(wrong, name);
        if (d > 2 || d > (int)wrong.size() / 2) continue;
        if (d < bestDist || (d == bestDist && count > bestSeen)) {
            best = name;
            bestDist = d;
            bestSeen = count;
        }
    }
    return best;
}

Advice Explain(const ScriptError& err, const std::string& source) {
    Advice a;
    if (!err.Title.empty()) ExplainSage(err, source, a);
    else ExplainLua(err, source, a);
    return a;
}

sage::Problem ToProblem(const ScriptError& err, const std::string& source) {
    sage::Problem p;
    p.Category = "Script";
    p.Level = sage::Severity::Error;
    switch (err.What) {
        case ScriptError::Kind::Compile: p.Code = "script.compile"; break;
        case ScriptError::Kind::Missing: p.Code = "script.missing"; break;
        case ScriptError::Kind::Timeout: p.Code = "script.timeout"; break;
        default: p.Code = "script.runtime"; break;
    }
    p.File = err.File;
    p.Line = err.Line;
    p.Col = err.Col;
    if (!err.Title.empty()) {
        // Диагностика SAGE Lua: одной строкой в заголовок, а подробности —
        // как есть: они уже написаны для человека.
        p.Title = err.Title + ":";
        for (const std::string& d : err.Details) p.Title += " " + d;
    } else {
        p.Title = err.Message;
        const size_t nl = p.Title.find('\n');
        if (nl != std::string::npos) p.Title = p.Title.substr(0, nl);
    }
    const Advice a = Explain(err, source);
    p.Detail = a.Detail;
    p.Hints = a.Hints;
    p.Example = a.Example;
    if (!source.empty() && err.Line > 0) p.Excerpt = sage::Problems::Excerpt(source, err.Line, err.Col);
    p.Technical = err.Message;
    if (!err.Traceback.empty()) p.Technical += "\n" + err.Traceback;
    if (err.What == ScriptError::Kind::Compile)
        p.Hints.push_back("Пока ошибка не исправлена, скрипт не запускается — объект работает без него, "
                          "а при горячей перезагрузке остаётся прежняя рабочая версия.");
    return p;
}

} // namespace sage::scripting
