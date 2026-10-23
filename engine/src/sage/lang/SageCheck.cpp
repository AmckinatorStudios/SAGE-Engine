#include "sage/lang/SageCheck.h"

#include <cmath>
#include <cstdlib>
#include <set>

#include "sage/lang/SageParser.h"

namespace sage::lang {

namespace {

// --- Конструкторы типов -------------------------------------------------------
TypeRef Prim(Type::K k) {
    static std::map<int, TypeRef> cache;
    auto it = cache.find((int)k);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<Type>();
    t->Kind = k;
    cache[(int)k] = t;
    return t;
}
TypeRef Any() { return Prim(Type::K::Any); }
TypeRef NilT() { return Prim(Type::K::Nil); }
TypeRef BoolT() { return Prim(Type::K::Boolean); }
TypeRef NumT() { return Prim(Type::K::Number); }
TypeRef IntT() { return Prim(Type::K::Integer); }
TypeRef StrT() { return Prim(Type::K::String); }
TypeRef TableT() { return Prim(Type::K::Table); }
TypeRef VoidT() { return Prim(Type::K::Void); }

TypeRef Make(Type::K k, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = k;
    t->Args = std::move(args);
    return t;
}
TypeRef InstanceOf(const ClassInfo* c, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Instance;
    t->Class = c;
    t->Args = std::move(args);
    return t;
}
TypeRef ClassRefOf(const ClassInfo* c, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::ClassRef;
    t->Class = c;
    t->Args = std::move(args);
    return t;
}
TypeRef FuncOf(std::shared_ptr<const FuncSig> sig) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Function;
    t->Sig = std::move(sig);
    return t;
}
TypeRef ParamT(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Param;
    t->Name = name;
    return t;
}
TypeRef EnumValueOf(const EnumInfo* e) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::EnumValue;
    t->Enum = e;
    return t;
}
TypeRef OptionalOf(TypeRef inner) {
    if (!inner || inner->Kind == Type::K::Any || inner->Kind == Type::K::Optional ||
        inner->Kind == Type::K::Nil)
        return inner;
    return Make(Type::K::Optional, {std::move(inner)});
}

bool IsNumeric(const TypeRef& t) {
    return t && (t->Kind == Type::K::Number || t->Kind == Type::K::Integer);
}
bool IsAnyish(const TypeRef& t) {
    return !t || t->Kind == Type::K::Any || t->Kind == Type::K::Param;
}
TypeRef First(const TypeRef& t) {
    if (t && t->Kind == Type::K::Tuple) return t->Args.empty() ? NilT() : t->Args[0];
    if (t && t->Kind == Type::K::Void) return NilT();
    return t;
}
// Целое в объявлении без типа (`:=`, поле без типа) расширяется до number:
// `local speed := 0` и потом `speed = 2.5` — обычное дело, а не ошибка.
TypeRef Widen(const TypeRef& t) {
    if (t && t->Kind == Type::K::Integer) return NumT();
    return t;
}

TypeRef Subst(const TypeRef& t, const std::map<std::string, TypeRef>& m) {
    if (!t || m.empty()) return t;
    if (t->Kind == Type::K::Param) {
        auto it = m.find(t->Name);
        return it != m.end() && it->second ? it->second : t;
    }
    if (t->Args.empty() && !t->Sig && t->Fields.empty()) return t;
    auto copy = std::make_shared<Type>(*t);
    for (TypeRef& a : copy->Args) a = Subst(a, m);
    for (auto& f : copy->Fields) f.second = Subst(f.second, m);
    if (t->Sig) {
        auto sig = std::make_shared<FuncSig>(*t->Sig);
        for (ParamInfo& p : sig->Params) p.T = Subst(p.T, m);
        sig->Ret = Subst(sig->Ret, m);
        sig->VarargType = Subst(sig->VarargType, m);
        copy->Sig = sig;
    }
    return copy;
}

std::map<std::string, TypeRef> BindingsOf(const TypeRef& inst) {
    std::map<std::string, TypeRef> m;
    if (!inst || !inst->Class) return m;
    const ClassInfo* c = inst->Class;
    for (size_t k = 0; k < c->TypeParams.size(); ++k)
        m[c->TypeParams[k]] = k < inst->Args.size() ? inst->Args[k] : Any();
    return m;
}

// --- Встроенные типы движка ---------------------------------------------------
struct Builtins {
    std::map<std::string, std::shared_ptr<ClassInfo>> Classes;

