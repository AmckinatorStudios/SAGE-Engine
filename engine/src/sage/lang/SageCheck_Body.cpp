#include "sage/lang/SageCheckDetail.h"

// Проход 4: тела функций и операторы (см. SageCheckDetail.h).
namespace sage::lang::check {

void Checker::CheckStmt(const StmtPtr& s, bool topLevel) {
    switch (s->Kind) {
        case Stmt::K::Local: CheckLocal(*s, topLevel); break;
        case Stmt::K::Assign: CheckAssign(*s); break;
        case Stmt::K::Call: Infer(s->CallE); break;
        case Stmt::K::Do: CheckBlock(s->Body); break;
        case Stmt::K::While: Infer(s->Cond); CheckBlock(s->Body); break;
        case Stmt::K::Repeat:
            PushScope();
            for (const StmtPtr& x : s->Body->Stmts) CheckStmt(x, false);
            Infer(s->Cond);
            PopScope();
            break;
        case Stmt::K::If:
            for (const auto& [cond, body] : s->Clauses) {
                Infer(cond);
                CheckBlock(body);
            }
            CheckBlock(s->Else);
            break;
        case Stmt::K::NumFor: {
            bool allInt = true;
            for (const ExprPtr& e : s->Exprs) {
                TypeRef t = First(Infer(e));
                if (!IsAnyish(t) && !IsNumeric(t))
                    TypeError(e->P, NumT(), t, "граница цикла for");
                allInt = allInt && t->Kind == Type::K::Integer;
            }
            PushScope();
            Symbol v;
            v.T = allInt ? IntT() : NumT();
            v.P = s->P;
            Declare(s->Names[0], v);
            CheckBlock(s->Body);
            PopScope();
            break;
        }
        case Stmt::K::GenFor: CheckGenFor(*s); break;
        case Stmt::K::Function: CheckFunctionStat(*s, topLevel); break;
        case Stmt::K::LocalFunction: {
            Symbol sym;
            auto sig = SigOf(*s->Fn);
            sym.T = FuncOf(sig);
            sym.P = s->P;
            Declare(s->FuncName[0], sym);
            CheckFunction(*s->Fn, sig, nullptr, false, s->FuncName[0]);
            break;
        }
        case Stmt::K::Return: CheckReturn(*s); break;
        case Stmt::K::Class: {
            if (!topLevel) {
                DeclarePass(s, false);
                ResolvePass(s);
                if (auto it = m_classOf.find(s.get()); it != m_classOf.end()) ValidateClass(*it->second);
            }
            CheckClassBody(*s);
            break;
        }
        case Stmt::K::Enum: case Stmt::K::Event: case Stmt::K::TypeAlias:
            if (!topLevel) {
                DeclarePass(s, false);
                ResolvePass(s);
            }
            CheckEnumValues(*s);
            break;
        case Stmt::K::Import:
            if (!topLevel) d.Error(s->P, "Ошибка импорта", {"import — только на верхнем уровне файла"});
            break;
        default: break;
    }
}

void Checker::CheckEnumValues(const Stmt& s) {
    if (s.Kind != Stmt::K::Enum) return;
    for (const EnumDecl::Item& it : s.Enum->Items)
        if (it.Value) {
            TypeRef t = Infer(it.Value);
            if (!IsAnyish(t) && !IsNumeric(t) && t->Kind != Type::K::String)
                d.Error(it.Value->P, "Ошибка типа", {"значение перечисления — число или строка"});
        }
}

void Checker::CheckLocal(const Stmt& s, bool topLevel) {
    std::vector<TypeRef> values;
    for (size_t k = 0; k < s.Exprs.size(); ++k) {
        const TypeRef want = k < s.Types.size() && s.Types[k] ? Resolve(s.Types[k]) : nullptr;
        if (want && k < s.Names.size()) {
            CheckAgainst(want, s.Exprs[k], "Переменная: " + s.Names[k]);
            values.push_back(want);
        } else {
            TypeRef t = Infer(s.Exprs[k]);
            if (k + 1 == s.Exprs.size() && t && t->Kind == Type::K::Tuple) {
                for (const TypeRef& part : t->Args) values.push_back(part);
            } else {
                values.push_back(First(t));
            }
        }
    }
    for (size_t k = 0; k < s.Names.size(); ++k) {
        Symbol sym;
        sym.P = s.P;
        sym.Const = s.DeclKind == Stmt::Decl::Const || (k < s.Attribs.size() && s.Attribs[k] == "const");
        const TypeRef declared = k < s.Types.size() && s.Types[k] ? Resolve(s.Types[k]) : nullptr;
        const TypeRef value = k < values.size() ? values[k] : NilT();
        if (declared) {
            sym.T = declared;
            sym.Typed = true;
            if (k >= s.Exprs.size() && !s.Exprs.empty() == false && declared->Kind != Type::K::Optional &&
                !IsAnyish(declared) && declared->Kind != Type::K::Nil && s.Exprs.empty())
                d.Warning(s.P, "Предупреждение типа",
                          {"переменная " + s.Names[k] + ": " + TypeName(declared) + " объявлена без значения и пока равна nil"});
            if (k >= s.Exprs.size() && !s.Exprs.empty() && !Assignable(declared, NilT()))
                TypeError(s.P, declared, NilT(), "Переменная: " + s.Names[k]);
        } else if (s.DeclKind == Stmt::Decl::Infer || s.DeclKind == Stmt::Decl::Const ||
                   s.DeclKind == Stmt::Decl::Var) {
            if (value && value->Kind == Type::K::Nil && s.DeclKind != Stmt::Decl::Var)
                d.Error(s.P, "Ошибка типа", {"тип " + s.Names[k] + " нельзя вывести из nil — укажите его: " +
                                                 s.Names[k] + ": Entity? = nil"});
            sym.T = s.DeclKind == Stmt::Decl::Infer || s.DeclKind == Stmt::Decl::Var ? Widen(value) : value;
            sym.Typed = s.DeclKind == Stmt::Decl::Infer || s.DeclKind == Stmt::Decl::Const;
            if (s.DeclKind == Stmt::Decl::Var && (!value || value->Kind == Type::K::Nil)) sym.T = Any();
        } else {
            // Обычный local без типа — обычный Lua: присваивать можно что
            // угодно. Но ЭКЗЕМПЛЯР класса тип помнит — `local p = Player()`
            // и следом `p:takeDamage("x")` обязано проверяться, иначе
            // классы проверялись бы только в объявлениях с типом.
            sym.T = KeepsType(value) ? value : Any();
        }
        Declare(s.Names[k], sym);
        if (topLevel && s.Exported) m_exports[s.Names[k]] = sym;
    }
}

void Checker::CheckAssign(const Stmt& s) {
    std::vector<TypeRef> values;
    for (size_t k = 0; k < s.Exprs.size(); ++k) {
        TypeRef t = Infer(s.Exprs[k]);
        if (k + 1 == s.Exprs.size() && t && t->Kind == Type::K::Tuple)
            for (const TypeRef& part : t->Args) values.push_back(part);
        else
            values.push_back(First(t));
    }
    for (size_t k = 0; k < s.Targets.size(); ++k) {
        const ExprPtr& target = s.Targets[k];
        const TypeRef value = k < values.size() ? values[k] : NilT();
        const ExprPtr valueExpr = k < s.Exprs.size() ? s.Exprs[k] : nullptr;
        if (target->Kind == Expr::K::Name) {
            Symbol* sym = Lookup(target->Text);
            if (!sym) continue;   // глобальная переменная Lua
            if (sym->Kind == Symbol::K::Class || sym->Kind == Symbol::K::Enum || sym->Kind == Symbol::K::Event) {
                d.Error(target->P, "Ошибка присваивания", {target->Text + " — объявление типа, ему нельзя присвоить значение"});
                continue;
            }
            if (sym->Const) {
                d.Error(target->P, "Ошибка присваивания", {"константу " + target->Text + " нельзя переприсвоить"});
                continue;
            }
            if (sym->Typed && !Assignable(sym->T, value))
                TypeError(valueExpr ? valueExpr->P : target->P, sym->T, value, "Переменная: " + target->Text,
                          ExprText(valueExpr));
            else if (!sym->Typed && sym->Kind == Symbol::K::Var)
                sym->T = KeepsType(value) ? value : Any();   // переменная без типа следует за значением
            continue;
        }
        if (target->Kind == Expr::K::Field) {
            TypeRef obj = First(Infer(target->A));
            CheckFieldWrite(obj, target, value, valueExpr);
            continue;
        }
        if (target->Kind == Expr::K::Index) {
            TypeRef obj = First(Infer(target->A));
            TypeRef key = First(Infer(target->B));
            if (obj && obj->Kind == Type::K::Array) {
                if (!Assignable(IntT(), key) && !Assignable(NumT(), key))
                    TypeError(target->B->P, IntT(), key, "индекс массива");
                if (!Assignable(obj->Args[0], value))
                    TypeError(valueExpr ? valueExpr->P : target->P, obj->Args[0], value, "элемент массива");
            } else if (obj && obj->Kind == Type::K::Map) {
                if (!Assignable(obj->Args[0], key)) TypeError(target->B->P, obj->Args[0], key, "ключ map");
                if (!Assignable(obj->Args[1], value))
                    TypeError(valueExpr ? valueExpr->P : target->P, obj->Args[1], value, "значение map");
            }
        }
    }
}

void Checker::CheckFieldWrite(const TypeRef& obj, const ExprPtr& target, const TypeRef& value, const ExprPtr& valueExpr) {
    if (!obj) return;
    const std::string& name = target->Text;
    const ClassInfo* cls = nullptr;
    bool isStatic = false;
    if (obj->Kind == Type::K::Instance) cls = obj->Class;
    else if (obj->Kind == Type::K::ClassRef) {
        cls = obj->Class;
        isStatic = true;
    } else if (obj->Kind == Type::K::EnumRef) {
        d.Error(target->P, "Ошибка присваивания", {"перечисление " + obj->Enum->Name + " не изменяется"});
        return;
    } else {
        return;
    }
    const FieldInfo* f = cls->FindField(name);
    if (!f) {
        if (cls->FindMethod(name)) {
            d.Error(target->P, "Ошибка присваивания", {cls->Name + "." + name + " — метод, а не поле"});
            return;
        }
        if (!cls->IsOpen() && cls->Kind != ClassInfo::K::Builtin)
            d.Error(target->P, "Ошибка присваивания",
                    {"у " + std::string(isStatic ? "класса " : "типа ") + cls->Name + " нет поля '" + name + "'",
                     "Объявите его в классе: public " + name + ": " + TypeName(Widen(value)) + " = ..."});
        return;
    }
    CheckAccess(target->P, f->Access, f->Owner, "поле " + f->Owner->Name + "." + name);
    if (f->Static != isStatic && !(isStatic == false && f->Static))
        d.Error(target->P, "Ошибка присваивания", {"поле " + name + " — поле экземпляра, а не класса"});
    if (f->Const) {
        const bool inInit = !m_fn.empty() && m_fn.back().Init && m_fn.back().Cls &&
                            m_fn.back().Cls->DerivesFrom(f->Owner);
        if (!inInit)
            d.Error(target->P, "Ошибка присваивания", {"поле " + f->Owner->Name + "." + name + " только для чтения"});
    }
    TypeRef want = Subst(f->T, BindingsOf(obj));
    if (!Assignable(want, value))
        TypeError(valueExpr ? valueExpr->P : target->P, want, value, "Поле: " + f->Owner->Name + "." + name,
                  ExprText(valueExpr));
}

void Checker::CheckAccess(Pos p, const std::string& access, const ClassInfo* owner, const std::string& what) {
    if (access == "public") return;
    const ClassInfo* here = m_cls;
    if (access == "private" && here != owner)
        d.Error(p, "Ошибка доступа", {what + " закрыто (private) — доступно только внутри " + owner->Name});
    else if (access == "protected" && (!here || !here->DerivesFrom(owner)))
        d.Error(p, "Ошибка доступа", {what + " защищено (protected) — доступно в " + owner->Name + " и наследниках"});
}

void Checker::CheckGenFor(const Stmt& s) {
    std::vector<TypeRef> vars(s.Names.size(), Any());
    for (const ExprPtr& e : s.Exprs) Infer(e);
    // for i, v in ipairs(list) / for k, v in pairs(map): типы переменных
    // известны из коллекции.
    if (s.Exprs.size() == 1 && s.Exprs[0]->Kind == Expr::K::Call && s.Exprs[0]->A->Kind == Expr::K::Name &&
        s.Exprs[0]->Args.size() == 1) {
        const std::string fn = s.Exprs[0]->A->Text;
        TypeRef coll = First(Infer(s.Exprs[0]->Args[0]));
        if (coll && coll->Kind == Type::K::Array) {
            if (fn == "ipairs" || fn == "pairs") {
                vars[0] = IntT();
                if (vars.size() > 1) vars[1] = coll->Args[0];
            }
        } else if (coll && coll->Kind == Type::K::Map && fn == "pairs") {
            vars[0] = coll->Args[0];
            if (vars.size() > 1) vars[1] = coll->Args[1];
        }
    }
    PushScope();
    for (size_t k = 0; k < s.Names.size(); ++k) {
        Symbol v;
        v.T = k < s.Types.size() && s.Types[k] ? Resolve(s.Types[k]) : vars[k];
        v.P = s.P;
        Declare(s.Names[k], v);
    }
    CheckBlock(s.Body);
    PopScope();
}

void Checker::CheckFunctionStat(const Stmt& s, bool topLevel) {
    auto sig = SigOf(*s.Fn);
    std::string deprecated, side;
    ParseMethodMeta(s.Annotations, deprecated, side, "функции");
    if (s.FuncName.size() == 1 && s.Method.empty()) {
        Symbol* existing = Lookup(s.FuncName[0]);
        if (existing && existing->Const)
            d.Error(s.P, "Ошибка присваивания", {"константу " + s.FuncName[0] + " нельзя переприсвоить"});
        Symbol sym;
        sym.T = FuncOf(sig);
        sym.P = s.P;
        if (!existing) Declare(s.FuncName[0], sym, true);
        else if (existing->Kind == Symbol::K::Var) existing->T = sym.T;
        if (topLevel && s.Exported) m_exports[s.FuncName[0]] = sym;
    }
    CheckFunction(*s.Fn, sig, nullptr, false, s.FuncName.back());
}

void Checker::CheckFunction(const FuncBody& f, std::shared_ptr<const FuncSig> sig, const ClassInfo* cls, bool isStatic, const std::string& name) {
    FnCtx ctx;
    ctx.Sig = sig;
    ctx.Cls = cls ? cls : (m_fn.empty() ? nullptr : m_fn.back().Cls);
    ctx.Static = isStatic;
    ctx.Init = cls && name == "init";
    ctx.Name = name;
    m_fn.push_back(ctx);
    PushScope();
    for (const std::string& tp : f.TypeParams) {
        Symbol p;
        p.Kind = Symbol::K::TypeParam;
        Declare(tp, p);
    }
    if (cls) {
        for (const std::string& tp : cls->TypeParams) {
            Symbol p;
            p.Kind = Symbol::K::TypeParam;
            Declare(tp, p);
        }
        if (!isStatic) {
            Symbol self;
            std::vector<TypeRef> args;
            for (const std::string& tp : cls->TypeParams) args.push_back(ParamT(tp));
            self.T = InstanceOf(cls, args);
            self.Const = false;
            Declare("self", self);
        }
    } else if (f.IsMethod) {
        Symbol self;
        self.T = Any();
        Declare("self", self);
    }
    for (size_t k = 0; k < f.Params.size(); ++k) {
        const Param& p = f.Params[k];
        const TypeRef t = k < sig->Params.size() ? sig->Params[k].T : Any();
        if (p.Default) CheckAgainst(t, p.Default, "значение по умолчанию параметра " + p.Name);
        Symbol v;
        v.T = t;
        v.Typed = p.Type != nullptr;
        v.P = p.P;
        Declare(p.Name, v);
    }
    if (f.Vararg && !f.VarargName.empty()) {
        Symbol v;
        v.T = Make(Type::K::Array, {sig->VarargType ? sig->VarargType : Any()});
        v.Typed = f.VarargType != nullptr;
        Declare(f.VarargName, v);
    }
    if (f.ArrowExpr) {
        if (sig->Ret && sig->Ret->Kind != Type::K::Void) CheckAgainst(sig->Ret, f.ArrowExpr, "результат функции");
        else Infer(f.ArrowExpr);
    } else if (f.Body) {
        for (const StmtPtr& s : f.Body->Stmts) CheckStmt(s, false);
    }
    PopScope();
    m_fn.pop_back();
}

void Checker::CheckReturn(const Stmt& s) {
    if (m_fn.empty()) {
        for (const ExprPtr& e : s.Exprs) Infer(e);
        return;
    }
    const FnCtx& ctx = m_fn.back();
    const TypeRef& ret = ctx.Sig ? ctx.Sig->Ret : nullptr;
    if (!ret) {
        for (const ExprPtr& e : s.Exprs) Infer(e);
        return;
    }
    if (ret->Kind == Type::K::Void) {
        if (!s.Exprs.empty())
            d.Error(s.Exprs[0]->P, "Ошибка типа", {"функция " + ctx.Name + " объявлена : void, а возвращает значение"});
        for (const ExprPtr& e : s.Exprs) Infer(e);
        return;
    }
    if (s.Exprs.empty()) {
        if (!Assignable(ret, NilT()))
            d.Error(s.P, "Ошибка типа", {"функция " + ctx.Name + " обязана вернуть " + TypeName(ret)});
        return;
    }
    if (ret->Kind == Type::K::Tuple) {
        if (s.Exprs.size() != ret->Args.size() &&
            !(s.Exprs.back()->Kind == Expr::K::Call || s.Exprs.back()->Kind == Expr::K::MethodCall))
            d.Error(s.P, "Ошибка типа", {"функция " + ctx.Name + " возвращает " + std::to_string(ret->Args.size()) +
                                             " значения, а здесь " + std::to_string(s.Exprs.size())});
        for (size_t k = 0; k < s.Exprs.size(); ++k)
            CheckAgainst(k < ret->Args.size() ? ret->Args[k] : nullptr, s.Exprs[k],
                         "значение " + std::to_string(k + 1) + " результата " + ctx.Name);
        return;
    }
    CheckAgainst(ret, s.Exprs[0], "Результат функции " + ctx.Name);
    for (size_t k = 1; k < s.Exprs.size(); ++k) Infer(s.Exprs[k]);
}

void Checker::CheckClassBody(const Stmt& s) {
    auto it = m_classOf.find(&s);
    if (it == m_classOf.end()) return;
    ClassInfo& c = *it->second;
    const ClassInfo* savedCls = m_cls;
    m_cls = &c;
    PushScope();
    for (const std::string& tp : c.TypeParams) {
        Symbol p;
        p.Kind = Symbol::K::TypeParam;
        Declare(tp, p);
    }
    for (const Member& m : s.Class->Members) {
        if (m.Kind == Member::K::Field) {
            if (!m.Default) continue;
            const FieldInfo* f = c.OwnField(m.Name);
            FnCtx ctx;
            ctx.Cls = &c;
            ctx.Static = m.Static;
            ctx.Name = m.Name;
            m_fn.push_back(ctx);
            PushScope();
            if (!m.Static) {
                Symbol self;
                self.T = InstanceOf(&c);
                Declare("self", self);
            }
            if (f && m.Type) CheckAgainst(f->T, m.Default, "Поле: " + c.Name + "." + m.Name);
            else Infer(m.Default);
            PopScope();
            m_fn.pop_back();
        } else if (m.Fn && !m.Abstract) {
            const MethodInfo* mi = c.OwnMethod(m.Name);
            CheckFunction(*m.Fn, mi ? mi->Sig : SigOf(*m.Fn), &c, m.Static, m.Name);
        }
    }
    PopScope();
    m_cls = savedCls;
}

} // namespace sage::lang::check
