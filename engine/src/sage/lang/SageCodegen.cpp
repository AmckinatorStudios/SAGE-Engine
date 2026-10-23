#include "sage/lang/SageCodegen.h"

#include <cctype>

namespace sage::lang {

namespace {

// Строковый литерал Lua из произвольного текста.
std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out + "\"";
}

const char* OpText(Tok k) {
    switch (k) {
        case Tok::Or: return "or";
        case Tok::And: return "and";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::Ne: return "~=";
        case Tok::Eq: return "==";
        case Tok::Pipe: return "|";
        case Tok::Tilde: return "~";
        case Tok::Amp: return "&";
        case Tok::Shl: return "<<";
        case Tok::Shr: return ">>";
        case Tok::Concat: return "..";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::DSlash: return "//";
        case Tok::Percent: return "%";
        case Tok::Caret: return "^";
        case Tok::Not: return "not";
        case Tok::Hash: return "#";
        default: return "?";
    }
}

// Имена, которые файл .slua видит готовыми: обёртки движка в стиле SAGE.
// Затеняются ЛОКАЛЬНО в файле — обычные Lua-скрипты рядом продолжают видеть
// прежние Input/Scene/Time без изменений.
const char* kApiNames[] = {"Events", "Input", "Time", "Scene", "Log", "Timer", "Assets", "Physics",
                           "Result", "Key", "Mouse", "Mathf", "Component", "Entity", "Vector2",
                           "Vector3", "Quaternion", "Transform", "Camera", "RigidBody", "Collider",
                           "AudioSource", "Animator", "ParticleEmitter", "CharacterController",
                           "Light", "Mesh"};

class Gen {
public:
    Gen(const ProgramInfo& info, const std::string& path) : m_info(info), m_path(path) {}

    CodegenResult Run(const BlockPtr& ast) {
        CodegenResult r;
        // Шапка — на ПЕРВОЙ строке, без перевода строки: номера строк кода
        // остаются номерами строк исходника.
        std::string names, values;
        for (const char* n : kApiNames) {
            names += std::string(names.empty() ? "" : ",") + n;
            values += std::string(values.empty() ? "" : ",") + "__sage.api." + n;
        }
        Raw("local __sage=__sage;local " + names + "=" + values + ";local __M={};");
        bool explicitReturn = false;
        if (ast) {
            explicitReturn = !ast->Stmts.empty() && ast->Stmts.back()->Kind == Stmt::K::Return;
            for (size_t k = 0; k < ast->Stmts.size(); ++k) {
                // Свой return у файла — последний оператор Lua: регистрация
                // модуля встаёт ПЕРЕД ним, иначе она была бы недостижимым кодом
                // и синтаксической ошибкой.
                if (explicitReturn && k + 1 == ast->Stmts.size()) {
                    At(ast->Stmts[k]->P);
                    W("__sage.module(" + Quote(m_path) + ",__M);");
                }
                Stmt_(ast->Stmts[k], true);
            }
        }
        // Главный компонент файла: одноимённый файлу или последний объявленный.
        std::string base = m_path;
        const size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        for (const auto& c : m_info.Classes) {
            if (c->Kind != ClassInfo::K::Class || !c->IsComponent()) continue;
            if (r.MainClass.empty() || c->Name == base) r.MainClass = c->Name;
            if (c->Name == base) break;
        }
        r.HasExports = m_hasExports;
        if (!explicitReturn) {
            Raw(" __sage.module(" + Quote(m_path) + ",__M)");
            if (!r.MainClass.empty()) Raw(" return " + r.MainClass);
            else if (m_hasExports) Raw(" return __M");
        }
        Raw("\n");
        r.Lua = std::move(m_out);
        return r;
    }

private:
    const ProgramInfo& m_info;
    std::string m_path;
    std::string m_out;
    int m_line = 1;
    bool m_hasExports = false;
    int m_tmp = 0;
    // Класс, метод которого сейчас генерируется (для super).
    std::vector<std::string> m_classStack;

