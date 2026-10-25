#pragma once
#include "sage/lang/SageParser.h"
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Внутренняя кухня разбора SAGE Lua, общая для файлов SageParser*.cpp.
//
// SageParser.cpp был одним классом на тысячу с лишним строк. Класс объявлен
// здесь, тела разложены по видам конструкций:
//   SageParser_Stmt.cpp — токены, блоки, операторы, аннотации;
//   SageParser_Decl.cpp — классы, интерфейсы, структуры, функции, типы;
//   SageParser_Expr.cpp — выражения;
//   SageParser.cpp      — вход (Parse) и печать типа.
// ---------------------------------------------------------------------------
namespace sage::lang::parse {

// Синтаксическая ошибка уже записана — разбор прекращается.
struct ParseAbort {};
// Пробный разбор (стрелочная функция, обобщённый вызов) не удался — откат.
struct SpecFail {};

// Слова, которые в начале члена класса означают модификатор.
inline bool IsModifier(const std::string& w) {
    return w == "public" || w == "private" || w == "protected" || w == "static" ||
           w == "override" || w == "final" || w == "async" || w == "readonly";
}

class Parser {
public:
    Parser(const std::vector<Token>& toks, DiagnosticSink& diag) : t(toks), d(diag) {}

    BlockPtr Chunk() {
        BlockPtr b = ParseBlock();
        if (!Check(Tok::Eof)) Fail("ожидался конец файла, а встретилось " + Describe(Peek()));
        return b;
    }

private:
    const std::vector<Token>& t;
    DiagnosticSink& d;
    size_t i = 0;
    int speculative = 0;
    int asyncDepth = 0;
    int classDepth = 0;

    // --- Токены ---------------------------------------------------------------
    const Token& Peek(size_t k = 0) const {
        const size_t j = i + k;
        return j < t.size() ? t[j] : t.back();
    }
    bool Check(Tok k, size_t off = 0) const { return Peek(off).Kind == k; }
    bool CheckWord(const char* w, size_t off = 0) const {
        return Peek(off).Kind == Tok::Name && Peek(off).Text == w;
    }
    const Token& Next() {
        const Token& tok = Peek();
        if (i < t.size() - 1) ++i;
        return tok;
    }
    bool Accept(Tok k) {
        if (!Check(k)) return false;
        Next();
        return true;
    }
    bool AcceptWord(const char* w) {
        if (!CheckWord(w)) return false;
        Next();
        return true;
    }
    static std::string Describe(const Token& tok) {
        if (tok.Kind == Tok::Eof) return "конец файла";
        return "'" + tok.Text + "'";
    }
    [[noreturn]] void Fail(const std::string& message) { FailAt(Peek().P, message); }
    [[noreturn]] void FailAt(Pos p, const std::string& message) {
        if (speculative > 0) throw SpecFail{};
        d.Error(p, "Синтаксическая ошибка", {message});
        throw ParseAbort{};
    }
    const Token& Expect(Tok k, const char* what) {
        if (!Check(k)) Fail(std::string("ожидалось ") + what + ", а встретилось " + Describe(Peek()));
        return Next();
    }
    std::string ExpectName(const char* what = "имя") {
        if (!Check(Tok::Name)) Fail(std::string("ожидалось ") + what + ", а встретилось " + Describe(Peek()));
        return Next().Text;
    }
    // '>' в записи типа; '>>' (map<string, Pool<T>>) режется пополам.
    void ExpectGt();

    // --- Блоки ----------------------------------------------------------------
    bool BlockEnds() const;

    BlockPtr ParseBlock();

    StmtPtr ParseReturn();

    // --- Операторы ------------------------------------------------------------
    StmtPtr ParseStatement();

    StmtPtr Make(Stmt::K k) {
        auto s = std::make_shared<Stmt>();
        s->Kind = k;
        s->P = Peek().P;
        return s;
    }

    // Слова SAGE в начале оператора: решаем по следующему токену.
    StmtPtr ParseContextual();

    // Что может стоять после аннотаций: класс, функция, экспорт.
    StmtPtr ParseAnnotated(std::vector<Annotation> notes);

    StmtPtr ParseExport(std::vector<Annotation> notes);

    StmtPtr ParseIf();

    StmtPtr ParseFor();

    StmtPtr ParseFunctionStat(bool async, std::vector<Annotation> notes);

    StmtPtr ParseLocal();

    // name [: T] [<attrib>] {, ...} [= list] | name := expr
    void ParseLocalNames(Stmt& s);

    StmtPtr ParseConstVar();

    StmtPtr ParseImport();

    static std::string Unquote(const std::string& raw);

    StmtPtr ParseEnum();

    StmtPtr ParseEvent();

    StmtPtr ParseTypeAlias();

    // --- Аннотации --------------------------------------------------------------
    std::vector<Annotation> ParseAnnotations();

    // --- Классы, интерфейсы, структуры -----------------------------------------
    StmtPtr ParseClass(std::vector<Annotation> notes, bool exported);

    Member ParseMember(const ClassDecl& c);

    // --- Функции ----------------------------------------------------------------
    // Подпись без тела: (params): Ret — для методов интерфейса и типов функций.
    FuncPtr ParseSignature(bool isMethod);

    void ParseParams(FuncBody& f);

    TypePtr ParseReturnType() {
        // void и кортеж (number, number) — только здесь.
        return ParseType();
    }

    FuncPtr ParseFuncBody(bool isMethod, bool async);

    // --- Типы -------------------------------------------------------------------
    TypePtr ParseType();

    TypePtr ParsePostfixType();

    TypePtr ParsePrimaryType();

    // --- Выражения --------------------------------------------------------------
    std::vector<ExprPtr> ParseExprList() {
        std::vector<ExprPtr> out;
        out.push_back(ParseExpr());
        while (Accept(Tok::Comma)) out.push_back(ParseExpr());
        return out;
    }

    ExprPtr ParseExpr() { return ParseSub(0); }

    // Приоритеты Lua 5.4: {левый, правый}.
    static bool BinaryPriority(Tok k, int& left, int& right);
    static constexpr int kUnaryPriority = 12;

    bool StartsExpr(size_t off) const;

    ExprPtr ParseSub(int limit);

    ExprPtr Leaf(Expr::K k);

    ExprPtr ParseSimple();

    // (a: number, b): number => a + b
    ExprPtr TryArrow();

    ExprPtr ParsePrimary();

    // Pool<Enemy>(...) / Assets.load<Texture>(...): пробный разбор аргументов
    // типа. Если за '>' нет '(' — это было сравнение, откатываемся.
    bool TryGenericArgs(ExprPtr& e);

    ExprPtr ParseSuffixed();

    std::vector<ExprPtr> ParseCallArgs();

    ExprPtr ParseTable();

    StmtPtr ParseExprStat();
};

} // namespace sage::lang::parse
