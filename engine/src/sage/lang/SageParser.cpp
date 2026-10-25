#include "sage/lang/SageParser.h"
#include "sage/lang/SageParserDetail.h"

#include <stdexcept>

namespace sage::lang {

using namespace parse;

BlockPtr Parse(const std::vector<Token>& tokens, DiagnosticSink& diag) {
    Parser p(tokens, diag);
    try {
        return p.Chunk();
    } catch (const ParseAbort&) {
        return nullptr;
    } catch (const SpecFail&) {
        diag.Error(Pos{}, "Синтаксическая ошибка", {"внутренняя ошибка разбора"});
        return nullptr;
    }
}

std::string TypeText(const TypePtr& t) {
    if (!t) return "any";
    switch (t->Kind) {
        case TypeExpr::K::Name: {
            std::string s = t->Name;
            if (!t->Args.empty()) {
                s += "<";
                for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
                s += ">";
            }
            return s;
        }
        case TypeExpr::K::Optional: return TypeText(t->Args[0]) + "?";
        case TypeExpr::K::Array: return TypeText(t->Args[0]) + "[]";
        case TypeExpr::K::Union: {
            std::string s;
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? " | " : "") + TypeText(t->Args[k]);
            return s;
        }
        case TypeExpr::K::Tuple: {
            std::string s = "(";
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
            return s + ")";
        }
        case TypeExpr::K::Function: {
            std::string s = "function(";
            for (size_t k = 0; k < t->Args.size(); ++k) s += (k ? ", " : "") + TypeText(t->Args[k]);
            s += ")";
            if (t->Ret) s += ": " + TypeText(t->Ret);
            return s;
        }
        case TypeExpr::K::Table: {
            std::string s = "{ ";
            for (size_t k = 0; k < t->Fields.size(); ++k)
                s += (k ? ", " : "") + t->Fields[k].first + ": " + TypeText(t->Fields[k].second);
            return s + " }";
        }
    }
    return "any";
}

} // namespace sage::lang