    // --- Вывод -------------------------------------------------------------
    void Raw(const std::string& s) {
        for (char c : s)
            if (c == '\n') ++m_line;
        m_out += s;
    }
    // Токен: всегда через пробел — склейки вроде «a--b» (комментарий!) или
    // «x..y» с числом невозможны по построению.
    void W(const std::string& s) {
        if (!m_out.empty() && m_out.back() != '\n' && m_out.back() != ' ') m_out += ' ';
        Raw(s);
    }
    void At(Pos p) {
        while (m_line < p.Line) {
            m_out += '\n';
            ++m_line;
        }
    }

    // --- Выражения -----------------------------------------------------------
    void ExprList(const std::vector<ExprPtr>& list) {
        for (size_t k = 0; k < list.size(); ++k) {
            if (k) W(",");
            Expr_(list[k]);
        }
    }

    void Expr_(const ExprPtr& e) {
        if (!e) {
            W("nil");
            return;
        }
        // Строка — только у узлов, которые НАЧИНАЮТСЯ своим токеном: у
        // бинарного узла и вызова позиция — оператор/скобка, правее начала, и
        // переход на неё раньше времени сдвигал бы левый операнд ниже.
        switch (e->Kind) {
            case Expr::K::Nil: case Expr::K::True: case Expr::K::False: case Expr::K::Number:
            case Expr::K::String: case Expr::K::Vararg: case Expr::K::Name: case Expr::K::Super:
            case Expr::K::Paren: case Expr::K::Function: case Expr::K::Table: case Expr::K::Await:
                At(e->P);
                break;
            default: break;
        }
        switch (e->Kind) {
            case Expr::K::Nil: W("nil"); return;
            case Expr::K::True: W("true"); return;
            case Expr::K::False: W("false"); return;
            case Expr::K::Number: case Expr::K::String: W(e->Text); return;
            case Expr::K::Vararg: W("..."); return;
            case Expr::K::Name: W(e->Text); return;
            case Expr::K::Super: W(SuperRef()); return;
            case Expr::K::Paren:
                W("(");
                Expr_(e->A);
                W(")");
                return;
            case Expr::K::Generic: Expr_(e->A); return;   // аргументы типа стираются
            case Expr::K::Await:
                W("__sage.await(");
                Expr_(e->A);
                W(")");
                return;
            case Expr::K::Function: FuncExpr(*e->Fn); return;
            case Expr::K::Table: Table(e); return;
            case Expr::K::Binary:
                // Скобки вокруг каждого бинарного узла: приоритеты SAGE и Lua
                // совпадают, но так генератор не обязан их знать.
                W("(");
                Expr_(e->A);
                W(OpText(e->Op));
                Expr_(e->B);
                W(")");
                return;
            case Expr::K::Unary:
                W("(");
                W(OpText(e->Op));
                Expr_(e->A);
                W(")");
                return;
            case Expr::K::Field:
                if (e->A->Kind == Expr::K::Super) {
                    W(SuperRef() + "." + e->Text);
                    return;
                }
                Prefix(e->A);
                Raw("." + e->Text);
                return;
            case Expr::K::Index:
                Prefix(e->A);
                W("[");
                Expr_(e->B);
                W("]");
                return;
            case Expr::K::Call: {
                // super.init(x) / super.update(dt) — вызов реализации родителя
                // с ТЕКУЩИМ self.
                if (e->A->Kind == Expr::K::Field && e->A->A->Kind == Expr::K::Super) {
                    W(SuperRef() + "." + e->A->Text + "(self");
                    for (const ExprPtr& a : e->Args) {
                        W(",");
                        Expr_(a);
                    }
                    W(")");
                    return;
                }
                Prefix(e->A);
                W("(");
                ExprList(e->Args);
                W(")");
                return;
            }
            case Expr::K::MethodCall:
                if (e->A->Kind == Expr::K::Super) {
                    W(SuperRef() + "." + e->Text + "(self");
                    for (const ExprPtr& a : e->Args) {
                        W(",");
                        Expr_(a);
                    }
                    W(")");
                    return;
                }
                Prefix(e->A);
                Raw(":" + e->Text);
                W("(");
                ExprList(e->Args);
                W(")");
                return;
        }
    }