    std::shared_ptr<ClassInfo> Add(const std::string& name, bool open, const std::string& engine = {}) {
        auto c = std::make_shared<ClassInfo>();
        c->Kind = ClassInfo::K::Builtin;
        c->Name = name;
        c->Open = open;
        c->EngineName = engine;
        c->Module = "SAGE";
        Classes[name] = c;
        return c;
    }
    static std::shared_ptr<FuncSig> Sig(std::vector<ParamInfo> params, TypeRef ret) {
        auto s = std::make_shared<FuncSig>();
        s->Params = std::move(params);
        s->Ret = std::move(ret);
        return s;
    }
    Builtins() {
        // Component: хуки жизненного цикла — «виртуальные»: переопределяются
        // без override, но подпись сверяется (onUpdate(dt: string) — ошибка).
        auto comp = Add("Component", true);
        auto hook = [&](const char* name, std::vector<ParamInfo> params) {
            MethodInfo m;
            m.Name = name;
            m.Sig = Sig(std::move(params), VoidT());
            m.Virtual = true;
            m.Owner = comp.get();
            comp->Methods.push_back(m);
        };
        hook("onAwake", {});
        hook("onStart", {});
        hook("onUpdate", {{"dt", NumT(), false}});
        hook("onFixedUpdate", {{"dt", NumT(), false}});
        hook("onLateUpdate", {{"dt", NumT(), false}});
        hook("onDestroy", {});
        hook("onEnable", {});
        hook("onDisable", {});
        hook("onCollisionEnter", {{"other", Any(), false}});
        hook("onCollisionExit", {{"other", Any(), false}});
        hook("onTriggerEnter", {{"other", Any(), false}});
        hook("onTriggerExit", {{"other", Any(), false}});
        hook("onTriggerStay", {{"other", Any(), false}});
        hook("onKeyDown", {{"key", StrT(), false}});
        hook("onKeyUp", {{"key", StrT(), false}});
        Add("Entity", true);
        Add("Transform", true, "Transform");
        Add("Camera", true, "Camera");
        Add("RigidBody", true, "RigidBody");
        Add("Collider", true, "Collider");
        Add("AudioSource", true, "Audio");
        Add("Animator", true, "Animation");
        Add("ParticleEmitter", true, "Particles");
        Add("CharacterController", true, "CharacterController");
        Add("Light", true, "Light");
        Add("Mesh", true, "Mesh");
        for (const char* n : {"Vector2", "Vector3", "Vector4", "Quaternion", "Color", "Ray",
                              "RaycastHit", "Bounds", "Texture", "Prefab", "Material", "AudioClip",
                              "SceneHandle", "Task"})
            Add(n, true);
        // Result<T, E> — закрытый: у него ровно эти методы.
        auto res = Add("Result", false);
        res->TypeParams = {"T", "E"};
        auto method = [&](const char* name, std::vector<ParamInfo> params, TypeRef ret) {
            MethodInfo m;
            m.Name = name;
            m.Sig = Sig(std::move(params), std::move(ret));
            m.Owner = res.get();
            res->Methods.push_back(m);
        };
        method("isOk", {}, BoolT());
        method("isErr", {}, BoolT());
        method("value", {}, ParamT("T"));
        method("error", {}, ParamT("E"));
        method("unwrap", {}, ParamT("T"));
        method("unwrapOr", {{"fallback", ParamT("T"), false}}, ParamT("T"));
        method("map", {{"fn", Any(), false}}, Any());
    }
};

const Builtins& TheBuiltins() {
    static const Builtins b;
    return b;
}

// Модули движка для import { ... } from "SAGE/...".
const std::set<std::string>& CoreNames() {
    static const std::set<std::string> s = {
        "Input", "Events", "Time", "Log", "Scene", "Assets", "Timer", "Physics", "Audio",
        "Key", "Mouse", "Result", "Entity", "Component", "Transform", "Camera", "RigidBody",
        "Collider", "AudioSource", "Animator", "ParticleEmitter", "CharacterController", "Light",
        "Mesh", "Debug", "UI", "Tween", "Ease", "Mathf", "Vector2", "Vector3", "Vector4",
        "Quaternion", "Color", "Ray", "Bounds", "Texture", "Prefab", "Material", "AudioClip"};
    return s;
}

// --- Литералы аннотаций -------------------------------------------------------
struct Literal {
    enum class K { None, Bool, Number, String, Table };
    K Kind = K::None;
    bool B = false;
    double N = 0;
    std::string S;
    std::map<std::string, Literal> Fields;
    Pos P;
};

