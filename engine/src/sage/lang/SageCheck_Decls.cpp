#include "sage/lang/SageCheckDetail.h"

// Проходы 1–3 и импорт: объявления, заголовки, правила классов (см. SageCheckDetail.h).
namespace sage::lang::check {

void Checker::DeclarePass(const StmtPtr& s, bool topLevel) {
    switch (s->Kind) {
        case Stmt::K::Import:
            if (topLevel) ResolveImport(*s);
            break;
        case Stmt::K::Class: DeclareClass(s); break;
        case Stmt::K::Enum: DeclareEnum(*s); break;
        case Stmt::K::Event: DeclareEvent(*s); break;
        case Stmt::K::TypeAlias: {
            Symbol sym;
            sym.Kind = Symbol::K::Alias;
            sym.P = s->P;
            Declare(s->Text, sym);
            m_aliasSrc[s->Text] = {s->Alias, s->TypeParams};
            break;
        }
        default: break;
    }
}

void Checker::DeclareClass(const StmtPtr& s) {
    auto c = std::make_shared<ClassInfo>();
    const ClassDecl& decl = *s->Class;
    c->Kind = decl.Kind == ClassDecl::K::Interface ? ClassInfo::K::Interface
            : decl.Kind == ClassDecl::K::Struct ? ClassInfo::K::Struct : ClassInfo::K::Class;
    c->Name = decl.Name;
    c->Module = m_path;
    c->TypeParams = decl.TypeParams;
    c->Final = decl.Final;
    c->Exported = s->Exported;
    c->Decl = s->Class;
    if (Symbol* prev = Lookup(decl.Name); prev && prev->Kind == Symbol::K::Class &&
                                           prev->Class->Module == m_path)
        d.Error(decl.P, "Ошибка объявления", {"тип " + decl.Name + " уже объявлен в этом файле"});
    Symbol sym;
    sym.Kind = Symbol::K::Class;
    sym.Class = c.get();
    sym.T = ClassRefOf(c.get());
    sym.P = decl.P;
    Declare(decl.Name, sym);
    if (s->Exported) m_exports[decl.Name] = sym;
    m_prog.Classes.push_back(c);
    m_prog.ByDecl[s->Class.get()] = c.get();
    m_classOf[s.get()] = c;
}

void Checker::DeclareEnum(const Stmt& s) {
    auto e = std::make_shared<EnumInfo>();
    e->Name = s.Enum->Name;
    e->Exported = s.Exported;
    std::set<std::string> seen;
    for (const EnumDecl::Item& it : s.Enum->Items) {
        if (!seen.insert(it.Name).second)
            d.Error(it.P, "Ошибка объявления", {"значение " + it.Name + " в перечислении " + e->Name + " повторяется"});
        e->Items.push_back(it.Name);
    }
    Symbol sym;
    sym.Kind = Symbol::K::Enum;
    sym.Enum = e.get();
    sym.P = s.P;
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::EnumRef;
    t->Enum = e.get();
    sym.T = t;
    Declare(e->Name, sym);
    if (s.Exported) m_exports[e->Name] = sym;
    m_prog.Enums.push_back(e);
}

void Checker::DeclareEvent(const Stmt& s) {
    auto e = std::make_shared<EventInfo>();
    e->Name = s.Event->Name;
    e->Exported = s.Exported;
    Symbol sym;
    sym.Kind = Symbol::K::Event;
    sym.Event = e.get();
    sym.P = s.P;
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::EventRef;
    t->Event = e.get();
    sym.T = t;
    sym.Const = true;
    Declare(e->Name, sym);
    if (s.Exported) m_exports[e->Name] = sym;
    m_prog.Events.push_back(e);
}

void Checker::ResolvePass(const StmtPtr& s) {
    switch (s->Kind) {
        case Stmt::K::TypeAlias: {
            PushScope();
            for (const std::string& tp : s->TypeParams) {
                Symbol p;
                p.Kind = Symbol::K::TypeParam;
                Declare(tp, p);
            }
            TypeRef t = Resolve(s->Alias);
            PopScope();
            if (Symbol* sym = Lookup(s->Text)) {
                sym->Alias = t;
                if (s->Exported) {
                    m_exports[s->Text] = *sym;
                }
            }
            break;
        }
        case Stmt::K::Event: {
            Symbol* sym = Lookup(s->Event->Name);
            if (!sym || !sym->Event) break;
            auto* e = const_cast<EventInfo*>(sym->Event);
            for (const Param& p : s->Event->Params) e->Params.push_back({p.Name, Resolve(p.Type), false});
            if (s->Exported) m_exports[e->Name] = *sym;
            break;
        }
        case Stmt::K::Class: ResolveClass(s); break;
        case Stmt::K::Function:
            // Глобальная функция видна до своего объявления — её зовут из
            // методов, объявленных выше по файлу.
            if (s->FuncName.size() == 1 && s->Method.empty()) {
                Symbol sym;
                sym.T = FuncOf(SigOf(*s->Fn));
                sym.P = s->P;
                if (!Lookup(s->FuncName[0])) Declare(s->FuncName[0], sym);
            }
            break;
        default: break;
    }
}

std::shared_ptr<FuncSig> Checker::SigOf(const FuncBody& f) {
    auto sig = std::make_shared<FuncSig>();
    PushScope();
    for (const std::string& tp : f.TypeParams) {
        Symbol p;
        p.Kind = Symbol::K::TypeParam;
        Declare(tp, p);
    }
    for (const Param& p : f.Params) {
        ParamInfo pi;
        pi.Name = p.Name;
        pi.T = p.Type ? Resolve(p.Type) : Any();
        pi.HasDefault = p.Default != nullptr;
        sig->Params.push_back(pi);
    }
    sig->Vararg = f.Vararg;
    if (f.VarargType) {
        TypeRef vt = Resolve(f.VarargType);
        // ...values: number[] — тип каждого значения number.
        sig->VarargType = vt->Kind == Type::K::Array ? vt->Args[0] : vt;
    }
    sig->Ret = f.HasRet ? Resolve(f.Ret) : nullptr;
    sig->Async = f.Async;
    sig->TypeParams = f.TypeParams;
    PopScope();
    return sig;
}

void Checker::ResolveClass(const StmtPtr& s) {
    auto it = m_classOf.find(s.get());
    if (it == m_classOf.end()) return;
    ClassInfo& c = *it->second;
    const ClassDecl& decl = *s->Class;
    PushScope();
    for (const std::string& tp : c.TypeParams) {
        Symbol p;
        p.Kind = Symbol::K::TypeParam;
        Declare(tp, p);
    }
    if (decl.Extends) {
        TypeRef parent = Resolve(decl.Extends);
        if (parent->Kind == Type::K::Instance) {
            if (parent->Class->Kind == ClassInfo::K::Interface)
                d.Error(decl.Extends->P, "Ошибка наследования",
                        {parent->Class->Name + " — интерфейс: его не наследуют, а реализуют (implements)"});
            else if (parent->Class->Kind == ClassInfo::K::Struct || c.Kind == ClassInfo::K::Struct)
                d.Error(decl.Extends->P, "Ошибка наследования", {"структуры не наследуются"});
            else if (parent->Class->Final)
                d.Error(decl.Extends->P, "Ошибка наследования", {"класс " + parent->Class->Name + " объявлен final"});
            else
                c.Parent = parent->Class;
        } else if (parent->Kind != Type::K::Any) {
            d.Error(decl.Extends->P, "Ошибка наследования", {"наследовать можно только класс"});
        }
    }
    for (const TypePtr& itype : decl.Implements) {
        TypeRef i = Resolve(itype);
        if (i->Kind == Type::K::Instance && i->Class->Kind == ClassInfo::K::Interface)
            c.Interfaces.push_back(i->Class);
        else if (i->Kind != Type::K::Any)
            d.Error(itype->P, "Ошибка наследования", {TypeName(i) + " — не интерфейс"});
    }
    // Аннотации класса.
    for (const Annotation& a : decl.Annotations) {
        if (a.Name == "serializable") {
            if (!a.Named.empty() || !a.Positional.empty())
                d.Error(a.P, "Ошибка аннотации", {"@serializable без параметров"});
            c.Serializable = true;
        } else if (a.Name == "class") {
            for (const auto& [key, value] : a.Named) {
                Literal lit;
                if (!EvalLiteral(value, lit) || lit.Kind != Literal::K::String) {
                    d.Error(value->P, "Ошибка аннотации", {"@class(" + key + ") ждёт строку"});
                    continue;
                }
                if (key == "name") c.DisplayName = lit.S;
                else if (key == "description") c.Description = lit.S;
                else if (key == "category") c.Category = lit.S;
                else d.Error(value->P, "Ошибка аннотации", {"у @class нет параметра '" + key + "'", "Есть: name, description, category."});
            }
        } else if (a.Name == "require") {
            if (a.Positional.empty()) d.Error(a.P, "Ошибка аннотации", {"@require(Компонент) — какой компонент нужен?"});
            for (const ExprPtr& p : a.Positional) {
                if (p->Kind != Expr::K::Name) {
                    d.Error(p->P, "Ошибка аннотации", {"@require ждёт имя компонента: @require(RigidBody)"});
                    continue;
                }
                Symbol* sym = Lookup(p->Text);
                if (!sym || sym->Kind != Symbol::K::Class ||
                    (sym->Class->EngineName.empty() && !sym->Class->IsComponent()))
                    d.Error(p->P, "Ошибка аннотации", {p->Text + " — не компонент"});
                else
                    c.Requires.push_back(sym->Class->EngineName.empty() ? sym->Class->Name : sym->Class->EngineName);
            }
        } else if (a.Name == "server" || a.Name == "client") {
            c.Side = a.Name;
        } else {
            Misplaced(a, "классу");
        }
    }
    // Члены.
    std::set<std::string> names;
    for (const Member& m : decl.Members) {
        if (!names.insert(m.Name).second)
            d.Error(m.P, "Ошибка объявления", {"член " + c.Name + "." + m.Name + " объявлен дважды"});
        if (m.Kind == Member::K::Field) {
            FieldInfo f;
            f.Name = m.Name;
            f.Access = m.Access;
            f.Static = m.Static;
            f.Const = m.Const;
            f.Owner = &c;
            f.P = m.P;
            f.Default = m.Default;
            f.DefaultIsLiteral = !m.Default || IsLiteralExpr(m.Default);
            if (m.Type) f.T = Resolve(m.Type);
            else f.T = Widen(LiteralType(m.Default));
            if (m.Override) d.Error(m.P, "Ошибка объявления", {"override применим к методу, а не к полю"});
            for (const Annotation& a : m.Annotations) {
                if (a.Name == "field") ParseFieldMeta(a, f);
                else Misplaced(a, "полю");
            }
            c.Fields.push_back(f);
        } else {
            MethodInfo mi;
            mi.Name = m.Name;
            mi.Access = m.Access;
            mi.Static = m.Static;
            mi.Override = m.Override;
            mi.Final = m.Final;
            mi.Abstract = m.Abstract;
            mi.Owner = &c;
            mi.P = m.P;
            mi.Sig = SigOf(*m.Fn);
            ParseMethodMeta(m.Annotations, mi.Deprecated, mi.Side, "методу");
            if (m.Name == "init" && mi.Sig->Ret && mi.Sig->Ret->Kind != Type::K::Void)
                d.Error(m.P, "Ошибка объявления", {"конструктор init ничего не возвращает (: void)"});
            c.Methods.push_back(mi);
        }
    }
    PopScope();
}

TypeRef Checker::LiteralType(const ExprPtr& e) {
    if (!e) return Any();
    switch (e->Kind) {
        case Expr::K::Number: return IntegerLiteral(e->Text) ? IntT() : NumT();
        case Expr::K::String: return StrT();
        case Expr::K::True: case Expr::K::False: return BoolT();
        case Expr::K::Unary:
            if (e->Op == Tok::Minus && e->A && e->A->Kind == Expr::K::Number) return LiteralType(e->A);
            return Any();
        default: return Any();
    }
}

void Checker::ValidateClass(ClassInfo& c) {
    // Цикл наследования.
    {
        std::set<const ClassInfo*> seen;
        for (const ClassInfo* p = &c; p; p = p->Parent) {
            if (!seen.insert(p).second) {
                d.Error(c.Decl ? c.Decl->P : Pos{}, "Ошибка наследования", {"класс " + c.Name + " наследует сам себя"});
                c.Parent = nullptr;
                break;
            }
        }
    }
    // Поле, уже объявленное родителем.
    for (const FieldInfo& f : c.Fields)
        if (c.Parent)
            if (const FieldInfo* pf = c.Parent->FindField(f.Name))
                d.Error(f.P, "Ошибка объявления", {"поле " + f.Name + " уже объявлено в " + pf->Owner->Name});
    // Методы: override / final / совместимость подписи.
    for (const MethodInfo& m : c.Methods) {
        if (c.Kind == ClassInfo::K::Interface) continue;
        const MethodInfo* base = c.Parent ? c.Parent->FindMethod(m.Name) : nullptr;
        if (m.Name == "init") {
            if (m.Override) d.Error(m.P, "Ошибка переопределения", {"конструктор init не переопределяется"});
            continue;
        }
        if (!base) {
            if (m.Override)
                d.Error(m.P, "Ошибка переопределения",
                        {"override у метода " + c.Name + "." + m.Name + ", но у родителя такого метода нет"});
            continue;
        }
        if (base->Final)
            d.Error(m.P, "Ошибка переопределения", {base->Owner->Name + "." + m.Name + " объявлен final"});
        if (!m.Override && !base->Virtual)
            d.Error(m.P, "Ошибка переопределения",
                    {"метод " + c.Name + "." + m.Name + " переопределяет " + base->Owner->Name + "." + m.Name +
                         " — напишите override"});
        if (m.Static != base->Static)
            d.Error(m.P, "Ошибка переопределения", {"static у " + m.Name + " не совпадает с родителем"});
        CheckSignatureCompat(m, *base, "переопределение");
    }
    // Интерфейсы.
    if (c.Kind == ClassInfo::K::Class) {
        std::set<const ClassInfo*> done;
        std::vector<const ClassInfo*> todo(c.Interfaces.begin(), c.Interfaces.end());
        while (!todo.empty()) {
            const ClassInfo* iface = todo.back();
            todo.pop_back();
            if (!done.insert(iface).second) continue;
            for (const ClassInfo* parent : iface->Interfaces) todo.push_back(parent);
            for (const MethodInfo& im : iface->Methods) {
                const MethodInfo* impl = nullptr;
                for (const ClassInfo* k = &c; k && !impl; k = k->Parent)
                    if (const MethodInfo* m = k->OwnMethod(im.Name); m && !m->Abstract) impl = m;
                if (!impl) {
                    d.Error(c.Decl ? c.Decl->P : Pos{}, "Ошибка интерфейса",
                            {"класс " + c.Name + " не реализует " + iface->Name + "." + im.Name});
                    continue;
                }
                CheckSignatureCompat(*impl, im, "реализация интерфейса " + iface->Name);
            }
        }
    }
    // @field: что можно показать в инспекторе, диапазон и значение.
    for (const FieldInfo& f : c.Fields) {
        if (f.Meta.HasRange) {
            if (!IsNumeric(f.T) && !IsAnyish(f.T))
                d.Error(f.P, "Ошибка аннотации", {"range применим к числовому полю, а " + f.Name + ": " + TypeName(f.T)});
            Literal lit;
            if (f.Default && EvalLiteral(f.Default, lit) && lit.Kind == Literal::K::Number &&
                (lit.N < f.Meta.Min || lit.N > f.Meta.Max))
                d.Error(f.Default->P, "Ошибка аннотации",
                        {"значение по умолчанию " + Num(lit.N) + " вне диапазона [" + Num(f.Meta.Min) + ", " +
                             Num(f.Meta.Max) + "]",
                         "Поле: " + c.Name + "." + f.Name});
        }
        if (f.Meta.Editable) {
            if (f.Static) d.Error(f.P, "Ошибка аннотации", {"статическое поле не показывается в инспекторе"});
            if (!Inspectable(f.T))
                d.Error(f.P, "Ошибка аннотации",
                        {"тип " + TypeName(f.T) + " не показывается в инспекторе",
                         "Можно: number, integer, boolean, string, Vector2, Vector3, Color, перечисление, "
                         "Entity или компонент."});
        }
        if (f.Meta.Replicated && f.Static)
            d.Error(f.P, "Ошибка аннотации", {"replicated применим к полю экземпляра"});
    }
}

bool Checker::Inspectable(const TypeRef& t) {
    if (!t) return true;
    switch (t->Kind) {
        case Type::K::Any: case Type::K::Number: case Type::K::Integer: case Type::K::Boolean:
        case Type::K::String: case Type::K::EnumValue:
            return true;
        case Type::K::Optional: return Inspectable(t->Args[0]);
        case Type::K::Instance: {
            const std::string& n = t->Class->Name;
            return n == "Vector2" || n == "Vector3" || n == "Color" || n == "Entity" ||
                   !t->Class->EngineName.empty() || t->Class->IsComponent() || n == "Texture" ||
                   n == "Prefab" || n == "AudioClip" || n == "Material";
        }
        default: return false;
    }
}

void Checker::CheckSignatureCompat(const MethodInfo& m, const MethodInfo& base, const std::string& what) {
    const FuncSig& a = *m.Sig;
    const FuncSig& b = *base.Sig;
    // Наследник может ДОБАВИТЬ необязательные параметры, но не убрать.
    bool ok = a.Params.size() >= b.Params.size();
    for (size_t k = b.Params.size(); ok && k < a.Params.size(); ++k)
        ok = a.Params[k].HasDefault || IsAnyish(a.Params[k].T) || a.Params[k].T->Kind == Type::K::Optional;
    for (size_t k = 0; ok && k < b.Params.size(); ++k)
        ok = Assignable(a.Params[k].T, b.Params[k].T);
    if (ok && a.Ret && b.Ret) ok = Assignable(b.Ret, a.Ret);
    if (!ok)
        d.Error(m.P, "Ошибка переопределения",
                {"подпись " + m.Owner->Name + "." + m.Name + SigText(a) + " несовместима с " +
                     base.Owner->Name + "." + base.Name + SigText(b) + " (" + what + ")"});
}

std::string Checker::SigText(const FuncSig& s) {
    std::string out = "(";
    for (size_t k = 0; k < s.Params.size(); ++k)
        out += (k ? ", " : "") + s.Params[k].Name + ": " + TypeName(s.Params[k].T);
    out += ")";
    if (s.Ret) out += ": " + TypeName(s.Ret);
    return out;
}

void Checker::ResolveImport(const Stmt& s) {
    const std::string& from = s.From;
    if (from.rfind("SAGE", 0) == 0) {
        if (from != "SAGE/Core" && from != "SAGE/Math" && from != "SAGE") {
            d.Error(s.FromP, "Ошибка импорта", {"модуля движка '" + from + "' нет", "Есть: SAGE/Core, SAGE/Math."});
            return;
        }
        for (const Stmt::ImportName& n : s.Imports) {
            if (!CoreNames().count(n.Name)) {
                d.Error(n.P, "Ошибка импорта", {"в модуле " + from + " нет '" + n.Name + "'"});
                continue;
            }
            Symbol sym;
            if (const ClassInfo* bc = BuiltinClass(n.Name)) {
                sym.Kind = Symbol::K::Class;
                sym.Class = bc;
                sym.T = ClassRefOf(bc);
            } else {
                sym.T = Any();
            }
            sym.P = n.P;
            Declare(n.As, sym);
        }
        if (!s.ImportAll.empty()) {
            Symbol sym;
            sym.Kind = Symbol::K::Module;
            for (const std::string& n : CoreNames()) {
                Symbol e;
                if (const ClassInfo* bc = BuiltinClass(n)) {
                    e.Kind = Symbol::K::Class;
                    e.Class = bc;
                    e.T = ClassRefOf(bc);
                }
                sym.Exports[n] = e;
            }
            Declare(s.ImportAll, sym);
        }
        return;
    }
    if (!m_mods.Loader) {
        d.Error(s.FromP, "Ошибка импорта", {"импорт модулей недоступен (нет загрузчика)"});
        return;
    }
    std::string resolved, text;
    if (!m_mods.Loader(from, m_path, resolved, text)) {
        d.Error(s.FromP, "Ошибка импорта", {"модуль '" + from + "' не найден"});
        return;
    }
    for (size_t k = 0; k < m_mods.Loading.size(); ++k) {
        if (m_mods.Loading[k] != resolved) continue;
        std::string chain;
        for (size_t j = k; j < m_mods.Loading.size(); ++j) chain += m_mods.Loading[j] + " → ";
        chain += resolved;
        d.Error(s.FromP, "Ошибка импорта", {"циклический импорт: " + chain});
        return;
    }
    m_prog.Imports.push_back(resolved);
    std::shared_ptr<ModuleInfo> mod;
    auto done = m_mods.Done.find(resolved);
    if (done != m_mods.Done.end()) {
        mod = done->second;
    } else {
        mod = std::make_shared<ModuleInfo>();
        mod->Path = resolved;
        const bool isLua = resolved.size() > 4 && resolved.compare(resolved.size() - 4, 4, ".lua") == 0;
        if (!isLua) {
            std::vector<LexError> lexErrors;
            std::vector<Token> toks = Lex(text, lexErrors);
            DiagnosticSink sub(resolved);
            for (const LexError& e : lexErrors) sub.Error(e.P, "Синтаксическая ошибка", {e.Message});
            if (lexErrors.empty()) mod->Ast = Parse(toks, sub);
            if (mod->Ast) {
                m_mods.Loading.push_back(m_path);
                Checker inner(resolved, sub, m_mods, true);
                ProgramInfo info = inner.Run(mod->Ast, &mod->Exports);
                m_mods.Loading.pop_back();
                mod->Classes = info.Classes;
                mod->Enums = info.Enums;
                mod->Events = info.Events;
            }
            for (Diagnostic& diag : sub.Items())
                if (diag.IsError()) d.Items().push_back(diag);
            mod->Ok = !sub.HasErrors();
        } else {
            mod->Ok = true;   // Lua-модуль: что он отдаёт, известно только при запуске
        }
        m_mods.Done[resolved] = mod;
    }
    const bool dynamic = resolved.size() > 4 && resolved.compare(resolved.size() - 4, 4, ".lua") == 0;
    for (const Stmt::ImportName& n : s.Imports) {
        auto it = mod->Exports.find(n.Name);
        if (it == mod->Exports.end()) {
            if (!dynamic) d.Error(n.P, "Ошибка импорта", {"модуль '" + from + "' не экспортирует '" + n.Name + "'"});
            Symbol any;
            any.T = Any();
            Declare(n.As, any);
            continue;
        }
        Declare(n.As, it->second);
    }
    if (!s.ImportAll.empty()) {
        Symbol sym;
        sym.Kind = Symbol::K::Module;
        sym.Exports = mod->Exports;
        sym.T = Any();
        Declare(s.ImportAll, sym);
    }
}

// Экспортируемые значения (функции, константы) для режима «только заголовки».
void Checker::DeclareExportedValue(const StmtPtr& s) {
    if (!s->Exported) return;
    if (s->Kind == Stmt::K::Function && s->FuncName.size() == 1) {
        Symbol sym;
        sym.T = FuncOf(SigOf(*s->Fn));
        m_exports[s->FuncName[0]] = sym;
    } else if (s->Kind == Stmt::K::Local) {
        for (size_t k = 0; k < s->Names.size(); ++k) {
            Symbol sym;
            sym.T = k < s->Types.size() && s->Types[k] ? Resolve(s->Types[k])
                  : (k < s->Exprs.size() ? Widen(LiteralType(s->Exprs[k])) : Any());
            sym.Const = s->DeclKind == Stmt::Decl::Const;
            sym.Typed = true;
            m_exports[s->Names[k]] = sym;
        }
    }
}

} // namespace sage::lang::check
