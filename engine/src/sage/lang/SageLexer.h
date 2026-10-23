#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SAGE Lua: ЛЕКСЕР.
//
// Слова языка SAGE (class, enum, import, const, await…) лексер отдаёт как
// ОБЫЧНЫЕ ИМЕНА, а не как ключевые слова. Это не упущение, а требование
// совместимости: `type(x)` — встроенная функция Lua, `class`, `event`, `var`
// — частые имена переменных в существующем коде. Что слово значит, решает
// парсер по месту: `class Player` в начале оператора — объявление класса,
// `class = 3` — присваивание. Ключевыми остаются только слова самого Lua.
// ---------------------------------------------------------------------------
namespace sage::lang {

struct Pos {
    int Line = 0;
    int Col = 0;
};

enum class Tok {
    Eof,
    Name,
    Number,
    String,
    // Ключевые слова Lua.
    And, Break, Do, Else, Elseif, End, False, For, Function, Goto, If, In, Local, Nil, Not, Or,
    Repeat, Return, Then, True, Until, While,
    // Знаки.
    Plus, Minus, Star, Slash, DSlash, Percent, Caret, Hash, Amp, Tilde, Pipe, Shl, Shr,
    Eq, Ne, Le, Ge, Lt, Gt, Assign, LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    DColon, Semi, Colon, Comma, Dot, Concat, Ellipsis,
    // Расширения SAGE.
    Arrow,      // =>
    Walrus,     // :=
    Question,   // ?
    At,         // @
    Bang,       // ! (только в составе !=)
};

struct Token {
    Tok Kind = Tok::Eof;
    std::string Text;   // имя, число как написано, строка КАК В ИСХОДНИКЕ (с кавычками)
    Pos P;
};

struct LexError {
    Pos P;
    std::string Message;
};

// Разбирает весь файл. Ошибки (незакрытая строка, неизвестный знак) копятся в
// errors; разбор продолжается, чтобы человек увидел их все разом.
std::vector<Token> Lex(const std::string& source, std::vector<LexError>& errors);

const char* TokName(Tok t);

} // namespace sage::lang