std::string UnquoteLua(const std::string& raw) {
    if (raw.size() >= 2 && (raw[0] == '"' || raw[0] == '\'')) {
        std::string out;
        for (size_t k = 1; k + 1 < raw.size(); ++k) {
            char c = raw[k];
            if (c == '\\' && k + 2 < raw.size()) {
                const char n = raw[++k];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    default: out += n; break;
                }
                continue;
            }
            out += c;
        }
        return out;
    }
    if (raw.size() >= 4 && raw[0] == '[') {
        const size_t open = raw.find('[', 1);
        const size_t level = open - 1;
        return raw.substr(open + 1, raw.size() - (open + 1) - (level + 2));
    }
    return raw;
}

bool EvalLiteral(const ExprPtr& e, Literal& out) {
    if (!e) return false;
    out.P = e->P;
    switch (e->Kind) {
        case Expr::K::True: out.Kind = Literal::K::Bool; out.B = true; return true;
        case Expr::K::False: out.Kind = Literal::K::Bool; out.B = false; return true;
        case Expr::K::Number:
            out.Kind = Literal::K::Number;
            out.N = std::strtod(e->Text.c_str(), nullptr);
            return true;
        case Expr::K::String: out.Kind = Literal::K::String; out.S = UnquoteLua(e->Text); return true;
        case Expr::K::Unary:
            if (e->Op == Tok::Minus && EvalLiteral(e->A, out) && out.Kind == Literal::K::Number) {
                out.N = -out.N;
                return true;
            }
            return false;
        case Expr::K::Paren: return EvalLiteral(e->A, out);
        case Expr::K::Table:
            out.Kind = Literal::K::Table;
            for (const Expr::TField& f : e->Fields) {
                if (f.Kind != Expr::TField::K::Named) return false;
                Literal v;
                if (!EvalLiteral(f.Value, v)) return false;
                out.Fields[f.Name] = v;
            }
            return true;
        default: return false;
    }
}

bool IsLiteralExpr(const ExprPtr& e) {
    if (!e) return false;
    switch (e->Kind) {
        case Expr::K::Nil: case Expr::K::True: case Expr::K::False: case Expr::K::Number:
        case Expr::K::String:
            return true;
        case Expr::K::Unary: return e->Op == Tok::Minus && e->A && e->A->Kind == Expr::K::Number;
        default: return false;
    }
}

bool IntegerLiteral(const std::string& text) {
    if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return text.find_first_of(".pP") == std::string::npos;
    return text.find_first_of(".eE") == std::string::npos;
}

// --- Проверка --------------------------------------------------------------
class Checker {
public:
    Checker(const std::string& path, DiagnosticSink& diag, ModuleCache& mods, bool headerOnly)
        : m_path(path), d(diag), m_mods(mods), m_headerOnly(headerOnly) {}

