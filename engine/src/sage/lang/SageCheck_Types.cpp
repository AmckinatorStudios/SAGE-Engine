#include "sage/lang/SageCheckDetail.h"

// Типы: области видимости, разрешение, совместимость, аннотации (см. SageCheckDetail.h).
namespace sage::lang::check {

ProgramInfo Checker::Run(const BlockPtr& ast, std::map<std::string, Symbol>* exportsOut) {
    PushScope();
    for (const auto& [name, cls] : TheBuiltins().Classes) {
        Symbol s;
        s.Kind = Symbol::K::Class;
        s.Class = cls.get();
        s.T = ClassRefOf(cls.get());
        Top()[name] = s;
    }
    if (ast) {
        for (const StmtPtr& s : ast->Stmts) DeclarePass(s, true);
        for (const StmtPtr& s : ast->Stmts) ResolvePass(s);
        for (const auto& c : m_prog.Classes) ValidateClass(*c);
        if (!m_headerOnly) {
            for (const StmtPtr& s : ast->Stmts) CheckStmt(s, true);
        } else {
            for (const StmtPtr& s : ast->Stmts) DeclareExportedValue(s);
        }
    }
    if (exportsOut) *exportsOut = m_exports;
    PopScope();
    return std::move(m_prog);
}

Symbol* Checker::Lookup(const std::string& name) {
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

void Checker::TypeError(Pos p, const TypeRef& want, const TypeRef& got, const std::string& where, const std::string& gotText) {
    std::vector<std::string> lines = {"Ожидалось значение " + TypeName(want) + ".",
                                      "Получено значение " + TypeName(got) + "."};
    if (!where.empty()) lines.push_back(where);
    if (!gotText.empty()) lines.push_back("Получено: " + gotText);
    d.Error(p, "Ошибка типа", lines);
}

std::string Checker::ExprText(const ExprPtr& e) {
    if (!e) return {};
    switch (e->Kind) {
        case Expr::K::Number: case Expr::K::Name: return e->Text;
        case Expr::K::String: return e->Text;
        case Expr::K::True: return "true";
        case Expr::K::False: return "false";
        case Expr::K::Nil: return "nil";
        default: return {};
    }
}

TypeRef Checker::Resolve(const TypePtr& t) {
    if (!t) return Any();
    switch (t->Kind) {
        case TypeExpr::K::Optional: return OptionalOf(Resolve(t->Args[0]));
        case TypeExpr::K::Array: return Make(Type::K::Array, {Resolve(t->Args[0])});
        case TypeExpr::K::Union: {
            std::vector<TypeRef> args;
            bool hasNil = false;
            for (const TypePtr& a : t->Args) {
                TypeRef r = Resolve(a);
                if (r->Kind == Type::K::Nil) hasNil = true;
                else args.push_back(r);
            }
            TypeRef u = args.size() == 1 ? args[0] : Make(Type::K::Union, args);
            return hasNil ? OptionalOf(u) : u;
        }
        case TypeExpr::K::Tuple: {
            std::vector<TypeRef> args;
            for (const TypePtr& a : t->Args) args.push_back(Resolve(a));
            return Make(Type::K::Tuple, args);
        }
        case TypeExpr::K::Function: {
            auto sig = std::make_shared<FuncSig>();
            for (const TypePtr& a : t->Args) sig->Params.push_back({"", Resolve(a), false});
            sig->Ret = t->Ret ? Resolve(t->Ret) : nullptr;
            return FuncOf(sig);
        }
        case TypeExpr::K::Table: {
            auto r = std::make_shared<Type>();
            r->Kind = Type::K::Record;
            for (const auto& [n, ft] : t->Fields) r->Fields.emplace_back(n, Resolve(ft));
            return r;
        }
        case TypeExpr::K::Name: break;
    }
    const std::string& n = t->Name;
    auto noArgs = [&]() {
        if (!t->Args.empty()) d.Error(t->P, "Ошибка типа", {"тип " + n + " не принимает параметров"});
    };
    if (n == "number") { noArgs(); return NumT(); }
    if (n == "integer") { noArgs(); return IntT(); }
    if (n == "string") { noArgs(); return StrT(); }
    if (n == "boolean") { noArgs(); return BoolT(); }
    if (n == "nil") return NilT();
    if (n == "any") return Any();
    if (n == "void") return VoidT();
    if (n == "table") return TableT();
    if (n == "function") return FuncOf(std::make_shared<FuncSig>());
    if (n == "map") {
        if (t->Args.size() != 2) {
            d.Error(t->P, "Ошибка типа", {"map ждёт два параметра: map<K, V>"});
            return TableT();
        }
        return Make(Type::K::Map, {Resolve(t->Args[0]), Resolve(t->Args[1])});
    }
    // Module.Type
    const size_t dot = n.find('.');
    Symbol* s = nullptr;
    Symbol moduleMember;
    if (dot != std::string::npos) {
        Symbol* mod = Lookup(n.substr(0, dot));
        if (mod && mod->Kind == Symbol::K::Module) {
            auto it = mod->Exports.find(n.substr(dot + 1));
            if (it != mod->Exports.end()) {
                moduleMember = it->second;
                s = &moduleMember;
            }
        }
    } else {
        s = Lookup(n);
    }
    if (!s) {
        d.Error(t->P, "Ошибка типа", {"неизвестный тип " + n + "."});
        return Any();
    }
    switch (s->Kind) {
        case Symbol::K::TypeParam: noArgs(); return ParamT(n);
        case Symbol::K::Class: {
            std::vector<TypeRef> args;
            for (const TypePtr& a : t->Args) args.push_back(Resolve(a));
            if (!s->Class->TypeParams.empty() && !args.empty() &&
                args.size() != s->Class->TypeParams.size())
                d.Error(t->P, "Ошибка типа", {"у " + n + " " + std::to_string(s->Class->TypeParams.size()) +
                                              " параметр(ов) типа, а передано " + std::to_string(args.size())});
            if (s->Class->TypeParams.empty() && !args.empty())
                d.Error(t->P, "Ошибка типа", {"класс " + n + " не обобщённый"});
            return InstanceOf(s->Class, std::move(args));
        }
        case Symbol::K::Enum: noArgs(); return EnumValueOf(s->Enum);
        case Symbol::K::Alias: {
            auto src = m_aliasSrc.find(n);
            if (src != m_aliasSrc.end() && !src->second.second.empty()) {
                std::map<std::string, TypeRef> m;
                for (size_t k = 0; k < src->second.second.size(); ++k)
                    m[src->second.second[k]] = k < t->Args.size() ? Resolve(t->Args[k]) : Any();
                return Subst(s->Alias, m);
            }
            return s->Alias ? s->Alias : Any();
        }
        default:
            d.Error(t->P, "Ошибка типа", {n + " — не тип."});
            return Any();
    }
}

bool Checker::Assignable(const TypeRef& to, const TypeRef& from) {
    if (IsAnyish(to) || IsAnyish(from)) return true;
    if (from->Kind == Type::K::Union) {
        for (const TypeRef& m : from->Args)
            if (!Assignable(to, m)) return false;
        return true;
    }
    if (to->Kind == Type::K::Union) {
        for (const TypeRef& m : to->Args)
            if (Assignable(m, from)) return true;
        return false;
    }
    if (to->Kind == Type::K::Optional) {
        if (from->Kind == Type::K::Nil) return true;
        if (from->Kind == Type::K::Optional) return Assignable(to->Args[0], from->Args[0]);
        return Assignable(to->Args[0], from);
    }
    // T? в T — без проверки на nil: потока значений компилятор не ведёт, а
    // `if self.target ~= nil then self:follow(self.target)` обязан работать.
    if (from->Kind == Type::K::Optional) return Assignable(to, from->Args[0]);
    if (from->Kind == Type::K::Nil) return to->Kind == Type::K::Nil;
    switch (to->Kind) {
        case Type::K::Number: return IsNumeric(from);
        case Type::K::Integer: return from->Kind == Type::K::Integer;
        case Type::K::String: case Type::K::Boolean: case Type::K::Void: case Type::K::Nil:
            return from->Kind == to->Kind;
        case Type::K::Table:
            return from->Kind == Type::K::Table || from->Kind == Type::K::Array ||
                   from->Kind == Type::K::Map || from->Kind == Type::K::Record ||
                   from->Kind == Type::K::Instance;
        case Type::K::Function:
            return from->Kind == Type::K::Function || from->Kind == Type::K::ClassRef;
        case Type::K::Instance: {
            if (from->Kind == Type::K::Table) return true;
            if (from->Kind == Type::K::Record && from->Fields.empty() &&
                to->Class->Kind == ClassInfo::K::Struct)
                return true;
            if (from->Kind != Type::K::Instance) return false;
            if (!from->Class->DerivesFrom(to->Class)) return false;
            for (size_t k = 0; k < to->Args.size() && k < from->Args.size(); ++k)
                if (!Assignable(to->Args[k], from->Args[k])) return false;
            return true;
        }
        case Type::K::EnumValue:
            return from->Kind == Type::K::EnumValue && from->Enum == to->Enum;
        case Type::K::Array:
            if (from->Kind == Type::K::Array) return Assignable(to->Args[0], from->Args[0]);
            return from->Kind == Type::K::Table || (from->Kind == Type::K::Record && from->Fields.empty());
        case Type::K::Map:
            if (from->Kind == Type::K::Map)
                return Assignable(to->Args[0], from->Args[0]) && Assignable(to->Args[1], from->Args[1]);
            if (from->Kind == Type::K::Record) {
                if (!Assignable(to->Args[0], StrT())) return from->Fields.empty();
                for (const auto& f : from->Fields)
                    if (!Assignable(to->Args[1], f.second)) return false;
                return true;
            }
            return from->Kind == Type::K::Table;
        case Type::K::Record:
            if (from->Kind == Type::K::Record) {
                for (const auto& [n, ft] : to->Fields) {
                    const TypeRef* have = nullptr;
                    for (const auto& f : from->Fields)
                        if (f.first == n) have = &f.second;
                    if (!have) {
                        if (ft->Kind == Type::K::Optional || IsAnyish(ft)) continue;
                        return false;
                    }
                    if (!Assignable(ft, *have)) return false;
                }
                return true;
            }
            return from->Kind == Type::K::Table || from->Kind == Type::K::Instance;
        case Type::K::Tuple:
            if (from->Kind != Type::K::Tuple) return to->Args.size() == 1 && Assignable(to->Args[0], from);
            for (size_t k = 0; k < to->Args.size(); ++k)
                if (!Assignable(to->Args[k], k < from->Args.size() ? from->Args[k] : NilT())) return false;
            return true;
        case Type::K::ClassRef: return from->Kind == Type::K::ClassRef && from->Class == to->Class;
        case Type::K::EnumRef: return from->Kind == Type::K::EnumRef && from->Enum == to->Enum;
        case Type::K::EventRef: return from->Kind == Type::K::EventRef && from->Event == to->Event;
        default: return true;
    }
}

// Значение выражения против ожидаемого типа: таблица-литерал проверяется
// ПОЭЛЕМЕНТНО — сообщение указывает на тот самый элемент, а не на скобку.
void Checker::CheckAgainst(const TypeRef& want, const ExprPtr& e, const std::string& where) {
    if (!e) return;
    if (want && e->Kind == Expr::K::Table) {
        if (want->Kind == Type::K::Array) {
            for (const Expr::TField& f : e->Fields) {
                if (f.Kind == Expr::TField::K::Positional) {
                    CheckAgainst(want->Args[0], f.Value, where);
                } else {
                    Infer(f.Value);
                    if (f.Key) Infer(f.Key);
                }
            }
            return;
        }
        if (want->Kind == Type::K::Map) {
            for (const Expr::TField& f : e->Fields) {
                if (f.Kind == Expr::TField::K::Named && !Assignable(want->Args[0], StrT()))
                    d.Error(f.P, "Ошибка типа", {"ключ '" + f.Name + "' — строка, а ключи " +
                                                 TypeName(want) + " — " + TypeName(want->Args[0]), where});
                if (f.Key) {
                    TypeRef kt = Infer(f.Key);
                    if (!Assignable(want->Args[0], kt))
                        TypeError(f.Key->P, want->Args[0], kt, where);
                }
                CheckAgainst(want->Args[1], f.Value, where);
            }
            return;
        }
        if (want->Kind == Type::K::Record) {
            for (const Expr::TField& f : e->Fields) {
                if (f.Kind != Expr::TField::K::Named) {
                    Infer(f.Value);
                    continue;
                }
                TypeRef ft;
                for (const auto& [n, t] : want->Fields)
                    if (n == f.Name) ft = t;
                if (!ft) {
                    d.Error(f.P, "Ошибка типа", {"у типа " + TypeName(want) + " нет поля '" + f.Name + "'", where});
                    Infer(f.Value);
                    continue;
                }
                CheckAgainst(ft, f.Value, where + (where.empty() ? "" : ", ") + "поле " + f.Name);
            }
            for (const auto& [n, t] : want->Fields) {
                bool present = false;
                for (const Expr::TField& f : e->Fields) present = present || f.Name == n;
                if (!present && t->Kind != Type::K::Optional && !IsAnyish(t))
                    d.Error(e->P, "Ошибка типа", {"в таблице не хватает поля '" + n + "': " + TypeName(t), where});
            }
            return;
        }
        if (want->Kind == Type::K::Instance && want->Class->Kind == ClassInfo::K::Struct) {
            for (const Expr::TField& f : e->Fields) {
                if (f.Kind != Expr::TField::K::Named) continue;
                const FieldInfo* fi = want->Class->FindField(f.Name);
                if (!fi) d.Error(f.P, "Ошибка типа", {"у структуры " + want->Class->Name + " нет поля '" + f.Name + "'"});
                else CheckAgainst(fi->T, f.Value, "поле " + want->Class->Name + "." + f.Name);
            }
            return;
        }
    }
    TypeRef got = First(Infer(e));
    if (want && !Assignable(want, got)) TypeError(e->P, want, got, where, ExprText(e));
}

void Checker::ParseFieldMeta(const Annotation& a, FieldInfo& f) {
    for (const ExprPtr& p : a.Positional)
        d.Error(p->P, "Ошибка аннотации", {"@field принимает только именованные параметры: @field(editable = true)"});
    for (const auto& [key, value] : a.Named) {
        Literal lit;
        if (!EvalLiteral(value, lit)) {
            d.Error(value->P, "Ошибка аннотации", {"параметр " + key + " должен быть литералом (число, строка, true/false, таблица)"});
            continue;
        }
        auto need = [&](Literal::K k, const char* what) {
            if (lit.Kind == k) return true;
            d.Error(value->P, "Ошибка аннотации", {"@field(" + key + ") ждёт " + what + "."});
            return false;
        };
        if (key == "editable") { if (need(Literal::K::Bool, "true или false")) f.Meta.Editable = lit.B; }
        else if (key == "name") { if (need(Literal::K::String, "строку")) f.Meta.Label = lit.S; }
        else if (key == "tooltip") { if (need(Literal::K::String, "строку")) f.Meta.Tooltip = lit.S; }
        else if (key == "group") { if (need(Literal::K::String, "строку")) f.Meta.Group = lit.S; }
        else if (key == "replicated") { if (need(Literal::K::Bool, "true или false")) f.Meta.Replicated = lit.B; }
        else if (key == "serialize") { if (need(Literal::K::Bool, "true или false")) f.Meta.Serialize = lit.B; }
        else if (key == "range") {
            if (!need(Literal::K::Table, "таблицу { min = …, max = …, step = … }")) continue;
            f.Meta.HasRange = true;
            bool hasMin = false, hasMax = false;
            for (const auto& [rk, rv] : lit.Fields) {
                if (rv.Kind != Literal::K::Number) {
                    d.Error(rv.P, "Ошибка аннотации", {"range." + rk + " должно быть числом"});
                    continue;
                }
                if (rk == "min") { f.Meta.Min = rv.N; hasMin = true; }
                else if (rk == "max") { f.Meta.Max = rv.N; hasMax = true; }
                else if (rk == "step") f.Meta.Step = rv.N;
                else d.Error(rv.P, "Ошибка аннотации", {"у range нет параметра '" + rk + "' (есть min, max, step)"});
            }
            if (!hasMin || !hasMax) d.Error(value->P, "Ошибка аннотации", {"range обязан задать и min, и max"});
            else if (f.Meta.Min > f.Meta.Max)
                d.Error(value->P, "Ошибка аннотации", {"range: min (" + Num(f.Meta.Min) + ") больше max (" + Num(f.Meta.Max) + ")"});
            if (f.Meta.Step < 0 || (lit.Fields.count("step") && f.Meta.Step == 0))
                d.Error(value->P, "Ошибка аннотации", {"range.step должен быть больше нуля"});
        } else {
            d.Error(value->P, "Ошибка аннотации",
                    {"у @field нет параметра '" + key + "'",
                     "Есть: editable, name, tooltip, group, range, replicated, serialize."});
        }
    }
}

void Checker::ParseMethodMeta(const std::vector<Annotation>& notes, std::string& deprecated, std::string& side, const char* what) {
    for (const Annotation& a : notes) {
        if (a.Name == "method") {
            for (const ExprPtr& p : a.Positional)
                d.Error(p->P, "Ошибка аннотации", {"@method принимает только именованные параметры"});
            for (const auto& [key, value] : a.Named) {
                Literal lit;
                if (!EvalLiteral(value, lit)) {
                    d.Error(value->P, "Ошибка аннотации", {"параметр " + key + " должен быть литералом"});
                    continue;
                }
                if (key == "deprecated") {
                    if (lit.Kind == Literal::K::String) deprecated = lit.S;
                    else if (lit.Kind == Literal::K::Bool && lit.B) deprecated = "устарел";
                    else d.Error(value->P, "Ошибка аннотации", {"@method(deprecated) ждёт строку с пояснением"});
                } else if (key == "name" || key == "tooltip") {
                    if (lit.Kind != Literal::K::String)
                        d.Error(value->P, "Ошибка аннотации", {"@method(" + key + ") ждёт строку"});
                } else {
                    d.Error(value->P, "Ошибка аннотации", {"у @method нет параметра '" + key + "'", "Есть: deprecated, name, tooltip."});
                }
            }
        } else if (a.Name == "server" || a.Name == "client") {
            if (a.HasParens && (!a.Named.empty() || !a.Positional.empty()))
                d.Error(a.P, "Ошибка аннотации", {"@" + a.Name + " без параметров"});
            if (!side.empty() && side != a.Name)
                d.Error(a.P, "Ошибка аннотации", {"нельзя быть одновременно @server и @client"});
            side = a.Name;
        } else {
            Misplaced(a, what);
        }
    }
}

void Checker::Misplaced(const Annotation& a, const char* what) {
    static const std::set<std::string> known = {"serializable", "class", "field", "method",
                                                 "require", "server", "client"};
    if (!known.count(a.Name))
        d.Error(a.P, "Ошибка аннотации", {"неизвестная аннотация @" + a.Name,
                                          "Есть: @serializable, @class, @field, @method, @require, @server, @client."});
    else
        d.Error(a.P, "Ошибка аннотации", {"@" + a.Name + " неприменима к " + what});
}

} // namespace sage::lang::check