    // Префикс вызова/индекса: строковый и табличный литерал, функция — в скобках
    // ("abc"):upper() требует скобок в Lua.
    void Prefix(const ExprPtr& e) {
        if (e->Kind == Expr::K::String || e->Kind == Expr::K::Table || e->Kind == Expr::K::Function ||
            e->Kind == Expr::K::Number || e->Kind == Expr::K::Nil || e->Kind == Expr::K::True ||
            e->Kind == Expr::K::False || e->Kind == Expr::K::Binary || e->Kind == Expr::K::Unary) {
            W("(");
            Expr_(e);
            W(")");
            return;
        }
        Expr_(e);
    }

    std::string SuperRef() const {
        if (m_classStack.empty()) return "nil";
        return m_classStack.back() + ".__parent";
    }

    void Table(const ExprPtr& e) {
        W("{");
        for (size_t k = 0; k < e->Fields.size(); ++k) {
            const Expr::TField& f = e->Fields[k];
            At(f.P);
            if (f.Kind == Expr::TField::K::Keyed) {
                W("[");
                Expr_(f.Key);
                W("]");
                W("=");
            } else if (f.Kind == Expr::TField::K::Named) {
                W(f.Name);
                W("=");
            }
            Expr_(f.Value);
            W(",");
        }
        W("}");
    }

    // --- Функции --------------------------------------------------------------
    void Params(const FuncBody& f) {
        W("(");
        bool first = true;
        for (const Param& p : f.Params) {
            if (!first) W(",");
            W(p.Name);
            first = false;
        }
        if (f.Vararg) {
            if (!first) W(",");
            W("...");
        }
        W(")");
    }

    // Пролог тела: значения по умолчанию и именованный vararg — на строке заголовка.
    void Prologue(const FuncBody& f) {
        for (const Param& p : f.Params) {
            if (!p.Default) continue;
            W("if " + p.Name + " == nil then " + p.Name + " =");
            Expr_(p.Default);
            W("end;");
        }
        if (f.Vararg && !f.VarargName.empty()) W("local " + f.VarargName + " = {...};");
    }

    void Body(const FuncBody& f) {
        if (f.ArrowExpr) {
            W("return");
            Expr_(f.ArrowExpr);
            return;
        }
        if (f.Body)
            for (const StmtPtr& s : f.Body->Stmts) Stmt_(s, false);
    }

    // Тело функции целиком: async оборачивается в задачу планировщика. Вызов
    // async-функции сразу выполняет её до первого await и отдаёт задачу —
    // её можно ждать (await), отменить или бросить.
    void FuncTail(const FuncBody& f) {
        Prologue(f);
        if (f.Async) {
            // Аргументы — внутрь задачи: `...` async-функции доступен в её теле.
            W(f.Vararg ? "return __sage.run(function(...)" : "return __sage.run(function()");
            Body(f);
            At(f.EndP);
            W(f.Vararg ? "end, ...)" : "end)");
        } else {
            Body(f);
        }
        At(f.EndP);
        W("end");
    }

    void FuncExpr(const FuncBody& f) {
        W("function");
        Params(f);
        FuncTail(f);
    }

    // --- Операторы -------------------------------------------------------------
    void Block_(const BlockPtr& b) {
        if (!b) return;
        for (const StmtPtr& s : b->Stmts) Stmt_(s, false);
    }

