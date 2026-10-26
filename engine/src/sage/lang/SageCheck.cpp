#include "sage/lang/SageCheck.h"
#include "sage/lang/SageCheckDetail.h"

namespace sage::lang {

using namespace check;

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

ProgramInfo Check(const BlockPtr& ast, const std::string& path, DiagnosticSink& diag, ModuleCache& modules,
                  bool lint) {
    Checker c(path, diag, modules, false);
    c.SetLint(lint);
    return c.Run(ast, nullptr);
}

} // namespace sage::lang
