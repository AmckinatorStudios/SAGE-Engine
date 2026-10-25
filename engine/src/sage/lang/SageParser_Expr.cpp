#include "sage/lang/SageParserDetail.h"

// Выражения (см. SageParserDetail.h).
namespace sage::lang::parse {

// Приоритеты Lua 5.4: {левый, правый}.
bool Parser::BinaryPriority(Tok k, int& left, int& right) {
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

bool Parser::StartsExpr(size_t off) const {
    switch (Peek(off).Kind) {
        case Tok::Name: case Tok::Number: case Tok::String: case Tok::Nil: case Tok::True:
        case Tok::False: case Tok::Ellipsis: case Tok::LParen: case Tok::LBrace:
        case Tok::Function: case Tok::Minus: case Tok::Not: case Tok::Hash: case Tok::Tilde:
            return true;
        default:
            return false;
    }
}

ExprPtr Parser::ParseSub(int limit) {
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

ExprPtr Parser::Leaf(Expr::K k) {
    auto e = std::make_shared<Expr>();
    e->Kind = k;
    e->P = Peek().P;
    e->Text = Next().Text;
    return e;
}

ExprPtr Parser::ParseSimple() {
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
ExprPtr Parser::TryArrow() {
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

ExprPtr Parser::ParsePrimary() {
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
bool Parser::TryGenericArgs(ExprPtr& e) {
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

ExprPtr Parser::ParseSuffixed() {
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

std::vector<ExprPtr> Parser::ParseCallArgs() {
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

ExprPtr Parser::ParseTable() {
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

StmtPtr Parser::ParseExprStat() {
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

} // namespace sage::lang::parse
