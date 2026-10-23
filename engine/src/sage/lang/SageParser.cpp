#include "sage/lang/SageParser.h"

#include <stdexcept>

namespace sage::lang {

namespace {

// Синтаксическая ошибка уже записана — разбор прекращается.
struct ParseAbort {};
// Пробный разбор (стрелочная функция, обобщённый вызов) не удался — откат.
struct SpecFail {};

// Слова, которые в начале члена класса означают модификатор.
bool IsModifier(const std::string& w) {
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
    void ExpectGt() {
        if (Accept(Tok::Gt)) return;
        if (Check(Tok::Shr)) {
            // Второй '>' остаётся в потоке: подменяем токен на месте.
            Token& tok = const_cast<Token&>(t[i]);
            tok.Kind = Tok::Gt;
            tok.Text = ">";
            tok.P.Col += 1;
            return;
        }
        Fail("ожидалось '>', а встретилось " + Describe(Peek()));
    }

    // --- Блоки ----------------------------------------------------------------
    bool BlockEnds() const {
        switch (Peek().Kind) {
            case Tok::End: case Tok::Else: case Tok::Elseif: case Tok::Until: case Tok::Eof:
                return true;
            default:
                return false;
        }
    }

    BlockPtr ParseBlock() {
        auto b = std::make_shared<Block>();
        while (!BlockEnds()) {
            if (Check(Tok::Return)) {
                b->Stmts.push_back(ParseReturn());
                break;
            }
            StmtPtr s = ParseStatement();
            if (s) b->Stmts.push_back(std::move(s));
        }
        b->EndP = Peek().P;
        return b;
    }

    StmtPtr ParseReturn() {
        auto s = std::make_shared<Stmt>();
        s->Kind = Stmt::K::Return;
        s->P = Next().P;
        if (!BlockEnds() && !Check(Tok::Semi)) s->Exprs = ParseExprList();
        Accept(Tok::Semi);
        return s;
    }

    // --- Операторы ------------------------------------------------------------
    StmtPtr ParseStatement() {
        const Token& tok = Peek();
        switch (tok.Kind) {
            case Tok::Semi: Next(); return nullptr;
            case Tok::If: return ParseIf();
            case Tok::While: {
                auto s = Make(Stmt::K::While);
                Next();
                s->Cond = ParseExpr();
                Expect(Tok::Do, "'do'");
                s->Body = ParseBlock();
                Expect(Tok::End, "'end' (закрывает while)");
                return s;
            }
            case Tok::Do: {
                auto s = Make(Stmt::K::Do);
                Next();
                s->Body = ParseBlock();
                Expect(Tok::End, "'end' (закрывает do)");
                return s;
            }
            case Tok::For: return ParseFor();
            case Tok::Repeat: {
                auto s = Make(Stmt::K::Repeat);
                Next();
                s->Body = ParseBlock();
                Expect(Tok::Until, "'until'");
                s->Cond = ParseExpr();
                return s;
            }
            case Tok::Function: return ParseFunctionStat(false, {});
            case Tok::Local: return ParseLocal();
            case Tok::DColon: {
                auto s = Make(Stmt::K::Label);
                Next();
                s->Text = ExpectName("имя метки");
                Expect(Tok::DColon, "'::'");
                return s;
            }
            case Tok::Break: {
                auto s = Make(Stmt::K::Break);
                Next();
                return s;
            }
            case Tok::Goto: {
                auto s = Make(Stmt::K::Goto);
                Next();
                s->Text = ExpectName("имя метки");
                return s;
            }
            case Tok::At: {
                std::vector<Annotation> notes = ParseAnnotations();
                return ParseAnnotated(std::move(notes));
            }
            case Tok::Name: {
                if (StmtPtr s = ParseContextual()) return s;
                break;
            }
            default: break;
        }
        return ParseExprStat();
    }

    StmtPtr Make(Stmt::K k) {
        auto s = std::make_shared<Stmt>();
        s->Kind = k;
        s->P = Peek().P;
        return s;
    }

    // Слова SAGE в начале оператора: решаем по следующему токену.
    StmtPtr ParseContextual() {
        const std::string& w = Peek().Text;
        if ((w == "class" || w == "interface" || w == "struct") && Check(Tok::Name, 1))
            return ParseClass({}, false);
        if (w == "final" && CheckWord("class", 1) && Check(Tok::Name, 2)) return ParseClass({}, false);
        if (w == "enum" && Check(Tok::Name, 1)) return ParseEnum();
        if (w == "event" && Check(Tok::Name, 1) && Check(Tok::LParen, 2)) return ParseEvent();
        if (w == "type" && Check(Tok::Name, 1) && (Check(Tok::Assign, 2) || Check(Tok::Lt, 2)))
            return ParseTypeAlias();
        if ((w == "const" || w == "var") && Check(Tok::Name, 1) &&
            (Check(Tok::Colon, 2) || Check(Tok::Assign, 2) || Check(Tok::Walrus, 2) ||
             (w == "var" && !Check(Tok::LParen, 2) && !Check(Tok::Dot, 2))))
            return ParseConstVar();
        if (w == "import" && (Check(Tok::LBrace, 1) || Check(Tok::Star, 1) || Check(Tok::String, 1)))
            return ParseImport();
        if (w == "export" && (Check(Tok::Name, 1) || Check(Tok::Function, 1) || Check(Tok::Local, 1)))
            return ParseExport({});
        if (w == "async" && Check(Tok::Function, 1)) {
            Next();
            return ParseFunctionStat(true, {});
        }
        return nullptr;
    }

    // Что может стоять после аннотаций: класс, функция, экспорт.
    StmtPtr ParseAnnotated(std::vector<Annotation> notes) {
        if (CheckWord("export")) return ParseExport(std::move(notes));
        if ((CheckWord("class") || CheckWord("interface") || CheckWord("struct") ||
             (CheckWord("final") && CheckWord("class", 1))) && Check(Tok::Name, CheckWord("final") ? 2 : 1))
            return ParseClass(std::move(notes), false);
        if (Check(Tok::Function)) return ParseFunctionStat(false, std::move(notes));
        if (CheckWord("async") && Check(Tok::Function, 1)) {
            Next();
            return ParseFunctionStat(true, std::move(notes));
        }
        if (Check(Tok::Local) && Check(Tok::Function, 1)) {
            StmtPtr s = ParseLocal();
            s->Annotations = std::move(notes);
            return s;
        }
        FailAt(notes.empty() ? Peek().P : notes.front().P,
               "аннотация должна стоять перед объявлением класса, поля, метода или функции, "
               "а за ней идёт " + Describe(Peek()));
    }

    StmtPtr ParseExport(std::vector<Annotation> notes) {
        const Pos p = Next().P;   // export
        if (Check(Tok::At)) {
            std::vector<Annotation> more = ParseAnnotations();
            notes.insert(notes.end(), more.begin(), more.end());
        }
        StmtPtr s;
        if ((CheckWord("class") || CheckWord("interface") || CheckWord("struct") ||
             (CheckWord("final") && CheckWord("class", 1))))
            s = ParseClass(std::move(notes), true);
        else if (Check(Tok::Function)) s = ParseFunctionStat(false, std::move(notes));
        else if (CheckWord("async") && Check(Tok::Function, 1)) {
            Next();
            s = ParseFunctionStat(true, std::move(notes));
        } else if (CheckWord("enum")) s = ParseEnum();
        else if (CheckWord("event")) s = ParseEvent();
        else if (CheckWord("type")) s = ParseTypeAlias();
        else if (CheckWord("const") || CheckWord("var")) s = ParseConstVar();
        else if (Check(Tok::Local)) s = ParseLocal();
        else FailAt(p, "после export ожидалось объявление (class, function, enum, const…), а встретилось " +
                           Describe(Peek()));
        s->Exported = true;
        if (s->Kind == Stmt::K::Function && (s->FuncName.size() != 1 || !s->Method.empty()))
            FailAt(p, "экспортировать можно только функцию с простым именем");
        return s;
    }

    StmtPtr ParseIf() {
        auto s = Make(Stmt::K::If);
        Next();
        ExprPtr cond = ParseExpr();
        Expect(Tok::Then, "'then'");
        s->Clauses.emplace_back(cond, ParseBlock());
        while (Check(Tok::Elseif)) {
            Next();
            ExprPtr c = ParseExpr();
            Expect(Tok::Then, "'then'");
            s->Clauses.emplace_back(c, ParseBlock());
        }
        if (Accept(Tok::Else)) s->Else = ParseBlock();
        Expect(Tok::End, "'end' (закрывает if)");
        return s;
    }

    StmtPtr ParseFor() {
        const Pos p = Next().P;
        std::string first = ExpectName("имя переменной цикла");
        // Тип переменной цикла допускается и отбрасывается: for i: integer = 1, 10.
        TypePtr firstType;
        if (Accept(Tok::Colon)) firstType = ParseType();
        if (Check(Tok::Assign)) {
            auto s = std::make_shared<Stmt>();
            s->Kind = Stmt::K::NumFor;
            s->P = p;
            Next();
            s->Names.push_back(first);
            s->Types.push_back(firstType);
            s->Exprs.push_back(ParseExpr());
            Expect(Tok::Comma, "','");
            s->Exprs.push_back(ParseExpr());
            if (Accept(Tok::Comma)) s->Exprs.push_back(ParseExpr());
            Expect(Tok::Do, "'do'");
            s->Body = ParseBlock();
            Expect(Tok::End, "'end' (закрывает for)");
            return s;
        }
        auto s = std::make_shared<Stmt>();
        s->Kind = Stmt::K::GenFor;
        s->P = p;
        s->Names.push_back(first);
        s->Types.push_back(firstType);
        while (Accept(Tok::Comma)) {
            s->Names.push_back(ExpectName("имя переменной цикла"));
            TypePtr ty;
            if (Accept(Tok::Colon)) ty = ParseType();
            s->Types.push_back(ty);
        }
        Expect(Tok::In, "'in' или '='");
        s->Exprs = ParseExprList();
        Expect(Tok::Do, "'do'");
        s->Body = ParseBlock();
        Expect(Tok::End, "'end' (закрывает for)");
        return s;
    }

    StmtPtr ParseFunctionStat(bool async, std::vector<Annotation> notes) {
        auto s = Make(Stmt::K::Function);
        Expect(Tok::Function, "'function'");
        s->FuncName.push_back(ExpectName("имя функции"));
        while (Accept(Tok::Dot)) s->FuncName.push_back(ExpectName("имя поля"));
        if (Accept(Tok::Colon)) s->Method = ExpectName("имя метода");
        s->Fn = ParseFuncBody(!s->Method.empty(), async);
        s->Annotations = std::move(notes);
        return s;
    }

    StmtPtr ParseLocal() {
        const Pos p = Next().P;   // local
        if (Check(Tok::Function) || (CheckWord("async") && Check(Tok::Function, 1))) {
            const bool async = AcceptWord("async");
            Next();
            auto s = std::make_shared<Stmt>();
            s->Kind = Stmt::K::LocalFunction;
            s->P = p;
            s->FuncName.push_back(ExpectName("имя функции"));
            s->Fn = ParseFuncBody(false, async);
            return s;
        }
        auto s = std::make_shared<Stmt>();
        s->Kind = Stmt::K::Local;
        s->P = p;
        s->DeclKind = Stmt::Decl::Local;
        ParseLocalNames(*s);
        return s;
    }

    // name [: T] [<attrib>] {, ...} [= list] | name := expr
    void ParseLocalNames(Stmt& s) {
        do {
            s.Names.push_back(ExpectName("имя переменной"));
            TypePtr ty;
            if (Accept(Tok::Colon)) ty = ParseType();
            s.Types.push_back(ty);
            std::string attrib;
            if (Check(Tok::Lt)) {
                Next();
                attrib = ExpectName("атрибут (const или close)");
                ExpectGt();
            }
            s.Attribs.push_back(attrib);
        } while (Accept(Tok::Comma));
        if (Accept(Tok::Walrus)) {
            if (s.Names.size() != 1) FailAt(s.P, "':=' объявляет ровно одну переменную");
            if (s.Types[0]) FailAt(s.P, "':=' выводит тип сам — запись типа здесь лишняя");
            s.DeclKind = Stmt::Decl::Infer;
            s.Exprs.push_back(ParseExpr());
            return;
        }
        if (Accept(Tok::Assign)) s.Exprs = ParseExprList();
    }

    StmtPtr ParseConstVar() {
        auto s = Make(Stmt::K::Local);
        const bool isConst = Next().Text == "const";
        s->DeclKind = isConst ? Stmt::Decl::Const : Stmt::Decl::Var;
        ParseLocalNames(*s);
        if (isConst && s->Exprs.empty()) FailAt(s->P, "константа обязана получить значение: const " +
                                                       s->Names[0] + " = ...");
        if (s->DeclKind == Stmt::Decl::Infer && isConst) s->DeclKind = Stmt::Decl::Const;
        return s;
    }

    StmtPtr ParseImport() {
        auto s = Make(Stmt::K::Import);
        Next();   // import
        if (Check(Tok::String)) {
            s->FromP = Peek().P;
            s->From = Unquote(Next().Text);
            return s;
        }
        if (Accept(Tok::Star)) {
            if (!AcceptWord("as")) Fail("ожидалось 'as' после 'import *'");
            s->ImportAll = ExpectName("имя модуля");
        } else {
            Expect(Tok::LBrace, "'{'");
            if (!Check(Tok::RBrace)) {
                do {
                    if (Check(Tok::RBrace)) break;
                    Stmt::ImportName n;
                    n.P = Peek().P;
                    n.Name = ExpectName("имя из модуля");
                    n.As = n.Name;
                    if (AcceptWord("as")) n.As = ExpectName("новое имя");
                    s->Imports.push_back(n);
                } while (Accept(Tok::Comma));
            }
            Expect(Tok::RBrace, "'}'");
        }
        if (!AcceptWord("from")) Fail("ожидалось 'from \"путь\"'");
        s->FromP = Peek().P;
        s->From = Unquote(Expect(Tok::String, "строка с путём модуля").Text);
        return s;
    }

    static std::string Unquote(const std::string& raw) {
        if (raw.size() >= 2 && (raw[0] == '"' || raw[0] == '\'')) return raw.substr(1, raw.size() - 2);
        if (raw.size() >= 4 && raw[0] == '[') {
            size_t open = raw.find('[', 1);
            size_t level = open - 1;
            return raw.substr(open + 1, raw.size() - (open + 1) - (level + 2));
        }
        return raw;
    }

    StmtPtr ParseEnum() {
        auto s = Make(Stmt::K::Enum);
        Next();   // enum
        auto e = std::make_shared<EnumDecl>();
        e->P = Peek().P;
        e->Name = ExpectName("имя перечисления");
        while (!Check(Tok::End)) {
            if (Check(Tok::Eof)) Fail("перечисление " + e->Name + " не закрыто 'end'");
            EnumDecl::Item item;
            item.P = Peek().P;
            item.Name = ExpectName("имя значения перечисления");
            if (Accept(Tok::Assign)) item.Value = ParseExpr();
            e->Items.push_back(std::move(item));
            if (!Accept(Tok::Comma)) Accept(Tok::Semi);
        }
        e->EndP = Next().P;
        s->Enum = e;
        return s;
    }

    StmtPtr ParseEvent() {
        auto s = Make(Stmt::K::Event);
        Next();   // event
        auto e = std::make_shared<EventDecl>();
        e->P = Peek().P;
        e->Name = ExpectName("имя события");
        Expect(Tok::LParen, "'('");
        if (!Check(Tok::RParen)) {
            do {
                Param prm;
                prm.P = Peek().P;
                prm.Name = ExpectName("имя параметра события");
                if (Accept(Tok::Colon)) prm.Type = ParseType();
                e->Params.push_back(std::move(prm));
            } while (Accept(Tok::Comma));
        }
        Expect(Tok::RParen, "')'");
        s->Event = e;
        return s;
    }

    StmtPtr ParseTypeAlias() {
        auto s = Make(Stmt::K::TypeAlias);
        Next();   // type
        s->Text = ExpectName("имя типа");
        if (Accept(Tok::Lt)) {
            do s->TypeParams.push_back(ExpectName("параметр типа"));
            while (Accept(Tok::Comma));
            ExpectGt();
        }
        Expect(Tok::Assign, "'='");
        s->Alias = ParseType();
        return s;
    }

    // --- Аннотации --------------------------------------------------------------
    std::vector<Annotation> ParseAnnotations() {
        std::vector<Annotation> out;
        while (Check(Tok::At)) {
            Annotation a;
            a.P = Next().P;
            a.Name = ExpectName("имя аннотации");
            if (Accept(Tok::LParen)) {
                a.HasParens = true;
                if (!Check(Tok::RParen)) {
                    do {
                        if (Check(Tok::RParen)) break;
                        if (Check(Tok::Name) && Check(Tok::Assign, 1)) {
                            std::string key = Next().Text;
                            Next();
                            a.Named.emplace_back(key, ParseExpr());
                        } else {
                            a.Positional.push_back(ParseExpr());
                        }
                    } while (Accept(Tok::Comma));
                }
                Expect(Tok::RParen, "')' (закрывает аннотацию)");
            }
            out.push_back(std::move(a));
        }
        return out;
    }

    // --- Классы, интерфейсы, структуры -----------------------------------------
    StmtPtr ParseClass(std::vector<Annotation> notes, bool exported) {
        auto s = Make(Stmt::K::Class);
        s->Exported = exported;
        auto c = std::make_shared<ClassDecl>();
        c->Annotations = std::move(notes);
        if (AcceptWord("final")) c->Final = true;
        const std::string kw = Next().Text;
        c->Kind = kw == "interface" ? ClassDecl::K::Interface
                : kw == "struct" ? ClassDecl::K::Struct : ClassDecl::K::Class;
        c->P = Peek().P;
        c->Name = ExpectName("имя типа");
        if (Accept(Tok::Lt)) {
            do c->TypeParams.push_back(ExpectName("параметр типа"));
            while (Accept(Tok::Comma));
            ExpectGt();
        }
        if (AcceptWord("extends")) {
            if (c->Kind == ClassDecl::K::Interface) {
                do c->Implements.push_back(ParseType());
                while (Accept(Tok::Comma));
            } else {
                c->Extends = ParseType();
            }
        }
        if (AcceptWord("implements")) {
            do c->Implements.push_back(ParseType());
            while (Accept(Tok::Comma));
        }
        ++classDepth;
        while (!Check(Tok::End)) {
            if (Check(Tok::Eof)) FailAt(c->P, kw + " " + c->Name + " не закрыт 'end'");
            if (Accept(Tok::Semi)) continue;
            c->Members.push_back(ParseMember(*c));
        }
        --classDepth;
        c->EndP = Next().P;
        s->Class = c;
        return s;
    }

    Member ParseMember(const ClassDecl& c) {
        Member m;
        m.Annotations = ParseAnnotations();
        m.P = Peek().P;
        bool async = false;
        // Модификаторы. Слово-модификатор, за которым идёт ':' или '=', — это
        // имя поля (поле может называться static), а не модификатор.
        while (Check(Tok::Name) && IsModifier(Peek().Text) && !Check(Tok::Colon, 1) &&
               !Check(Tok::Assign, 1)) {
            const std::string w = Next().Text;
            if (w == "public" || w == "private" || w == "protected") {
                m.Access = w;
                m.AccessWritten = true;
            } else if (w == "static") m.Static = true;
            else if (w == "override") m.Override = true;
            else if (w == "final") m.Final = true;
            else if (w == "async") async = true;
            else if (w == "readonly") m.Const = true;
        }
        if (Check(Tok::Function)) {
            Next();
            m.Kind = Member::K::Method;
            m.P = Peek().P;
            m.Name = ExpectName("имя метода");
            if (c.Kind == ClassDecl::K::Interface) {
                m.Abstract = true;
                m.Fn = ParseSignature(true);
            } else {
                m.Fn = ParseFuncBody(!m.Static, async);
            }
            m.Fn->Async = async;
            return m;
        }
        if (async) FailAt(m.P, "async применим только к методу");
        if (CheckWord("const") && Check(Tok::Name, 1)) {
            Next();
            m.Const = true;
        } else if (CheckWord("var") && Check(Tok::Name, 1)) {
            Next();
        }
        if (!Check(Tok::Name))
            Fail("в теле " + c.Name + " ожидалось поле или метод, а встретилось " + Describe(Peek()));
        m.Kind = Member::K::Field;
        m.P = Peek().P;
        m.Name = Next().Text;
        if (Accept(Tok::Colon)) m.Type = ParseType();
        if (Accept(Tok::Assign)) m.Default = ParseExpr();
        if (!m.Type && !m.Default)
            FailAt(m.P, "поле " + m.Name + " объявлено без типа и без значения: " + m.Name +
                            ": number = 0");
        Accept(Tok::Comma);
        return m;
    }

    // --- Функции ----------------------------------------------------------------
    // Подпись без тела: (params): Ret — для методов интерфейса и типов функций.
    FuncPtr ParseSignature(bool isMethod) {
        auto f = std::make_shared<FuncBody>();
        f->P = Peek().P;
        f->IsMethod = isMethod;
        if (Accept(Tok::Lt)) {
            do f->TypeParams.push_back(ExpectName("параметр типа"));
            while (Accept(Tok::Comma));
            ExpectGt();
        }
        ParseParams(*f);
        if (Accept(Tok::Colon)) {
            f->Ret = ParseReturnType();
            f->HasRet = true;
        }
        return f;
    }

    void ParseParams(FuncBody& f) {
        Expect(Tok::LParen, "'('");
        if (!Check(Tok::RParen)) {
            do {
                if (Check(Tok::Ellipsis)) {
                    Next();
                    f.Vararg = true;
                    if (Check(Tok::Name)) f.VarargName = Next().Text;
                    if (Accept(Tok::Colon)) f.VarargType = ParseType();
                    break;
                }
                Param prm;
                prm.P = Peek().P;
                prm.Name = ExpectName("имя параметра");
                if (Accept(Tok::Colon)) prm.Type = ParseType();
                if (Accept(Tok::Assign)) prm.Default = ParseExpr();
                f.Params.push_back(std::move(prm));
            } while (Accept(Tok::Comma));
        }
        Expect(Tok::RParen, "')'");
    }

    TypePtr ParseReturnType() {
        // void и кортеж (number, number) — только здесь.
        return ParseType();
    }

    FuncPtr ParseFuncBody(bool isMethod, bool async) {
        FuncPtr f = ParseSignature(isMethod);
        f->Async = async;
        // await — только в СВОЁМ теле async-функции: обычная функция внутри
        // async (обработчик, замыкание) ждать не может, её никто не возобновит.
        const int saved = asyncDepth;
        asyncDepth = async ? 1 : 0;
        f->Body = ParseBlock();
        asyncDepth = saved;
        f->EndP = Peek().P;
        Expect(Tok::End, "'end' (закрывает function)");
        return f;
    }

    // --- Типы -------------------------------------------------------------------
    TypePtr ParseType() {
        TypePtr first = ParsePostfixType();
        if (!Check(Tok::Pipe)) return first;
        auto u = std::make_shared<TypeExpr>();
        u->Kind = TypeExpr::K::Union;
        u->P = first->P;
        u->Args.push_back(first);
        while (Accept(Tok::Pipe)) u->Args.push_back(ParsePostfixType());
        return u;
    }

    TypePtr ParsePostfixType() {
        TypePtr ty = ParsePrimaryType();
        for (;;) {
            if (Check(Tok::Question)) {
                Next();
                auto o = std::make_shared<TypeExpr>();
                o->Kind = TypeExpr::K::Optional;
                o->P = ty->P;
                o->Args.push_back(ty);
                ty = o;
                continue;
            }
            if (Check(Tok::LBracket) && Check(Tok::RBracket, 1)) {
                Next();
                Next();
                auto a = std::make_shared<TypeExpr>();
                a->Kind = TypeExpr::K::Array;
                a->P = ty->P;
                a->Args.push_back(ty);
                ty = a;
                continue;
            }
            return ty;
        }
    }

    TypePtr ParsePrimaryType() {
        auto ty = std::make_shared<TypeExpr>();
        ty->P = Peek().P;
        if (Accept(Tok::Nil)) {
            ty->Name = "nil";
            return ty;
        }
        if (Check(Tok::Function)) {
            Next();
            ty->Kind = TypeExpr::K::Function;
            Expect(Tok::LParen, "'('");
            if (!Check(Tok::RParen)) {
                do {
                    if (Accept(Tok::Ellipsis)) {
                        if (Accept(Tok::Colon)) ty->Args.push_back(ParseType());
                        break;
                    }
                    // Имя параметра необязательно: function(value: T) и function(T).
                    if (Check(Tok::Name) && Check(Tok::Colon, 1)) {
                        Next();
                        Next();
                    }
                    ty->Args.push_back(ParseType());
                } while (Accept(Tok::Comma));
            }
            Expect(Tok::RParen, "')'");
            if (Accept(Tok::Colon)) ty->Ret = ParseType();
            return ty;
        }
        if (Accept(Tok::LParen)) {
            TypePtr inner = ParseType();
            if (Check(Tok::Comma)) {
                ty->Kind = TypeExpr::K::Tuple;
                ty->Args.push_back(inner);
                while (Accept(Tok::Comma)) ty->Args.push_back(ParseType());
                Expect(Tok::RParen, "')'");
                return ty;
            }
            Expect(Tok::RParen, "')'");
            return inner;
        }
        if (Accept(Tok::LBrace)) {
            ty->Kind = TypeExpr::K::Table;
            while (!Check(Tok::RBrace)) {
                std::string name = ExpectName("имя поля");
                Expect(Tok::Colon, "':'");
                ty->Fields.emplace_back(name, ParseType());
                if (!Accept(Tok::Comma)) Accept(Tok::Semi);
            }
            Expect(Tok::RBrace, "'}'");
            return ty;
        }
        ty->Name = ExpectName("тип");
        while (Check(Tok::Dot) && Check(Tok::Name, 1)) {   // Module.Type
            Next();
            ty->Name += "." + Next().Text;
        }
        if (Accept(Tok::Lt)) {
            do ty->Args.push_back(ParseType());
            while (Accept(Tok::Comma));
            ExpectGt();
        }
        return ty;
    }

    // --- Выражения --------------------------------------------------------------
    std::vector<ExprPtr> ParseExprList() {
        std::vector<ExprPtr> out;
        out.push_back(ParseExpr());
        while (Accept(Tok::Comma)) out.push_back(ParseExpr());
        return out;
    }

    ExprPtr ParseExpr() { return ParseSub(0); }

    // Приоритеты Lua 5.4: {левый, правый}.
    static bool BinaryPriority(Tok k, int& left, int& right) {
        switch (k) {
            case Tok::Or: left = right = 1; return true;
            case Tok::And: left = right = 2; return true;
            case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge: case Tok::Ne: case Tok::Eq:
                left = right = 3; return true;
            case Tok::Pipe: left = right = 4; return true;
            case Tok::Tilde: left = right = 5; return true;
            case Tok::Amp: left = right = 6; return true;
            case Tok::Shl: case Tok::Shr: left = right = 7; return true;
            case Tok::Concat: left = 9; right = 8; return true;
            case Tok::Plus: case Tok::Minus: left = right = 10; return true;
            case Tok::Star: case Tok::Slash: case Tok::DSlash: case Tok::Percent:
                left = right = 11; return true;
            case Tok::Caret: left = 14; right = 13; return true;
            default: return false;
        }
    }
    static constexpr int kUnaryPriority = 12;

    bool StartsExpr(size_t off) const {
        switch (Peek(off).Kind) {
            case Tok::Name: case Tok::Number: case Tok::String: case Tok::Nil: case Tok::True:
            case Tok::False: case Tok::Ellipsis: case Tok::LParen: case Tok::LBrace:
            case Tok::Function: case Tok::Minus: case Tok::Not: case Tok::Hash: case Tok::Tilde:
                return true;
            default:
                return false;
        }
    }

    ExprPtr ParseSub(int limit) {
        ExprPtr left;
        const Token& tok = Peek();
        if (tok.Kind == Tok::Not || tok.Kind == Tok::Minus || tok.Kind == Tok::Hash ||
            tok.Kind == Tok::Tilde) {
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::K::Unary;
            e->P = tok.P;
            e->Op = Next().Kind;
            e->A = ParseSub(kUnaryPriority);
            left = e;
        } else if (tok.Kind == Tok::Name && tok.Text == "await" &&
                   ((asyncDepth > 0 && StartsExpr(1)) || (asyncDepth == 0 && Check(Tok::Name, 1)))) {
            // `await x` вне async — не имя переменной (два имени подряд в Lua
            // не выражение), а ошибка, о которой стоит сказать прямо.
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::K::Await;
            if (asyncDepth == 0) e->Text = "outside";
            e->P = Next().P;
            e->A = ParseSub(kUnaryPriority);
            left = e;
        } else {
            left = ParseSimple();
        }
        int lp = 0, rp = 0;
        while (BinaryPriority(Peek().Kind, lp, rp) && lp > limit) {
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::K::Binary;
            e->P = Peek().P;
            e->Op = Next().Kind;
            e->A = left;
            e->B = ParseSub(rp);
            left = e;
        }
        return left;
    }

    ExprPtr Leaf(Expr::K k) {
        auto e = std::make_shared<Expr>();
        e->Kind = k;
        e->P = Peek().P;
        e->Text = Next().Text;
        return e;
    }

    ExprPtr ParseSimple() {
        switch (Peek().Kind) {
            case Tok::Number: return Leaf(Expr::K::Number);
            case Tok::String: return Leaf(Expr::K::String);
            case Tok::Nil: return Leaf(Expr::K::Nil);
            case Tok::True: return Leaf(Expr::K::True);
            case Tok::False: return Leaf(Expr::K::False);
            case Tok::Ellipsis: return Leaf(Expr::K::Vararg);
            case Tok::LBrace: return ParseTable();
            case Tok::Function: {
                auto e = std::make_shared<Expr>();
                e->Kind = Expr::K::Function;
                e->P = Next().P;
                e->Fn = ParseFuncBody(false, false);
                return e;
            }
            case Tok::Name:
                if (Peek().Text == "async" && Check(Tok::Function, 1)) {
                    auto e = std::make_shared<Expr>();
                    e->Kind = Expr::K::Function;
                    e->P = Next().P;
                    Next();
                    e->Fn = ParseFuncBody(false, true);
                    return e;
                }
                break;
            case Tok::LParen:
                if (ExprPtr arrow = TryArrow()) return arrow;
                break;
            default: break;
        }
        return ParseSuffixed();
    }

    // (a: number, b): number => a + b
    ExprPtr TryArrow() {
        const size_t save = i;
        ++speculative;
        try {
            auto f = std::make_shared<FuncBody>();
            f->P = Peek().P;
            ParseParams(*f);
            if (Accept(Tok::Colon)) {
                f->Ret = ParseType();
                f->HasRet = true;
            }
            if (!Check(Tok::Arrow)) throw SpecFail{};
            --speculative;
            Next();   // =>
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::K::Function;
            e->P = f->P;
            const int saved = asyncDepth;
            asyncDepth = 0;
            f->ArrowExpr = ParseExpr();
            asyncDepth = saved;
            f->EndP = Peek().P;
            e->Fn = f;
            return e;
        } catch (const SpecFail&) {
            --speculative;
            i = save;
            return nullptr;
        }
    }

    ExprPtr ParsePrimary() {
        const Token& tok = Peek();
        if (tok.Kind == Tok::Name) {
            if (tok.Text == "super" && classDepth > 0) return Leaf(Expr::K::Super);
            return Leaf(Expr::K::Name);
        }
        if (tok.Kind == Tok::LParen) {
            auto e = std::make_shared<Expr>();
            e->Kind = Expr::K::Paren;
            e->P = Next().P;
            e->A = ParseExpr();
            Expect(Tok::RParen, "')'");
            return e;
        }
        Fail("ожидалось выражение, а встретилось " + Describe(tok));
    }

    // Pool<Enemy>(...) / Assets.load<Texture>(...): пробный разбор аргументов
    // типа. Если за '>' нет '(' — это было сравнение, откатываемся.
    bool TryGenericArgs(ExprPtr& e) {
        const size_t save = i;
        ++speculative;
        try {
            Next();   // <
            std::vector<TypePtr> args;
            do args.push_back(ParseType());
            while (Accept(Tok::Comma));
            ExpectGt();
            if (!Check(Tok::LParen)) throw SpecFail{};
            --speculative;
            auto g = std::make_shared<Expr>();
            g->Kind = Expr::K::Generic;
            g->P = e->P;
            g->A = e;
            g->TypeArgs = std::move(args);
            e = g;
            return true;
        } catch (const SpecFail&) {
            --speculative;
            i = save;
            return false;
        }
    }

    ExprPtr ParseSuffixed() {
        ExprPtr e = ParsePrimary();
        for (;;) {
            switch (Peek().Kind) {
                case Tok::Dot: {
                    auto f = std::make_shared<Expr>();
                    f->Kind = Expr::K::Field;
                    Next();   // .
                    f->P = Peek().P;
                    f->A = e;
                    f->Text = ExpectName("имя поля");
                    e = f;
                    continue;
                }
                case Tok::LBracket: {
                    auto x = std::make_shared<Expr>();
                    x->Kind = Expr::K::Index;
                    x->P = Next().P;
                    x->A = e;
                    x->B = ParseExpr();
                    Expect(Tok::RBracket, "']'");
                    e = x;
                    continue;
                }
                case Tok::Colon: {
                    auto m = std::make_shared<Expr>();
                    m->Kind = Expr::K::MethodCall;
                    m->P = Next().P;
                    m->A = e;
                    m->Text = ExpectName("имя метода");
                    m->Args = ParseCallArgs();
                    e = m;
                    continue;
                }
                case Tok::Lt:
                    if ((e->Kind == Expr::K::Name || e->Kind == Expr::K::Field) && TryGenericArgs(e))
                        continue;
                    return e;
                case Tok::LParen: case Tok::String: case Tok::LBrace: {
                    // Вызов на СЛЕДУЮЩЕЙ строке после выражения в Lua — двусмысленность
                    // ("a\n(f)()"); Lua сам решает в пользу вызова, делаем так же.
                    auto c = std::make_shared<Expr>();
                    c->Kind = Expr::K::Call;
                    c->P = Peek().P;
                    c->A = e;
                    c->Args = ParseCallArgs();
                    e = c;
                    continue;
                }
                default:
                    return e;
            }
        }
    }

    std::vector<ExprPtr> ParseCallArgs() {
        std::vector<ExprPtr> args;
        if (Check(Tok::String)) {
            args.push_back(Leaf(Expr::K::String));
            return args;
        }
        if (Check(Tok::LBrace)) {
            args.push_back(ParseTable());
            return args;
        }
        Expect(Tok::LParen, "'(' (аргументы вызова)");
        if (!Check(Tok::RParen)) args = ParseExprList();
        Expect(Tok::RParen, "')'");
        return args;
    }

    ExprPtr ParseTable() {
        auto e = std::make_shared<Expr>();
        e->Kind = Expr::K::Table;
        e->P = Next().P;   // {
        while (!Check(Tok::RBrace)) {
            Expr::TField f;
            f.P = Peek().P;
            if (Check(Tok::LBracket)) {
                Next();
                f.Kind = Expr::TField::K::Keyed;
                f.Key = ParseExpr();
                Expect(Tok::RBracket, "']'");
                Expect(Tok::Assign, "'='");
                f.Value = ParseExpr();
            } else if (Check(Tok::Name) && Check(Tok::Assign, 1)) {
                f.Kind = Expr::TField::K::Named;
                f.Name = Next().Text;
                Next();
                f.Value = ParseExpr();
            } else {
                f.Kind = Expr::TField::K::Positional;
                f.Value = ParseExpr();
            }
            e->Fields.push_back(std::move(f));
            if (!Accept(Tok::Comma) && !Accept(Tok::Semi)) break;
        }
        Expect(Tok::RBrace, "'}' (закрывает таблицу)");
        return e;
    }

    StmtPtr ParseExprStat() {
        const Pos p = Peek().P;
        // `await Time.wait(1)` — оператор сам по себе.
        if (CheckWord("await") && (asyncDepth > 0 ? StartsExpr(1) : Check(Tok::Name, 1))) {
            auto s = std::make_shared<Stmt>();
            s->Kind = Stmt::K::Call;
            s->P = p;
            s->CallE = ParseExpr();
            return s;
        }
        ExprPtr e = ParseSuffixed();
        if (Check(Tok::Assign) || Check(Tok::Comma)) {
            auto s = std::make_shared<Stmt>();
            s->Kind = Stmt::K::Assign;
            s->P = p;
            s->Targets.push_back(e);
            while (Accept(Tok::Comma)) s->Targets.push_back(ParseSuffixed());
            Expect(Tok::Assign, "'='");
            s->Exprs = ParseExprList();
            for (const ExprPtr& target : s->Targets)
                if (target->Kind != Expr::K::Name && target->Kind != Expr::K::Field &&
                    target->Kind != Expr::K::Index)
                    FailAt(target->P, "присвоить можно только переменной или полю");
            return s;
        }
        if (e->Kind != Expr::K::Call && e->Kind != Expr::K::MethodCall)
            FailAt(p, "выражение само по себе не оператор — ожидался вызов или присваивание");
        auto s = std::make_shared<Stmt>();
        s->Kind = Stmt::K::Call;
        s->P = p;
        s->CallE = e;
        return s;
    }
};

} // namespace

BlockPtr Parse(const std::vector<Token>& tokens, DiagnosticSink& diag) {
    Parser p(tokens, diag);
    try {
        return p.Chunk();
    } catch (const ParseAbort&) {
        return nullptr;
    } catch (const SpecFail&) {
        diag.Error(Pos{}, "Синтаксическая ошибка", {"внутренняя ошибка разбора"});
        return nullptr;
    }
}

std::string TypeText(const TypePtr& t) {
    if (!t) return "any";
    switch (t->Kind) {
        case TypeExpr::K::Name: {
            std::string s = t->Name;
            if (!t->Args.empty()) {
                s += "<";
                for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
                s += ">";
            }
            return s;
        }
        case TypeExpr::K::Optional: return TypeText(t->Args[0]) + "?";
        case TypeExpr::K::Array: return TypeText(t->Args[0]) + "[]";
        case TypeExpr::K::Union: {
            std::string s;
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? " | " : "") + TypeText(t->Args[k]);
            return s;
        }
        case TypeExpr::K::Tuple: {
            std::string s = "(";
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
            return s + ")";
        }
        case TypeExpr::K::Function: {
            std::string s = "function(";
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
            s += ")";
            if (t->Ret) s += ": " + TypeText(t->Ret);
            return s;
        }
        case TypeExpr::K::Table: {
            std::string s = "{ ";
            for (size_t k = 0; k < t->Fields.size(); ++k)
                s += (k ? ", " : "") + t->Fields[k].first + ": " + TypeText(t->Fields[k].second);
            return s + " }";
        }
    }
    return "any";
}

} // namespace sage::lang