    void Stmt_(const StmtPtr& s, bool top) {
        At(s->P);
        switch (s->Kind) {
            case Stmt::K::Local: {
                W("local");
                for (size_t k = 0; k < s->Names.size(); ++k) {
                    if (k) W(",");
                    W(s->Names[k]);
                    if (k < s->Attribs.size() && !s->Attribs[k].empty()) W("<" + s->Attribs[k] + ">");
                }
                if (!s->Exprs.empty()) {
                    W("=");
                    ExprList(s->Exprs);
                }
                W(";");
                if (s->Exported && top) {
                    for (const std::string& n : s->Names) W("__M." + n + " = " + n + ";");
                    m_hasExports = true;
                }
                return;
            }
            case Stmt::K::Assign:
                ExprList(s->Targets);
                W("=");
                ExprList(s->Exprs);
                W(";");
                return;
            case Stmt::K::Call:
                Expr_(s->CallE);
                W(";");
                return;
            case Stmt::K::Do:
                W("do");
                Block_(s->Body);
                At(s->Body->EndP);
                W("end");
                return;
            case Stmt::K::While:
                W("while");
                Expr_(s->Cond);
                W("do");
                Block_(s->Body);
                At(s->Body->EndP);
                W("end");
                return;
            case Stmt::K::Repeat:
                W("repeat");
                Block_(s->Body);
                At(s->Body->EndP);
                W("until");
                Expr_(s->Cond);
                return;
            case Stmt::K::If:
                for (size_t k = 0; k < s->Clauses.size(); ++k) {
                    W(k == 0 ? "if" : "elseif");
                    Expr_(s->Clauses[k].first);
                    W("then");
                    Block_(s->Clauses[k].second);
                    At(s->Clauses[k].second->EndP);
                }
                if (s->Else) {
                    W("else");
                    Block_(s->Else);
                    At(s->Else->EndP);
                }
                W("end");
                return;
            case Stmt::K::NumFor:
                W("for " + s->Names[0] + " =");
                ExprList(s->Exprs);
                W("do");
                Block_(s->Body);
                At(s->Body->EndP);
                W("end");
                return;
            case Stmt::K::GenFor:
                W("for");
                for (size_t k = 0; k < s->Names.size(); ++k) {
                    if (k) W(",");
                    W(s->Names[k]);
                }
                W("in");
                ExprList(s->Exprs);
                W("do");
                Block_(s->Body);
                At(s->Body->EndP);
                W("end");
                return;
            case Stmt::K::Function: {
                std::string name;
                for (size_t k = 0; k < s->FuncName.size(); ++k) name += (k ? "." : "") + s->FuncName[k];
                if (!s->Method.empty()) name += ":" + s->Method;
                // Экспортируемая функция — локальная модуля (не глобальная окружения).
                if (s->Exported) W("local");
                W("function " + name);
                Params(*s->Fn);
                FuncTail(*s->Fn);
                W(";");
                SideWrap(s->Annotations, name, !s->Method.empty() ? std::string() : name);
                if (s->Exported && top) {
                    W("__M." + name + " = " + name + ";");
                    m_hasExports = true;
                }
                return;
            }
            case Stmt::K::LocalFunction:
                W("local function " + s->FuncName[0]);
                Params(*s->Fn);
                FuncTail(*s->Fn);
                W(";");
                SideWrap(s->Annotations, s->FuncName[0], s->FuncName[0]);
                return;
            case Stmt::K::Return:
                W("return");
                ExprList(s->Exprs);
                W(";");
                return;
            case Stmt::K::Break: W("break;"); return;
            case Stmt::K::Goto: W("goto " + s->Text + ";"); return;
            case Stmt::K::Label: W("::" + s->Text + "::"); return;
            case Stmt::K::Class: Class_(*s, top); return;
            case Stmt::K::Enum: Enum_(*s, top); return;
            case Stmt::K::Event: Event_(*s, top); return;
            case Stmt::K::TypeAlias: return;   // тип существует только для компилятора
            case Stmt::K::Import: Import_(*s); return;
        }
    }

    // @server/@client у функции: вызов вне своей стороны ничего не делает.
    void SideWrap(const std::vector<Annotation>& notes, const std::string& display, const std::string& target) {
        for (const Annotation& a : notes) {
            if ((a.Name == "server" || a.Name == "client") && !target.empty())
                W(target + " = __sage.side(" + target + "," + Quote(a.Name) + "," + Quote(display) + ");");
        }
    }

    void Import_(const Stmt& s) {
        const bool engine = s.From.rfind("SAGE", 0) == 0;
        const std::string mod = engine ? std::string("__sage.core")
                                       : "__sage.import(" + Quote(s.From) + "," + Quote(m_path) + ")";
        if (!s.ImportAll.empty()) {
            W("local " + s.ImportAll + " = " + mod + ";");
            return;
        }
        if (s.Imports.empty()) {
            W(mod + ";");
            return;
        }
        const std::string tmp = "__i" + std::to_string(++m_tmp);
        W("local " + tmp + " = " + mod + ";");
        std::string names, values;
        for (const Stmt::ImportName& n : s.Imports) {
            names += (names.empty() ? "" : ",") + n.As;
            values += (values.empty() ? "" : ",") + tmp + "." + n.Name;
        }
        W("local " + names + " = " + values + ";");
    }