    ProgramInfo Run(const BlockPtr& ast, std::map<std::string, Symbol>* exportsOut) {
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

private:
    std::string m_path;
    DiagnosticSink& d;
    ModuleCache& m_mods;
    bool m_headerOnly = false;
    ProgramInfo m_prog;
    std::vector<std::map<std::string, Symbol>> m_scopes;
    std::map<std::string, Symbol> m_exports;
    std::map<const Stmt*, std::shared_ptr<ClassInfo>> m_classOf;
    std::map<std::string, std::pair<TypePtr, std::vector<std::string>>> m_aliasSrc;

    struct FnCtx {
        std::shared_ptr<const FuncSig> Sig;
        const ClassInfo* Cls = nullptr;
        bool Static = false;
        bool Init = false;
        std::string Name;
    };
    std::vector<FnCtx> m_fn;
    const ClassInfo* m_cls = nullptr;   // класс, в теле которого стоим (для доступа)

    // --- Области видимости ---------------------------------------------------
    void PushScope() { m_scopes.emplace_back(); }
    void PopScope() { m_scopes.pop_back(); }
    std::map<std::string, Symbol>& Top() { return m_scopes.back(); }
    std::map<std::string, Symbol>& Module() { return m_scopes.front(); }
    Symbol* Lookup(const std::string& name) {
        for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }
    void Declare(const std::string& name, Symbol s, bool moduleLevel = false) {
        (moduleLevel ? Module() : Top())[name] = std::move(s);
    }

    // --- Сообщения -----------------------------------------------------------
    void TypeError(Pos p, const TypeRef& want, const TypeRef& got, const std::string& where,
                   const std::string& gotText = {}) {
        std::vector<std::string> lines = {"Ожидалось значение " + TypeName(want) + ".",
                                          "Получено значение " + TypeName(got) + "."};
        if (!where.empty()) lines.push_back(where);
        if (!gotText.empty()) lines.push_back("Получено: " + gotText);
        d.Error(p, "Ошибка типа", lines);
    }

    static std::string ExprText(const ExprPtr& e) {
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

    // --- Разрешение типов ----------------------------------------------------
    TypeRef Resolve(const TypePtr& t) {
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

    // --- Совместимость -------------------------------------------------------
    bool Assignable(const TypeRef& to, const TypeRef& from) {
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
    void CheckAgainst(const TypeRef& want, const ExprPtr& e, const std::string& where) {
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

    // --- Аннотации ------------------------------------------------------------
    void ParseFieldMeta(const Annotation& a, FieldInfo& f) {
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
    static std::string Num(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }

    void ParseMethodMeta(const std::vector<Annotation>& notes, std::string& deprecated, std::string& side,
                         const char* what) {
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

    void Misplaced(const Annotation& a, const char* what) {
        static const std::set<std::string> known = {"serializable", "class", "field", "method",
                                                     "require", "server", "client"};
        if (!known.count(a.Name))
            d.Error(a.P, "Ошибка аннотации", {"неизвестная аннотация @" + a.Name,
                                              "Есть: @serializable, @class, @field, @method, @require, @server, @client."});
        else
            d.Error(a.P, "Ошибка аннотации", {"@" + a.Name + " неприменима к " + what});
    }

    // --- Проход 1: объявления верхнего уровня ---------------------------------
    void DeclarePass(const StmtPtr& s, bool topLevel) {
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

    void DeclareClass(const StmtPtr& s) {
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

    void DeclareEnum(const Stmt& s) {
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

    void DeclareEvent(const Stmt& s) {
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

    // --- Проход 2: заголовки ---------------------------------------------------
    void ResolvePass(const StmtPtr& s) {
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

    std::shared_ptr<FuncSig> SigOf(const FuncBody& f) {
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

    void ResolveClass(const StmtPtr& s) {
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

    static bool KeepsType(const TypeRef& t) {
        return t && (t->Kind == Type::K::Instance || t->Kind == Type::K::EnumValue);
    }

    TypeRef LiteralType(const ExprPtr& e) {
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

    // --- Проход 3: правила классов ----------------------------------------------
    static bool SameShape(const FuncSig& a, const FuncSig& b) {
        return a.Params.size() == b.Params.size();
    }

    void ValidateClass(ClassInfo& c) {
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

    static bool Inspectable(const TypeRef& t) {
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

    void CheckSignatureCompat(const MethodInfo& m, const MethodInfo& base, const std::string& what) {
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

    static std::string SigText(const FuncSig& s) {
        std::string out = "(";
        for (size_t k = 0; k < s.Params.size(); ++k)
            out += (k ? ", " : "") + s.Params[k].Name + ": " + TypeName(s.Params[k].T);
        out += ")";
        if (s.Ret) out += ": " + TypeName(s.Ret);
        return out;
    }

    // --- Импорт -----------------------------------------------------------------
    void ResolveImport(const Stmt& s) {
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
    void DeclareExportedValue(const StmtPtr& s) {
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

    // --- Проход 4: тела ----------------------------------------------------------
    void CheckBlock(const BlockPtr& b) {
        if (!b) return;
        PushScope();
        for (const StmtPtr& s : b->Stmts) CheckStmt(s, false);
        PopScope();
    }

    void CheckStmt(const StmtPtr& s, bool topLevel) {
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

    void CheckEnumValues(const Stmt& s) {
        if (s.Kind != Stmt::K::Enum) return;
        for (const EnumDecl::Item& it : s.Enum->Items)
            if (it.Value) {
                TypeRef t = Infer(it.Value);
                if (!IsAnyish(t) && !IsNumeric(t) && t->Kind != Type::K::String)
                    d.Error(it.Value->P, "Ошибка типа", {"значение перечисления — число или строка"});
            }
    }

    void CheckLocal(const Stmt& s, bool topLevel) {
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

    void CheckAssign(const Stmt& s) {
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

    void CheckFieldWrite(const TypeRef& obj, const ExprPtr& target, const TypeRef& value, const ExprPtr& valueExpr) {
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

    void CheckAccess(Pos p, const std::string& access, const ClassInfo* owner, const std::string& what) {
        if (access == "public") return;
        const ClassInfo* here = m_cls;
        if (access == "private" && here != owner)
            d.Error(p, "Ошибка доступа", {what + " закрыто (private) — доступно только внутри " + owner->Name});
        else if (access == "protected" && (!here || !here->DerivesFrom(owner)))
            d.Error(p, "Ошибка доступа", {what + " защищено (protected) — доступно в " + owner->Name + " и наследниках"});
    }

    void CheckGenFor(const Stmt& s) {
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

    void CheckFunctionStat(const Stmt& s, bool topLevel) {
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

    void CheckFunction(const FuncBody& f, std::shared_ptr<const FuncSig> sig, const ClassInfo* cls, bool isStatic,
                       const std::string& name) {
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

    void CheckReturn(const Stmt& s) {
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

    void CheckClassBody(const Stmt& s) {
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

    // --- Выражения ---------------------------------------------------------------
    TypeRef Infer(const ExprPtr& e) {
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

    TypeRef InferTable(const ExprPtr& e) {
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
    TypeRef InferField(const ExprPtr& e) {
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

    TypeRef MemberType(const TypeRef& obj, const ExprPtr& e, bool viaColon) {
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

    bool ExpandsLast(const std::vector<ExprPtr>& args) {
        if (args.empty()) return false;
        const Expr::K k = args.back()->Kind;
        return k == Expr::K::Call || k == Expr::K::MethodCall || k == Expr::K::Vararg;
    }

    void CheckArgs(const FuncSig& sig, const std::vector<ExprPtr>& args, Pos p, const std::string& who,
                   const std::map<std::string, TypeRef>& bind) {
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

    TypeRef RetOf(const FuncSig& sig, const std::map<std::string, TypeRef>& bind) {
        if (!sig.Ret) return Any();
        return Subst(sig.Ret, bind);
    }

    // Events.emit(PlayerDied, player) / Events.on(PlayerDied, fn)
    bool CheckEventsCall(const ExprPtr& e) {
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

    TypeRef InferCall(const ExprPtr& e) {
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

    TypeRef Construct(const TypeRef& cls, const ExprPtr& e) {
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

    TypeRef InferMethodCall(const ExprPtr& e) {
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

    TypeRef InferBinary(const ExprPtr& e) {
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
};

} // namespace

bool ClassInfo::IsComponent() const {
    for (const ClassInfo* c = this; c; c = c->Parent)
        if (c->Kind == K::Builtin && c->Name == "Component") return true;
    return false;
}

const ClassInfo* BuiltinClass(const std::string& name) {
    auto it = TheBuiltins().Classes.find(name);
    return it == TheBuiltins().Classes.end() ? nullptr : it->second.get();
}

std::string TypeName(const TypeRef& t) {
    if (!t) return "any";
    switch (t->Kind) {
        case Type::K::Any: return "any";
        case Type::K::Nil: return "nil";
        case Type::K::Boolean: return "boolean";
        case Type::K::Number: return "number";
        case Type::K::Integer: return "integer";
        case Type::K::String: return "string";
        case Type::K::Table: return "table";
        case Type::K::Void: return "void";
        case Type::K::Function: {
            if (!t->Sig) return "function";
            std::string s = "function(";
            for (size_t k = 0; k < t->Sig->Params.size(); ++k) s += (k ? ", " : "") + TypeName(t->Sig->Params[k].T);
            s += ")";
            if (t->Sig->Ret) s += ": " + TypeName(t->Sig->Ret);
            return s;
        }
        case Type::K::Instance: {
            std::string s = t->Class->Name;
            if (!t->Args.empty()) {
                s += "<";
                for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeName(t->Args[k]);
                s += ">";
            }
            return s;
        }
        case Type::K::ClassRef: return "класс " + t->Class->Name;
        case Type::K::EnumValue: return t->Enum->Name;
        case Type::K::EnumRef: return "перечисление " + t->Enum->Name;
        case Type::K::EventRef: return "событие " + t->Event->Name;
        case Type::K::Array: return TypeName(t->Args[0]) + "[]";
        case Type::K::Map: return "map<" + TypeName(t->Args[0]) + ", " + TypeName(t->Args[1]) + ">";
        case Type::K::Optional: return TypeName(t->Args[0]) + "?";
        case Type::K::Union: {
            std::string s;
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? " | " : "") + TypeName(t->Args[k]);
            return s;
        }
        case Type::K::Tuple: {
            std::string s = "(";
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeName(t->Args[k]);
            return s + ")";
        }
        case Type::K::Param: return t->Name;
        case Type::K::Record: {
            if (t->Fields.empty()) return "{}";
            std::string s = "{ ";
            for (size_t k = 0; k < t->Fields.size(); ++k)
                s += (k ? ", " : "") + t->Fields[k].first + ": " + TypeName(t->Fields[k].second);
            return s + " }";
        }
    }
    return "any";
}

ProgramInfo Check(const BlockPtr& ast, const std::string& path, DiagnosticSink& diag, ModuleCache& modules) {
    Checker c(path, diag, modules, false);
    return c.Run(ast, nullptr);
}

} // namespace sage::lang
