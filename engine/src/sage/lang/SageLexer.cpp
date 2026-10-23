#include "sage/lang/SageLexer.h"

#include <cctype>
#include <unordered_map>

namespace sage::lang {

namespace {

const std::unordered_map<std::string, Tok>& Keywords() {
    static const std::unordered_map<std::string, Tok> k = {
        {"and", Tok::And},       {"break", Tok::Break},   {"do", Tok::Do},
        {"else", Tok::Else},     {"elseif", Tok::Elseif}, {"end", Tok::End},
        {"false", Tok::False},   {"for", Tok::For},       {"function", Tok::Function},
        {"goto", Tok::Goto},     {"if", Tok::If},         {"in", Tok::In},
        {"local", Tok::Local},   {"nil", Tok::Nil},       {"not", Tok::Not},
        {"or", Tok::Or},         {"repeat", Tok::Repeat}, {"return", Tok::Return},
        {"then", Tok::Then},     {"true", Tok::True},     {"until", Tok::Until},
        {"while", Tok::While},
    };
    return k;
}

struct Cursor {
    const std::string& S;
    size_t I = 0;
    int Line = 1;
    int Col = 1;

    char At(size_t off = 0) const { return I + off < S.size() ? S[I + off] : '\0'; }
    bool Done() const { return I >= S.size(); }
    void Step() {
        if (Done()) return;
        if (S[I] == '\n') {
            ++Line;
            Col = 1;
        } else {
            ++Col;
        }
        ++I;
    }
};

// Длинная скобка [[ ]] / [==[ ]==]: возвращает уровень (число '='), -1 — не она.
int LongBracketLevel(const Cursor& c) {
    if (c.At() != '[') return -1;
    size_t j = 1;
    while (c.At(j) == '=') ++j;
    return c.At(j) == '[' ? (int)(j - 1) : -1;
}

// Читает длинную скобку целиком; true — закрыта.
bool ReadLong(Cursor& c, int level, std::string* out) {
    const size_t start = c.I;
    for (int i = 0; i < level + 2; ++i) c.Step();   // [==[
    while (!c.Done()) {
        if (c.At() == ']') {
            size_t j = 1;
            while (c.At(j) == '=') ++j;
            if ((int)(j - 1) == level && c.At(j) == ']') {
                for (size_t k = 0; k <= j; ++k) c.Step();
                if (out) *out = c.S.substr(start, c.I - start);
                return true;
            }
        }
        c.Step();
    }
    if (out) *out = c.S.substr(start, c.I - start);
    return false;
}

} // namespace

std::vector<Token> Lex(const std::string& source, std::vector<LexError>& errors) {
    std::vector<Token> out;
    out.reserve(source.size() / 4 + 16);
    Cursor c{source};
    // Строка «#!» в начале файла — как у Lua: пропускается.
    if (c.At() == '#' && c.At(1) == '!')
        while (!c.Done() && c.At() != '\n') c.Step();

    auto push = [&](Tok kind, Pos p, std::string text = {}) {
        out.push_back(Token{kind, std::move(text), p});
    };

    while (!c.Done()) {
        const char ch = c.At();
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v') {
            c.Step();
            continue;
        }
        const Pos p{c.Line, c.Col};
        // Комментарии.
        if (ch == '-' && c.At(1) == '-') {
            c.Step();
            c.Step();
            const int level = LongBracketLevel(c);
            if (level >= 0) {
                if (!ReadLong(c, level, nullptr)) errors.push_back({p, "незакрытый многострочный комментарий"});
            } else {
                while (!c.Done() && c.At() != '\n') c.Step();
            }
            continue;
        }
        // Имена.
        if (std::isalpha((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80) {
            const size_t start = c.I;
            while (!c.Done() && (std::isalnum((unsigned char)c.At()) || c.At() == '_' ||
                                 (unsigned char)c.At() >= 0x80))
                c.Step();
            std::string word = source.substr(start, c.I - start);
            auto kw = Keywords().find(word);
            push(kw != Keywords().end() ? kw->second : Tok::Name, p, std::move(word));
            continue;
        }
        // Числа (десятичные, шестнадцатеричные, с порядком).
        if (std::isdigit((unsigned char)ch) || (ch == '.' && std::isdigit((unsigned char)c.At(1)))) {
            const size_t start = c.I;
            if (ch == '0' && (c.At(1) == 'x' || c.At(1) == 'X')) {
                c.Step();
                c.Step();
                while (std::isxdigit((unsigned char)c.At()) || c.At() == '.' ||
                       ((c.At() == 'p' || c.At() == 'P')) ||
                       ((c.At() == '+' || c.At() == '-') && (c.S[c.I - 1] == 'p' || c.S[c.I - 1] == 'P')))
                    c.Step();
            } else {
                while (std::isdigit((unsigned char)c.At()) || c.At() == '.' || c.At() == 'e' ||
                       c.At() == 'E' ||
                       ((c.At() == '+' || c.At() == '-') && (c.S[c.I - 1] == 'e' || c.S[c.I - 1] == 'E')))
                    c.Step();
            }
            push(Tok::Number, p, source.substr(start, c.I - start));
            continue;
        }
        // Строки: кавычки и длинные скобки. Текст хранится КАК В ИСХОДНИКЕ —
        // генератор отдаёт его Lua без перекодирования, и экранирование
        // остаётся ровно тем, что написал человек.
        if (ch == '"' || ch == '\'') {
            const size_t start = c.I;
            const char q = ch;
            c.Step();
            bool closed = false;
            while (!c.Done()) {
                const char d = c.At();
                if (d == '\\') {
                    c.Step();
                    if (c.At() == 'z') {   // \z — пропуск пробелов, в том числе переводов строк
                        c.Step();
                        continue;
                    }
                    c.Step();
                    continue;
                }
                if (d == '\n') break;
                c.Step();
                if (d == q) {
                    closed = true;
                    break;
                }
            }
            if (!closed) errors.push_back({p, "незакрытая строка"});
            push(Tok::String, p, source.substr(start, c.I - start));
            continue;
        }
        if (ch == '[') {
            const int level = LongBracketLevel(c);
            if (level >= 0) {
                std::string text;
                if (!ReadLong(c, level, &text)) errors.push_back({p, "незакрытая многострочная строка"});
                push(Tok::String, p, std::move(text));
                continue;
            }
        }
        // Знаки: сначала трёх- и двухсимвольные.
        auto two = [&](char a, char b) { return ch == a && c.At(1) == b; };
        Tok kind = Tok::Eof;
        int len = 1;
        if (ch == '.' && c.At(1) == '.' && c.At(2) == '.') { kind = Tok::Ellipsis; len = 3; }
        else if (two('.', '.')) { kind = Tok::Concat; len = 2; }
        else if (two('=', '=')) { kind = Tok::Eq; len = 2; }
        else if (two('~', '=')) { kind = Tok::Ne; len = 2; }
        else if (two('!', '=')) { kind = Tok::Ne; len = 2; }   // != — тот же ~=
        else if (two('<', '=')) { kind = Tok::Le; len = 2; }
        else if (two('>', '=')) { kind = Tok::Ge; len = 2; }
        else if (two('<', '<')) { kind = Tok::Shl; len = 2; }
        else if (two('>', '>')) { kind = Tok::Shr; len = 2; }
        else if (two('/', '/')) { kind = Tok::DSlash; len = 2; }
        else if (two(':', ':')) { kind = Tok::DColon; len = 2; }
        else if (two('=', '>')) { kind = Tok::Arrow; len = 2; }
        else if (two(':', '=')) { kind = Tok::Walrus; len = 2; }
        else {
            switch (ch) {
                case '+': kind = Tok::Plus; break;
                case '-': kind = Tok::Minus; break;
                case '*': kind = Tok::Star; break;
                case '/': kind = Tok::Slash; break;
                case '%': kind = Tok::Percent; break;
                case '^': kind = Tok::Caret; break;
                case '#': kind = Tok::Hash; break;
                case '&': kind = Tok::Amp; break;
                case '~': kind = Tok::Tilde; break;
                case '|': kind = Tok::Pipe; break;
                case '<': kind = Tok::Lt; break;
                case '>': kind = Tok::Gt; break;
                case '=': kind = Tok::Assign; break;
                case '(': kind = Tok::LParen; break;
                case ')': kind = Tok::RParen; break;
                case '{': kind = Tok::LBrace; break;
                case '}': kind = Tok::RBrace; break;
                case '[': kind = Tok::LBracket; break;
                case ']': kind = Tok::RBracket; break;
                case ';': kind = Tok::Semi; break;
                case ':': kind = Tok::Colon; break;
                case ',': kind = Tok::Comma; break;
                case '.': kind = Tok::Dot; break;
                case '?': kind = Tok::Question; break;
                case '@': kind = Tok::At; break;
                default: break;
            }
        }
        if (kind == Tok::Eof) {
            errors.push_back({p, std::string("неизвестный знак '") + ch + "'"});
            c.Step();
            continue;
        }
        std::string text = source.substr(c.I, (size_t)len);
        for (int i = 0; i < len; ++i) c.Step();
        push(kind, p, std::move(text));
    }
    push(Tok::Eof, Pos{c.Line, c.Col});
    return out;
}

const char* TokName(Tok t) {
    switch (t) {
        case Tok::Eof: return "конец файла";
        case Tok::Name: return "имя";
        case Tok::Number: return "число";
        case Tok::String: return "строка";
        case Tok::End: return "'end'";
        case Tok::Then: return "'then'";
        case Tok::Do: return "'do'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::RBracket: return "']'";
        case Tok::Assign: return "'='";
        case Tok::Comma: return "','";
        case Tok::Colon: return "':'";
        case Tok::Gt: return "'>'";
        case Tok::Until: return "'until'";
        case Tok::In: return "'in'";
        default: return "знак";
    }
}

} // namespace sage::lang