    void Enum_(const Stmt& s, bool top) {
        const EnumDecl& e = *s.Enum;
        W("local " + e.Name + " = __sage.enum(" + Quote(e.Name) + ");");
        for (const EnumDecl::Item& it : e.Items) {
            At(it.P);
            W("__sage.item(" + e.Name + "," + Quote(it.Name));
            if (it.Value) {
                W(",");
                Expr_(it.Value);
            }
            W(");");
        }
        At(e.EndP);
        W("__sage.seal(" + e.Name + ");");
        if (s.Exported && top) {
            W("__M." + e.Name + " = " + e.Name + ";");
            m_hasExports = true;
        }
    }

    void Event_(const Stmt& s, bool top) {
        const EventDecl& e = *s.Event;
        std::string params;
        for (const Param& p : e.Params) params += (params.empty() ? "" : ",") + Quote(p.Name);
        W("local " + e.Name + " = __sage.event(" + Quote(e.Name) + ",{" + params + "});");
        if (s.Exported && top) {
            W("__M." + e.Name + " = " + e.Name + ";");
            m_hasExports = true;
        }
    }

    // --- Классы ----------------------------------------------------------------
    static std::string TypeLiteral(const TypeRef& t) { return Quote(TypeName(t)); }

    std::string LiteralText(const ExprPtr& e) {
        if (!e) return "nil";
        switch (e->Kind) {
            case Expr::K::Number: case Expr::K::String: return e->Text;
            case Expr::K::True: return "true";
            case Expr::K::False: return "false";
            case Expr::K::Nil: return "nil";
            case Expr::K::Unary:
                if (e->Op == Tok::Minus && e->A && e->A->Kind == Expr::K::Number) return "-" + e->A->Text;
                return "nil";
            default: return "nil";
        }
    }

