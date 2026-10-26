#pragma once
#include "sage/lang/SageCheck.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <set>
#include "sage/lang/SageApi.h"
#include "sage/lang/SageParser.h"

// ---------------------------------------------------------------------------
// Внутренняя кухня проверки типов SAGE Lua, общая для файлов SageCheck*.cpp.
//
// SageCheck.cpp был одним классом на две с лишним тысячи строк. Теперь класс
// объявлен здесь, а его тела разложены по проходам проверки:
//   SageCheck_Types.cpp — области видимости, разрешение и совместимость типов,
//                         аннотации;
//   SageCheck_Decls.cpp — проходы 1–3 (объявления, заголовки, правила классов)
//                         и импорт;
//   SageCheck_Body.cpp  — проход 4: тела функций и операторы;
//   SageCheck_Expr.cpp  — выражения;
//   SageCheck_Api.cpp   — функции API движка (таблица SageApi.h) и
//                         предупреждения об именах, которых нигде нет;
//   SageCheck.cpp       — вход (Check) и общедоступные помощники.
// ---------------------------------------------------------------------------
namespace sage::lang::check {

// --- Конструкторы типов -------------------------------------------------------
inline TypeRef Prim(Type::K k) {
    static std::map<int, TypeRef> cache;
    auto it = cache.find((int)k);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<Type>();
    t->Kind = k;
    cache[(int)k] = t;
    return t;
}
inline TypeRef Any() { return Prim(Type::K::Any); }
inline TypeRef NilT() { return Prim(Type::K::Nil); }
inline TypeRef BoolT() { return Prim(Type::K::Boolean); }
inline TypeRef NumT() { return Prim(Type::K::Number); }
inline TypeRef IntT() { return Prim(Type::K::Integer); }
inline TypeRef StrT() { return Prim(Type::K::String); }
inline TypeRef TableT() { return Prim(Type::K::Table); }
inline TypeRef VoidT() { return Prim(Type::K::Void); }

inline TypeRef Make(Type::K k, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = k;
    t->Args = std::move(args);
    return t;
}
inline TypeRef InstanceOf(const ClassInfo* c, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Instance;
    t->Class = c;
    t->Args = std::move(args);
    return t;
}
inline TypeRef ClassRefOf(const ClassInfo* c, std::vector<TypeRef> args = {}) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::ClassRef;
    t->Class = c;
    t->Args = std::move(args);
    return t;
}
inline TypeRef FuncOf(std::shared_ptr<const FuncSig> sig) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Function;
    t->Sig = std::move(sig);
    return t;
}
inline TypeRef ParamT(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::Param;
    t->Name = name;
    return t;
}
inline TypeRef EnumValueOf(const EnumInfo* e) {
    auto t = std::make_shared<Type>();
    t->Kind = Type::K::EnumValue;
    t->Enum = e;
    return t;
}
inline TypeRef OptionalOf(TypeRef inner) {
    if (!inner || inner->Kind == Type::K::Any || inner->Kind == Type::K::Optional ||
        inner->Kind == Type::K::Nil)
        return inner;
    return Make(Type::K::Optional, {std::move(inner)});
}

inline bool IsNumeric(const TypeRef& t) {
    return t && (t->Kind == Type::K::Number || t->Kind == Type::K::Integer);
}
inline bool IsAnyish(const TypeRef& t) {
    return !t || t->Kind == Type::K::Any || t->Kind == Type::K::Param;
}
inline TypeRef First(const TypeRef& t) {
    if (t && t->Kind == Type::K::Tuple) return t->Args.empty() ? NilT() : t->Args[0];
    if (t && t->Kind == Type::K::Void) return NilT();
    return t;
}
// Целое в объявлении без типа (`:=`, поле без типа) расширяется до number:
// `local speed := 0` и потом `speed = 2.5` — обычное дело, а не ошибка.
inline TypeRef Widen(const TypeRef& t) {
    if (t && t->Kind == Type::K::Integer) return NumT();
    return t;
}

