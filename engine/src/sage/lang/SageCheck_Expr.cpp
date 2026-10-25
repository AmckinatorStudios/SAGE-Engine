#include "sage/lang/SageCheckDetail.h"

// Выражения: вывод типов, вызовы, операции (см. SageCheckDetail.h).
namespace sage::lang::check {

TypeRef Checker::Infer(const ExprPtr& e) {
    if (!e) return Any();
    switch (e->Kind) {
        case Expr::K::Nil: return NilT();
        case Expr::K::True: case Expr::K::False: return BoolT();
        case Expr::K::Number: return IntegerLiteral(e->Text) ? IntT() : NumT();
        case Expr::K::String: return StrT();
        case Expr::K::Vararg: return Any();
        case Expr::K::Function: {
            auto sig = SigOf(*e->Fn);
            CheckFunction(*e->Fn, sig, nullptr, false, "<функция>");
            return FuncOf(sig);
        }
        case Expr::K::Table: return InferTable(e);
        case Expr::K::Name: {
            Symbol* s = Lookup(e->Text);
            if (!s) return Any();
            if (s->Kind == Symbol::K::Alias || s->Kind == Symbol::K::TypeParam) {
                d.Error(e->P, "Ошибка типа", {e->Text + " — тип, а не значение"});
                return Any();
            }
            return s->T ? s->T : Any();
        }
        case Expr::K::Super: {
            if (!m_cls || !m_cls->Parent) {
                d.Error(e->P, "Ошибка наследования", {"super — только в методе класса с родителем"});
                return Any();
            }
            return InstanceOf(m_cls->Parent);
        }
        case Expr::K::Paren: return First(Infer(e->A));
        case Expr::K::Await:
            if (e->Text == "outside")
                d.Error(e->P, "Ошибка async", {"await вне async-функции — объявите её: async function ..."});
            Infer(e->A);
            return Any();
        case Expr::K::Generic: {
            TypeRef base = Infer(e->A);
            std::vector<TypeRef> args;
            for (const TypePtr& t : e->TypeArgs) args.push_back(Resolve(t));
            if (base && base->Kind == Type::K::ClassRef) {
                if (base->Class->TypeParams.empty())
                    d.Error(e->P, "Ошибка типа", {"класс " + base->Class->Name + " не обобщённый"});
                else if (args.size() != base->Class->TypeParams.size())
                    d.Error(e->P, "Ошибка типа", {"у " + base->Class->Name + " " +
                                                     std::to_string(base->Class->TypeParams.size()) +
                                                     " параметр(ов) типа"});
                return ClassRefOf(base->Class, args);
            }
            return base;
        }
        case Expr::K::Field: return InferField(e);
        case Expr::K::Index: {
            TypeRef obj = First(Infer(e->A));
            TypeRef key = First(Infer(e->B));
            if (obj && obj->Kind == Type::K::Array) return obj->Args[0];
            if (obj && obj->Kind == Type::K::Map) return obj->Args[1];
            (void)key;
            return Any();
        }
        case Expr::K::Call: return InferCall(e);
        case Expr::K::MethodCall: return InferMethodCall(e);
        case Expr::K::Binary: return InferBinary(e);
        case Expr::K::Unary: {
            TypeRef a = First(Infer(e->A));
            switch (e->Op) {
                case Tok::Not: return BoolT();
                case Tok::Hash: return IntT();
                case Tok::Tilde: return IntT();
                case Tok::Minus:
                    if (a && (a->Kind == Type::K::String || a->Kind == Type::K::Boolean || a->Kind == Type::K::Nil))
                        d.Error(e->P, "Ошибка типа", {"унарный минус над значением типа " + TypeName(a)});
                    return IsNumeric(a) ? a : Any();
                default: return Any();
            }
        }
    }
    return Any();
}

TypeRef Checker::InferTable(const ExprPtr& e) {
    bool positional = false, named = false;
    std::vector<TypeRef> elems;
    auto rec = std::make_shared<Type>();
    rec->Kind = Type::K::Record;
    for (const Expr::TField& f : e->Fields) {
        if (f.Key) Infer(f.Key);
        TypeRef v = First(Infer(f.Value));
        if (f.Kind == Expr::TField::K::Positional) {
            positional = true;
            elems.push_back(v);
        } else if (f.Kind == Expr::TField::K::Named) {
            named = true;
            rec->Fields.emplace_back(f.Name, Widen(v));
        } else {
            named = positional = true;
        }
    }
    if (positional && named) return TableT();
    if (positional) {
        TypeRef elem = elems.empty() ? Any() : Widen(elems[0]);
        for (const TypeRef& t : elems)
            if (!Assignable(elem, t)) {
                elem = Any();
                break;
            }
        return Make(Type::K::Array, {elem});
    }
    return rec;
}

// Член по точке: поле, метод, статический член, значение перечисления.
TypeRef Checker::InferField(const ExprPtr& e) {
    // Модуль: import * as M.
    if (e->A->Kind == Expr::K::Name) {
        Symbol* s = Lookup(e->A->Text);
        if (s && s->Kind == Symbol::K::Module) {
            auto it = s->Exports.find(e->Text);
            if (it == s->Exports.end()) {
                if (!s->Exports.empty())
                    d.Error(e->P, "Ошибка импорта", {"модуль " + e->A->Text + " не экспортирует '" + e->Text + "'"});
                return Any();
            }
            return it->second.T ? it->second.T : Any();
        }
    }
    TypeRef obj = First(Infer(e->A));
    return MemberType(obj, e, false);
}

TypeRef Checker::MemberType(const TypeRef& obj, const ExprPtr& e, bool viaColon) {
    if (!obj) return Any();
    const std::string& name = e->Text;
    switch (obj->Kind) {
        case Type::K::EnumRef: {
            if (obj->Enum->Has(name)) return EnumValueOf(obj->Enum);
            if (name == "name" || name == "values" || name == "count" || name == "has") return Any();
            d.Error(e->P, "Ошибка перечисления",
                    {"в перечислении " + obj->Enum->Name + " нет значения '" + name + "'"});
            return Any();
        }
        case Type::K::ClassRef: {
            const ClassInfo* c = obj->Class;
            if (c->Name == "Result" && c->Kind == ClassInfo::K::Builtin && (name == "ok" || name == "err"))
                return Any();
            if (const FieldInfo* f = c->FindField(name)) {
                CheckAccess(e->P, f->Access, f->Owner, "поле " + f->Owner->Name + "." + name);
                if (!f->Static)
                    d.Error(e->P, "Ошибка доступа", {name + " — поле экземпляра; у класса " + c->Name + " его нет"});
                return f->T;
            }
            if (const MethodInfo* m = c->FindMethod(name)) {
                CheckAccess(e->P, m->Access, m->Owner, "метод " + m->Owner->Name + "." + name);
                return FuncOf(m->Sig);
            }
            if (name == "isInstance" || name == "name" || name == "is") return Any();
            if (!c->IsOpen() && c->Kind != ClassInfo::K::Builtin)
                d.Error(e->P, "Ошибка доступа", {"у класса " + c->Name + " нет статического члена '" + name + "'"});
            return Any();
        }
        case Type::K::Instance: {
            const ClassInfo* c = obj->Class;
            const auto bind = BindingsOf(obj);
            if (const FieldInfo* f = c->FindField(name)) {
                CheckAccess(e->P, f->Access, f->Owner, "поле " + f->Owner->Name + "." + name);
                return Subst(f->T, bind);
            }
            if (const MethodInfo* m = c->FindMethod(name)) {
                CheckAccess(e->P, m->Access, m->Owner, "метод " + m->Owner->Name + "." + name);
                if (!m->Deprecated.empty())
                    d.Warning(e->P, "Устаревший метод", {m->Owner->Name + "." + name + ": " + m->Deprecated});
                auto t = FuncOf(m->Sig);
                return Subst(t, bind);
            }
            // Общие члены каждого экземпляра класса SAGE.
            if (name == "copy" || name == "is" || name == "class") return Any();
            if (!c->IsOpen()) {
                d.Error(e->P, "Ошибка доступа",
                        {"у типа " + c->Name + " нет " + std::string(viaColon ? "метода" : "члена") + " '" + name + "'"});
            }
            return Any();
        }
        case Type::K::Record:
            for (const auto& [n, t] : obj->Fields)
                if (n == name) return t;
            return Any();
        case Type::K::Number: case Type::K::Integer: case Type::K::Boolean: case Type::K::Nil:
            d.Error(e->P, "Ошибка типа", {"у значения типа " + TypeName(obj) + " нет членов (обращение к '" + name + "')"});
            return Any();
        default: return Any();
    }
}

void Checker::CheckArgs(const FuncSig& sig, const std::vector<ExprPtr>& args, Pos p, const std::string& who, const std::map<std::string, TypeRef>& bind) {
    const bool expand = ExpandsLast(args);
    for (size_t k = 0; k < args.size(); ++k) {
        TypeRef want;
        std::string pname;
        if (k < sig.Params.size()) {
            want = Subst(sig.Params[k].T, bind);
            pname = sig.Params[k].Name;
        } else if (sig.Vararg) {
            want = Subst(sig.VarargType, bind);
            pname = "...";
        } else {
            if (!(expand && k + 1 == args.size()))
                d.Error(args[k]->P, "Ошибка вызова",
                        {who + " принимает " + std::to_string(sig.Params.size()) + " аргумент(ов), а передано " +
                         std::to_string(args.size())});
            for (size_t j = k; j < args.size(); ++j) Infer(args[j]);
            break;
        }
        CheckAgainst(want, args[k], "Аргумент " + pname + " у " + who);
    }
    if (!expand) {
        for (size_t k = args.size(); k < sig.Params.size(); ++k) {
            const ParamInfo& prm = sig.Params[k];
            if (prm.HasDefault || IsAnyish(prm.T) || prm.T->Kind == Type::K::Optional) continue;
            d.Error(p, "Ошибка вызова", {"у " + who + " не хватает аргумента " + prm.Name + ": " + TypeName(prm.T)});
        }
    }
}

// Events.emit(PlayerDied, player) / Events.on(PlayerDied, fn)
bool Checker::CheckEventsCall(const ExprPtr& e) {
    if (e->A->Kind != Expr::K::Field || e->A->A->Kind != Expr::K::Name || e->A->A->Text != "Events")
        return false;
    Symbol* ev = Lookup("Events");
    if (ev && ev->Kind != Symbol::K::Var) return false;
    if (e->Args.empty()) return false;
    TypeRef first = First(Infer(e->Args[0]));
    const std::string& op = e->A->Text;
    if (!first || first->Kind != Type::K::EventRef) {
        for (size_t k = 1; k < e->Args.size(); ++k) Infer(e->Args[k]);
        return true;
    }
    const EventInfo* info = first->Event;
    if (op == "emit") {
        const size_t got = e->Args.size() - 1;
        if (got != info->Params.size() && !(got > 0 && ExpandsLast(e->Args)))
            d.Error(e->P, "Ошибка события",
                    {"событие " + info->Name + " объявлено с " + std::to_string(info->Params.size()) +
                     " параметр(ами), а отправлено " + std::to_string(got)});
        for (size_t k = 1; k < e->Args.size(); ++k)
            CheckAgainst(k - 1 < info->Params.size() ? info->Params[k - 1].T : nullptr, e->Args[k],
                         "Аргумент " + (k - 1 < info->Params.size() ? info->Params[k - 1].Name : std::string("?")) +
                             " события " + info->Name);
    } else {
        for (size_t k = 1; k < e->Args.size(); ++k) {
            if (e->Args[k]->Kind == Expr::K::Function && e->Args[k]->Fn->Params.size() > info->Params.size())
                d.Error(e->Args[k]->P, "Ошибка события",
                        {"обработчик ждёт " + std::to_string(e->Args[k]->Fn->Params.size()) +
                         " параметр(ов), а событие " + info->Name + " даёт " +
                         std::to_string(info->Params.size())});
            Infer(e->Args[k]);
        }
    }
    return true;
}

TypeRef Checker::InferCall(const ExprPtr& e) {
    if (CheckEventsCall(e)) return Any();
    // Result.ok(v) / Result.err(e)
    if (e->A->Kind == Expr::K::Field && e->A->A->Kind == Expr::K::Name && e->A->A->Text == "Result") {
        Symbol* r = Lookup("Result");
        if (r && r->Kind == Symbol::K::Class && r->Class->Kind == ClassInfo::K::Builtin &&
            (e->A->Text == "ok" || e->A->Text == "err")) {
            TypeRef v = e->Args.empty() ? NilT() : Widen(First(Infer(e->Args[0])));
            for (size_t k = 1; k < e->Args.size(); ++k) Infer(e->Args[k]);
            return e->A->Text == "ok" ? InstanceOf(r->Class, {v, Any()}) : InstanceOf(r->Class, {Any(), v});
        }
    }
    TypeRef callee = First(Infer(e->A));
    if (!callee) callee = Any();
    if (callee->Kind == Type::K::ClassRef) return Construct(callee, e);
    if (callee->Kind == Type::K::Function && callee->Sig) {
        const std::string who = e->A->Kind == Expr::K::Name ? e->A->Text + "()"
                              : e->A->Kind == Expr::K::Field ? e->A->Text + "()" : "функции";
        CheckArgs(*callee->Sig, e->Args, e->P, who, {});
        return RetOf(*callee->Sig, {});
    }
    if (callee->Kind == Type::K::EnumRef || callee->Kind == Type::K::EventRef || IsNumeric(callee) ||
        callee->Kind == Type::K::String || callee->Kind == Type::K::Boolean || callee->Kind == Type::K::Nil) {
        d.Error(e->P, "Ошибка вызова", {"значение типа " + TypeName(callee) + " нельзя вызвать"});
    }
    for (const ExprPtr& a : e->Args) Infer(a);
    return Any();
}

TypeRef Checker::Construct(const TypeRef& cls, const ExprPtr& e) {
    const ClassInfo* c = cls->Class;
    if (c->Kind == ClassInfo::K::Interface) {
        d.Error(e->P, "Ошибка вызова", {"интерфейс " + c->Name + " нельзя создать — создают класс, который его реализует"});
        return Any();
    }
    std::vector<TypeRef> args = cls->Args;
    if (args.empty())
        for (size_t k = 0; k < c->TypeParams.size(); ++k) args.push_back(Any());
    TypeRef inst = InstanceOf(c, args);
    if (c->Kind == ClassInfo::K::Struct) {
        // Vector2(10, 20) — по порядку полей; Vector2{ x = 1 } — таблицей.
        if (e->Args.size() == 1 && e->Args[0]->Kind == Expr::K::Table) {
            CheckAgainst(inst, e->Args[0], "структура " + c->Name);
            return inst;
        }
        std::vector<const FieldInfo*> fields;
        for (const FieldInfo& f : c->Fields)
            if (!f.Static) fields.push_back(&f);
        if (e->Args.size() > fields.size())
            d.Error(e->P, "Ошибка вызова", {"у структуры " + c->Name + " " + std::to_string(fields.size()) +
                                             " поля, а передано " + std::to_string(e->Args.size()) + " значений"});
        for (size_t k = 0; k < e->Args.size(); ++k)
            CheckAgainst(k < fields.size() ? fields[k]->T : nullptr, e->Args[k],
                         k < fields.size() ? "Поле: " + c->Name + "." + fields[k]->Name : std::string());
        return inst;
    }
    if (c->IsComponent() && c->Kind != ClassInfo::K::Builtin) {
        // Компонент создаёт движок, навешивая его на объект.
        d.Warning(e->P, "Предупреждение", {"компонент " + c->Name + " создаётся вызовом — он не будет привязан к объекту",
                                           "Используйте entity:addComponent(" + c->Name + ")."});
    }
    if (const MethodInfo* init = c->FindMethod("init")) {
        CheckAccess(e->P, init->Access, init->Owner, "конструктор " + c->Name);
        CheckArgs(*init->Sig, e->Args, e->P, c->Name + "()", BindingsOf(inst));
    } else {
        if (!e->Args.empty() && c->Kind != ClassInfo::K::Builtin && !c->IsOpen())
            d.Error(e->P, "Ошибка вызова", {"у класса " + c->Name + " нет конструктора init — аргументы некуда передать"});
        for (const ExprPtr& a : e->Args) Infer(a);
    }
    return inst;
}

TypeRef Checker::InferMethodCall(const ExprPtr& e) {
    TypeRef obj;
    bool viaSuper = e->A->Kind == Expr::K::Super;
    obj = First(Infer(e->A));
    if (!obj || IsAnyish(obj)) {
        for (const ExprPtr& a : e->Args) Infer(a);
        return Any();
    }
    if (obj->Kind == Type::K::Instance) {
        const ClassInfo* c = obj->Class;
        const MethodInfo* m = c->FindMethod(e->Text);
        if (!m) {
            const FieldInfo* f = c->FindField(e->Text);
            if (!f && !c->IsOpen() && e->Text != "copy" && e->Text != "is")
                d.Error(e->P, "Ошибка вызова", {"у типа " + c->Name + " нет метода '" + e->Text + "'"});
            for (const ExprPtr& a : e->Args) Infer(a);
            return Any();
        }
        if (!viaSuper) CheckAccess(e->P, m->Access, m->Owner, "метод " + m->Owner->Name + "." + e->Text);
        if (m->Static)
            d.Error(e->P, "Ошибка вызова", {m->Owner->Name + "." + e->Text + " — статический: " +
                                             m->Owner->Name + "." + e->Text + "(...)"});
        if (!m->Deprecated.empty())
            d.Warning(e->P, "Устаревший метод", {m->Owner->Name + "." + e->Text + ": " + m->Deprecated});
        const auto bind = BindingsOf(obj);
        CheckArgs(*m->Sig, e->Args, e->P, m->Owner->Name + ":" + e->Text + "()", bind);
        return RetOf(*m->Sig, bind);
    }
    if (obj->Kind == Type::K::Number || obj->Kind == Type::K::Integer || obj->Kind == Type::K::Boolean ||
        obj->Kind == Type::K::Nil) {
        d.Error(e->P, "Ошибка вызова", {"у значения типа " + TypeName(obj) + " нет методов"});
    }
    for (const ExprPtr& a : e->Args) Infer(a);
    return Any();
}

TypeRef Checker::InferBinary(const ExprPtr& e) {
    TypeRef a = First(Infer(e->A));
    TypeRef b = First(Infer(e->B));
    auto bad = [](const TypeRef& t) {
        return t && (t->Kind == Type::K::String || t->Kind == Type::K::Boolean || t->Kind == Type::K::Nil ||
                     t->Kind == Type::K::EnumValue);
    };
    switch (e->Op) {
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent:
        case Tok::Caret: case Tok::DSlash: {
            if (bad(a) || bad(b)) {
                const TypeRef& w = bad(a) ? a : b;
                d.Error(e->P, "Ошибка типа", {"арифметика над значением типа " + TypeName(w),
                                               w->Kind == Type::K::String ? "Строки соединяются оператором '..'." : ""});
                return Any();
            }
            if (a && b && a->Kind == Type::K::Integer && b->Kind == Type::K::Integer &&
                e->Op != Tok::Slash && e->Op != Tok::Caret)
                return IntT();
            if (IsNumeric(a) && IsNumeric(b)) return NumT();
            if ((a && a->Kind == Type::K::Instance) || (b && b->Kind == Type::K::Instance)) {
                // Vector3 + Vector3, Vector3 * number — операторы объекта.
                if (a && a->Kind == Type::K::Instance) return a;
                return b;
            }
            return Any();
        }
        case Tok::Concat:
            if ((a && (a->Kind == Type::K::Boolean || a->Kind == Type::K::Nil)) ||
                (b && (b->Kind == Type::K::Boolean || b->Kind == Type::K::Nil)))
                d.Error(e->P, "Ошибка типа", {"'..' соединяет строки и числа, а не " +
                                               TypeName((a->Kind == Type::K::Boolean || a->Kind == Type::K::Nil) ? a : b),
                                               "Используйте tostring(...)."});
            return StrT();
        case Tok::Eq: case Tok::Ne: case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge:
            return BoolT();
        case Tok::And: return b;
        case Tok::Or:
            if (a && a->Kind == Type::K::Optional) return a->Args[0];
            if (a && a->Kind == Type::K::Nil) return b;
            if (a && b && a->Kind == b->Kind && a->Kind != Type::K::Instance) return a;
            return Any();
        case Tok::Amp: case Tok::Pipe: case Tok::Tilde: case Tok::Shl: case Tok::Shr:
            return IntT();
        default: return Any();
    }
}

} // namespace sage::lang::check