    // Описание класса для редактора, сериализации и сети — одной строкой Lua.
    std::string Meta(const ClassInfo& c) {
        std::string m = "{kind=" + Quote(c.Kind == ClassInfo::K::Interface ? "interface"
                                          : c.Kind == ClassInfo::K::Struct ? "struct" : "class");
        if (c.IsComponent()) m += ",component=true";
        if (c.Serializable) m += ",serializable=true";
        if (!c.Side.empty()) m += ",side=" + Quote(c.Side);
        if (!c.DisplayName.empty()) m += ",name=" + Quote(c.DisplayName);
        if (!c.Requires.empty()) {
            m += ",require={";
            for (const std::string& r : c.Requires) m += Quote(r) + ",";
            m += "}";
        }
        if (!c.Interfaces.empty()) {
            m += ",interfaces={";
            for (const ClassInfo* i : c.Interfaces) m += i->Name + ",";
            m += "}";
        }
        m += ",fields={";
        for (const FieldInfo& f : c.Fields) {
            if (f.Static) continue;
            m += "{name=" + Quote(f.Name) + ",type=" + TypeLiteral(f.T) + ",access=" + Quote(f.Access);
            if (f.DefaultIsLiteral) m += ",default=" + LiteralText(f.Default);
            if (f.Meta.Editable) m += ",editable=true";
            if (!f.Meta.Label.empty()) m += ",label=" + Quote(f.Meta.Label);
            if (!f.Meta.Tooltip.empty()) m += ",tooltip=" + Quote(f.Meta.Tooltip);
            if (f.Meta.HasRange) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), ",range={min=%.17g,max=%.17g,step=%.17g}", f.Meta.Min, f.Meta.Max,
                              f.Meta.Step);
                m += buf;
            }
            if (f.Meta.Replicated) m += ",replicated=true";
            if (!f.Meta.Serialize) m += ",serialize=false";
            if (f.Const) m += ",readonly=true";
            m += "},";
        }
        m += "},methods={";
        for (const MethodInfo& mi : c.Methods) {
            m += "{name=" + Quote(mi.Name) + ",access=" + Quote(mi.Access);
            if (mi.Static) m += ",static=true";
            if (!mi.Deprecated.empty()) m += ",deprecated=" + Quote(mi.Deprecated);
            if (!mi.Side.empty()) m += ",side=" + Quote(mi.Side);
            m += "},";
        }
        m += "}}";
        return m;
    }

    void Class_(const Stmt& s, bool top) {
        const ClassDecl& decl = *s.Class;
        auto it = m_info.ByDecl.find(&decl);
        const ClassInfo* info = it != m_info.ByDecl.end() ? it->second : nullptr;
        if (decl.Kind == ClassDecl::K::Interface) {
            std::string methods, parents;
            for (const Member& m : decl.Members)
                if (m.Kind == Member::K::Method) methods += (methods.empty() ? "" : ",") + Quote(m.Name);
            if (info)
                for (const ClassInfo* p : info->Interfaces) parents += (parents.empty() ? "" : ",") + p->Name;
            W("local " + decl.Name + " = __sage.interface(" + Quote(decl.Name) + ",{" + methods + "},{" + parents + "});");
            At(decl.EndP);
            if (s.Exported && top) {
                W("__M." + decl.Name + " = " + decl.Name + ";");
                m_hasExports = true;
            }
            return;
        }
        const std::string& C = decl.Name;
        if (decl.Kind == ClassDecl::K::Struct) {
            W("local " + C + " = __sage.struct(" + Quote(C) + "," + Quote(m_path) + ");");
        } else {
            std::string parent = "nil";
            if (decl.Extends && decl.Extends->Kind == TypeExpr::K::Name) parent = decl.Extends->Name;
            W("local " + C + " = __sage.class(" + Quote(C) + "," + parent + "," + Quote(m_path) + ");");
        }
        m_classStack.push_back(C);
        for (const Member& m : decl.Members) {
            At(m.P);
            if (m.Kind == Member::K::Field) {
                const bool literal = !m.Default || (m.Default->Kind == Expr::K::Nil || m.Default->Kind == Expr::K::True ||
                                                    m.Default->Kind == Expr::K::False ||
                                                    m.Default->Kind == Expr::K::Number ||
                                                    m.Default->Kind == Expr::K::String ||
                                                    (m.Default->Kind == Expr::K::Unary && m.Default->A &&
                                                     m.Default->A->Kind == Expr::K::Number));
                if (m.Static) {
                    W(C + "." + m.Name + " =");
                    Expr_(m.Default);
                    W(";");
                } else if (decl.Kind == ClassDecl::K::Struct) {
                    W("__sage.sfield(" + C + "," + Quote(m.Name) + ",");
                    if (literal) {
                        Expr_(m.Default);
                        W(",false);");
                    } else {
                        W("function(self) return");
                        Expr_(m.Default);
                        W("end,true);");
                    }
                } else if (literal) {
                    // Значение-литерал лежит в классе: экземпляр видит его, пока
                    // не запишет своё, — без копии в каждом объекте.
                    if (m.Default) {
                        W(C + "." + m.Name + " =");
                        Expr_(m.Default);
                        W(";");
                    }
                } else {
                    // Таблица, вектор, вызов — СВОЁ значение у каждого экземпляра:
                    // общий {} в классе был бы одним инвентарём на всех.
                    W("__sage.field(" + C + "," + Quote(m.Name) + ", function(self) return");
                    Expr_(m.Default);
                    W("end);");
                }
                continue;
            }
            if (m.Abstract) continue;
            const FuncBody& f = *m.Fn;
            W(std::string("function ") + C + (m.Static ? "." : ":") + m.Name);
            Params(f);
            FuncTail(f);
            W(";");
            for (const Annotation& a : m.Annotations)
                if (a.Name == "server" || a.Name == "client")
                    W(C + "." + m.Name + " = __sage.side(" + C + "." + m.Name + "," + Quote(a.Name) + "," +
                      Quote(C + "." + m.Name) + ");");
        }
        m_classStack.pop_back();
        At(decl.EndP);
        W("__sage.finish(" + C + "," + (info ? Meta(*info) : std::string("{}")) + ");");
        if (s.Exported && top) {
            W("__M." + C + " = " + C + ";");
            m_hasExports = true;
        }
    }
};

} // namespace

CodegenResult Generate(const BlockPtr& ast, const ProgramInfo& info, const std::string& path) {
    Gen g(info, path);
    return g.Run(ast);
}

} // namespace sage::lang