inline TypeRef Subst(const TypeRef& t, const std::map<std::string, TypeRef>& m) {
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

inline std::map<std::string, TypeRef> BindingsOf(const TypeRef& inst) {
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

inline const Builtins& TheBuiltins() {
    static const Builtins b;
    return b;
}

// Модули движка для import { ... } from "SAGE/...".
inline const std::set<std::string>& CoreNames() {
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

inline std::string UnquoteLua(const std::string& raw) {
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

inline bool EvalLiteral(const ExprPtr& e, Literal& out) {
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

inline bool IsLiteralExpr(const ExprPtr& e) {
    if (!e) return false;
    switch (e->Kind) {
        case Expr::K::Nil: case Expr::K::True: case Expr::K::False: case Expr::K::Number:
        case Expr::K::String:
            return true;
        case Expr::K::Unary: return e->Op == Tok::Minus && e->A && e->A->Kind == Expr::K::Number;
        default: return false;
    }
}

inline bool IntegerLiteral(const std::string& text) {
    if (text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return text.find_first_of(".pP") == std::string::npos;
    return text.find_first_of(".eE") == std::string::npos;
}

// --- Проверка --------------------------------------------------------------

class Checker {
public:
    Checker(const std::string& path, DiagnosticSink& diag, ModuleCache& mods, bool headerOnly)
        : m_path(path), d(diag), m_mods(mods), m_headerOnly(headerOnly) {}

    ProgramInfo Run(const BlockPtr& ast, std::map<std::string, Symbol>* exportsOut);
    // Строгая проверка: ещё и предупреждения об именах, которых нет ни в
    // скрипте, ни в API движка (опечатки). Только по просьбе — инструмент
    // «Проверить скрипты» и тесты; загрузка игры её не включает.
    void SetLint(bool on) { m_lint = on; }

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

    // --- API движка (SageCheck_Api.cpp) ---------------------------------------
    bool m_lint = false;
    // Глобальные, которые скрипт задаёт сам (function Update, score = 0): им
    // не навязывается сигнатура API с тем же именем, и они не «неизвестны».
    std::set<std::string> m_scriptGlobals;
    std::set<std::string> m_warnedNames;   // одно предупреждение на имя
    std::map<std::string, TypeRef> m_apiTypes;
    void CollectScriptGlobals(const BlockPtr& b);
    // a.b.c из цепочки полей с корнем-глобальной; false — не такая цепочка.
    bool GlobalPath(const ExprPtr& e, std::string& out);
    // Тип функции API (с сигнатурой) или nullptr.
    TypeRef ApiType(const std::string& dotted);
    // Имя верхнего уровня, которого нет нигде: предупредить (в строгом режиме).
    void UnknownGlobal(const ExprPtr& e);
    std::map<std::string, Symbol>& Top() { return m_scopes.back(); }
    std::map<std::string, Symbol>& Module() { return m_scopes.front(); }
    Symbol* Lookup(const std::string& name);
    void Declare(const std::string& name, Symbol s, bool moduleLevel = false) {
        (moduleLevel ? Module() : Top())[name] = std::move(s);
    }

    // --- Сообщения -----------------------------------------------------------
    void TypeError(Pos p, const TypeRef& want, const TypeRef& got, const std::string& where,
                   const std::string& gotText = {});

    static std::string ExprText(const ExprPtr& e);

    // --- Разрешение типов ----------------------------------------------------
    TypeRef Resolve(const TypePtr& t);

    // --- Совместимость -------------------------------------------------------
    bool Assignable(const TypeRef& to, const TypeRef& from);

    // Значение выражения против ожидаемого типа: таблица-литерал проверяется
    // ПОЭЛЕМЕНТНО — сообщение указывает на тот самый элемент, а не на скобку.
    void CheckAgainst(const TypeRef& want, const ExprPtr& e, const std::string& where);

    // --- Аннотации ------------------------------------------------------------
    void ParseFieldMeta(const Annotation& a, FieldInfo& f);
    static std::string Num(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }

    void ParseMethodMeta(const std::vector<Annotation>& notes, std::string& deprecated, std::string& side,
                         const char* what);

    void Misplaced(const Annotation& a, const char* what);

    // --- Проход 1: объявления верхнего уровня ---------------------------------
    void DeclarePass(const StmtPtr& s, bool topLevel);

    void DeclareClass(const StmtPtr& s);

    void DeclareEnum(const Stmt& s);

    void DeclareEvent(const Stmt& s);

    // --- Проход 2: заголовки ---------------------------------------------------
    void ResolvePass(const StmtPtr& s);

    std::shared_ptr<FuncSig> SigOf(const FuncBody& f);

    void ResolveClass(const StmtPtr& s);

    static bool KeepsType(const TypeRef& t) {
        return t && (t->Kind == Type::K::Instance || t->Kind == Type::K::EnumValue);
    }

    TypeRef LiteralType(const ExprPtr& e);

    // --- Проход 3: правила классов ----------------------------------------------
    static bool SameShape(const FuncSig& a, const FuncSig& b) {
        return a.Params.size() == b.Params.size();
    }

    void ValidateClass(ClassInfo& c);

    static bool Inspectable(const TypeRef& t);

    void CheckSignatureCompat(const MethodInfo& m, const MethodInfo& base, const std::string& what);

    static std::string SigText(const FuncSig& s);

    // --- Импорт -----------------------------------------------------------------
    void ResolveImport(const Stmt& s);

    // Экспортируемые значения (функции, константы) для режима «только заголовки».
    void DeclareExportedValue(const StmtPtr& s);

    // --- Проход 4: тела ----------------------------------------------------------
    void CheckBlock(const BlockPtr& b) {
        if (!b) return;
        PushScope();
        for (const StmtPtr& s : b->Stmts) CheckStmt(s, false);
        PopScope();
    }

    void CheckStmt(const StmtPtr& s, bool topLevel);

    void CheckEnumValues(const Stmt& s);

    void CheckLocal(const Stmt& s, bool topLevel);

    void CheckAssign(const Stmt& s);

    void CheckFieldWrite(const TypeRef& obj, const ExprPtr& target, const TypeRef& value, const ExprPtr& valueExpr);

    void CheckAccess(Pos p, const std::string& access, const ClassInfo* owner, const std::string& what);

    void CheckGenFor(const Stmt& s);

    void CheckFunctionStat(const Stmt& s, bool topLevel);

    void CheckFunction(const FuncBody& f, std::shared_ptr<const FuncSig> sig, const ClassInfo* cls, bool isStatic,
                       const std::string& name);

    void CheckReturn(const Stmt& s);

    void CheckClassBody(const Stmt& s);

    // --- Выражения ---------------------------------------------------------------
    TypeRef Infer(const ExprPtr& e);

    TypeRef InferTable(const ExprPtr& e);

    // Член по точке: поле, метод, статический член, значение перечисления.
    TypeRef InferField(const ExprPtr& e);

    TypeRef MemberType(const TypeRef& obj, const ExprPtr& e, bool viaColon);

    bool ExpandsLast(const std::vector<ExprPtr>& args) {
        if (args.empty()) return false;
        const Expr::K k = args.back()->Kind;
        return k == Expr::K::Call || k == Expr::K::MethodCall || k == Expr::K::Vararg;
    }

    void CheckArgs(const FuncSig& sig, const std::vector<ExprPtr>& args, Pos p, const std::string& who,
                   const std::map<std::string, TypeRef>& bind);

    TypeRef RetOf(const FuncSig& sig, const std::map<std::string, TypeRef>& bind) {
        if (!sig.Ret) return Any();
        return Subst(sig.Ret, bind);
    }

    // Events.emit(PlayerDied, player) / Events.on(PlayerDied, fn)
    bool CheckEventsCall(const ExprPtr& e);

    TypeRef InferCall(const ExprPtr& e);

    TypeRef Construct(const TypeRef& cls, const ExprPtr& e);

    TypeRef InferMethodCall(const ExprPtr& e);

    TypeRef InferBinary(const ExprPtr& e);
};

} // namespace sage::lang::check
