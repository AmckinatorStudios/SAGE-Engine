#include "sage/lang/SageCheckDetail.h"

// Функции API движка в проверке типов и строгий режим (опечатки в именах).
//
// Типы из таблицы переводятся осторожно: проверка, которая ругается на
// рабочий код, хуже молчащей — её перестают читать. Поэтому строго держится
// только то, в чём ошибиться нельзя: число там, где ждут число, и число
// аргументов. string принимает и число (Lua склеивает их без спроса),
// boolean — что угодно (в Lua истинно всё, кроме nil и false), а таблицы,
// векторы и сущности не проверяются: их тип скрипту на Lua не виден.
namespace sage::lang::check {
namespace {

TypeRef ParamType(const std::string& t) {
    if (t == "number" || t == "integer") return NumT();
    if (t == "string") return Make(Type::K::Union, {StrT(), NumT()});
    return Any();
}

TypeRef RetType(const std::string& t) {
    if (t == "number" || t == "integer") return NumT();
    if (t == "string") return StrT();
    if (t == "boolean") return BoolT();
    return nullptr;   // nil в таблице — «возврат не объявлен», а не «ничего»
}

std::vector<std::string> SplitParams(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t k = 0; k <= s.size(); ++k) {
        const char c = k < s.size() ? s[k] : ',';
        if (c == '(' || c == '<' || c == '{') ++depth;
        else if (c == ')' || c == '>' || c == '}') --depth;
        else if (c == ',' && depth == 0) {
            if (k > start) out.push_back(s.substr(start, k - start));
            start = k + 1;
        }
    }
    return out;
}

std::shared_ptr<FuncSig> SigOfApi(const ApiFunction& f) {
    auto sig = std::make_shared<FuncSig>();
    for (const std::string& p : SplitParams(f.Params)) {
        if (p == "...") {
            sig->Vararg = true;
            sig->VarargType = Any();
            continue;
        }
        const size_t colon = p.find(':');
        ParamInfo info;
        info.Name = p.substr(0, colon);
        info.HasDefault = !info.Name.empty() && info.Name.back() == '?';
        if (info.HasDefault) info.Name.pop_back();
        info.T = ParamType(colon == std::string::npos ? "any" : p.substr(colon + 1));
        sig->Params.push_back(std::move(info));
    }
    sig->Ret = RetType(f.Ret);
    return sig;
}

void Collect(const BlockPtr& b, std::set<std::string>& out);

void CollectStmt(const StmtPtr& s, std::set<std::string>& out) {
    if (!s) return;
    switch (s->Kind) {
        case Stmt::K::Function:
            if (s->FuncName.size() == 1 && s->Method.empty()) out.insert(s->FuncName[0]);
            break;
        case Stmt::K::Assign:
            for (const ExprPtr& t : s->Targets)
                if (t && t->Kind == Expr::K::Name) out.insert(t->Text);
            break;
        default: break;
    }
    Collect(s->Body, out);
    Collect(s->Else, out);
    for (const auto& [cond, block] : s->Clauses) Collect(block, out);
    if (s->Fn) Collect(s->Fn->Body, out);
}

void Collect(const BlockPtr& b, std::set<std::string>& out) {
    if (!b) return;
    for (const StmtPtr& s : b->Stmts) CollectStmt(s, out);
}

} // namespace

void Checker::CollectScriptGlobals(const BlockPtr& b) { Collect(b, m_scriptGlobals); }

bool Checker::GlobalPath(const ExprPtr& e, std::string& out) {
    if (!e) return false;
    if (e->Kind == Expr::K::Name) {
        if (Lookup(e->Text) || m_scriptGlobals.count(e->Text)) return false;
        out = e->Text;
        return true;
    }
    if (e->Kind != Expr::K::Field || !GlobalPath(e->A, out)) return false;
    out += "." + e->Text;
    return true;
}

TypeRef Checker::ApiType(const std::string& dotted) {
    // Имя, которое скрипт задал сам, — его, а не движка, даже если совпало.
    if (dotted.find('.') == std::string::npos && m_scriptGlobals.count(dotted)) return nullptr;
    auto cached = m_apiTypes.find(dotted);
    if (cached != m_apiTypes.end()) return cached->second;
    const ApiFunction* f = FindApiFunction(dotted);
    TypeRef t = f ? FuncOf(SigOfApi(*f)) : nullptr;
    m_apiTypes.emplace(dotted, t);
    return t;
}

void Checker::UnknownGlobal(const ExprPtr& e) {
    if (!m_lint || m_scriptGlobals.count(e->Text) || IsKnownGlobal(e->Text) || CoreNames().count(e->Text)) return;
    if (!m_warnedNames.insert(e->Text).second) return;
    // Кандидаты: что видно в этом месте (локальные, поля модуля), глобальные
    // скрипта и API движка — опечатка чаще всего в своём же имени.
    std::vector<std::string> candidates(KnownGlobals());
    for (const auto& scope : m_scopes)
        for (const auto& [name, sym] : scope) candidates.push_back(name);
    candidates.insert(candidates.end(), m_scriptGlobals.begin(), m_scriptGlobals.end());
    const std::string near = ClosestName(e->Text, candidates);
    std::vector<std::string> lines = {"'" + e->Text + "' нигде не объявлено: ни в скрипте, ни в API движка"};
    if (!near.empty()) lines.push_back("возможно, имелось в виду " + near);
    else lines.push_back("если это глобальная из другого скрипта — объявите её здесь или передайте явно");
    d.Warning(e->P, "Неизвестное имя", lines);
}

} // namespace sage::lang::check
