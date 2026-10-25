#include "sage/lang/SageParserDetail.h"

// Операторы, блоки и аннотации (см. SageParserDetail.h).
namespace sage::lang::parse {

// '>' в записи типа; '>>' (map<string, Pool<T>>) режется пополам.
void Parser::ExpectGt() {
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

bool Parser::BlockEnds() const {
    switch (Peek().Kind) {
        case Tok::End: case Tok::Else: case Tok::Elseif: case Tok::Until: case Tok::Eof:
            return true;
        default:
            return false;
    }
}

BlockPtr Parser::ParseBlock() {
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

StmtPtr Parser::ParseReturn() {
    auto s = std::make_shared<Stmt>();
    s->Kind = Stmt::K::Return;
    s->P = Next().P;
    if (!BlockEnds() && !Check(Tok::Semi)) s->Exprs = ParseExprList();
    Accept(Tok::Semi);
    return s;
}

StmtPtr Parser::ParseStatement() {
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

// Слова SAGE в начале оператора: решаем по следующему токену.
StmtPtr Parser::ParseContextual() {
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
StmtPtr Parser::ParseAnnotated(std::vector<Annotation> notes) {
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

StmtPtr Parser::ParseExport(std::vector<Annotation> notes) {
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

StmtPtr Parser::ParseIf() {
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

StmtPtr Parser::ParseFor() {
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

StmtPtr Parser::ParseFunctionStat(bool async, std::vector<Annotation> notes) {
    auto s = Make(Stmt::K::Function);
    Expect(Tok::Function, "'function'");
    s->FuncName.push_back(ExpectName("имя функции"));
    while (Accept(Tok::Dot)) s->FuncName.push_back(ExpectName("имя поля"));
    if (Accept(Tok::Colon)) s->Method = ExpectName("имя метода");
    s->Fn = ParseFuncBody(!s->Method.empty(), async);
    s->Annotations = std::move(notes);
    return s;
}

StmtPtr Parser::ParseLocal() {
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
void Parser::ParseLocalNames(Stmt& s) {
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

StmtPtr Parser::ParseConstVar() {
    auto s = Make(Stmt::K::Local);
    const bool isConst = Next().Text == "const";
    s->DeclKind = isConst ? Stmt::Decl::Const : Stmt::Decl::Var;
    ParseLocalNames(*s);
    if (isConst && s->Exprs.empty()) FailAt(s->P, "константа обязана получить значение: const " +
                                                   s->Names[0] + " = ...");
    if (s->DeclKind == Stmt::Decl::Infer && isConst) s->DeclKind = Stmt::Decl::Const;
    return s;
}

StmtPtr Parser::ParseImport() {
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

std::string Parser::Unquote(const std::string& raw) {
    if (raw.size() >= 2 && (raw[0] == '"' || raw[0] == '\'')) return raw.substr(1, raw.size() - 2);
    if (raw.size() >= 4 && raw[0] == '[') {
        size_t open = raw.find('[', 1);
        size_t level = open - 1;
        return raw.substr(open + 1, raw.size() - (open + 1) - (level + 2));
    }
    return raw;
}

StmtPtr Parser::ParseEnum() {
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

StmtPtr Parser::ParseEvent() {
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

StmtPtr Parser::ParseTypeAlias() {
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

std::vector<Annotation> Parser::ParseAnnotations() {
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

} // namespace sage::lang::parse
