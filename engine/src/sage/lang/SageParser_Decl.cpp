#include "sage/lang/SageParserDetail.h"

// Объявления: классы, интерфейсы, структуры, функции, типы (см. SageParserDetail.h).
namespace sage::lang::parse {

StmtPtr Parser::ParseClass(std::vector<Annotation> notes, bool exported) {
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

Member Parser::ParseMember(const ClassDecl& c) {
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

// Подпись без тела: (params): Ret — для методов интерфейса и типов функций.
FuncPtr Parser::ParseSignature(bool isMethod) {
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

void Parser::ParseParams(FuncBody& f) {
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

FuncPtr Parser::ParseFuncBody(bool isMethod, bool async) {
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

TypePtr Parser::ParseType() {
    TypePtr first = ParsePostfixType();
    if (!Check(Tok::Pipe)) return first;
    auto u = std::make_shared<TypeExpr>();
    u->Kind = TypeExpr::K::Union;
    u->P = first->P;
    u->Args.push_back(first);
    while (Accept(Tok::Pipe)) u->Args.push_back(ParsePostfixType());
    return u;
}

TypePtr Parser::ParsePostfixType() {
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

TypePtr Parser::ParsePrimaryType() {
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

} // namespace sage::lang::parse
