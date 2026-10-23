#include "sage/lang/SageCompiler.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "sage/assets/Pack.h"
#include "sage/lang/SageCodegen.h"
#include "sage/lang/SageLexer.h"
#include "sage/lang/SageParser.h"

namespace sage::lang {

CompileResult Compile(const std::string& path, const std::string& source, const CompileOptions& options) {
    CompileResult r;
    DiagnosticSink diag(path);
    std::vector<LexError> lexErrors;
    std::vector<Token> tokens = Lex(source, lexErrors);
    for (const LexError& e : lexErrors) diag.Error(e.P, "Синтаксическая ошибка", {e.Message});
    BlockPtr ast;
    if (lexErrors.empty()) ast = Parse(tokens, diag);
    r.Modules = std::make_shared<ModuleCache>();
    r.Modules->Loader = options.Loader;
    if (ast) {
        r.Modules->Loading.push_back(path);
        r.Program = Check(ast, path, diag, *r.Modules);
        r.Modules->Loading.pop_back();
    }
    r.Diagnostics = diag.Items();
    r.Ok = ast && !diag.HasErrors();
    if (r.Ok) {
        CodegenResult g = Generate(ast, r.Program, path);
        r.Lua = std::move(g.Lua);
        r.MainClass = g.MainClass;
    }
    return r;
}

namespace {
struct CacheEntry {
    size_t Hash = 0;
    std::shared_ptr<const CompileResult> Result;
};
std::mutex& CacheMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, CacheEntry>& Cache() {
    static std::unordered_map<std::string, CacheEntry> c;
    return c;
}
} // namespace

std::shared_ptr<const CompileResult> CompileCached(const std::string& path, const std::string& source,
                                                   const CompileOptions& options) {
    const size_t h = std::hash<std::string>{}(source);
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto it = Cache().find(path);
        // Файл с импортами кэшируется только по своему тексту: правка
        // импортируемого модуля сбрасывает кэш целиком (ClearCompileCache),
        // этим занимается тот, кто следит за файлами.
        if (it != Cache().end() && it->second.Hash == h) return it->second.Result;
    }
    auto result = std::make_shared<CompileResult>(Compile(path, source, options));
    std::lock_guard<std::mutex> lock(CacheMutex());
    Cache()[path] = CacheEntry{h, result};
    return result;
}

void ClearCompileCache() {
    std::lock_guard<std::mutex> lock(CacheMutex());
    Cache().clear();
}

namespace {

const ClassInfo* MainOf(const CompileResult& r) {
    if (r.MainClass.empty()) return nullptr;
    return r.Program.Find(r.MainClass);
}

double LiteralNumber(const ExprPtr& e, bool& ok) {
    ok = false;
    if (!e) return 0;
    if (e->Kind == Expr::K::Number) {
        ok = true;
        return std::strtod(e->Text.c_str(), nullptr);
    }
    if (e->Kind == Expr::K::Unary && e->Op == Tok::Minus && e->A && e->A->Kind == Expr::K::Number) {
        ok = true;
        return -std::strtod(e->A->Text.c_str(), nullptr);
    }
    return 0;
}

std::string LiteralString(const ExprPtr& e) {
    if (!e || e->Kind != Expr::K::String || e->Text.size() < 2) return {};
    std::string raw = e->Text;
    if (raw[0] == '"' || raw[0] == '\'') return raw.substr(1, raw.size() - 2);
    return {};
}

// Vector3(1, 2, 3) — литерал вектора по умолчанию.
bool LiteralVector(const ExprPtr& e, float* out, size_t n) {
    if (!e || e->Kind != Expr::K::Call || !e->A) return false;
    std::string callee;
    if (e->A->Kind == Expr::K::Name) callee = e->A->Text;
    else if (e->A->Kind == Expr::K::Field && e->A->A && e->A->A->Kind == Expr::K::Name)
        callee = e->A->A->Text;
    if (callee != "Vector3" && callee != "Vector2" && callee != "Color") return false;
    for (size_t k = 0; k < n; ++k) {
        bool ok = false;
        out[k] = k < e->Args.size() ? (float)LiteralNumber(e->Args[k], ok) : (callee == "Color" && k == 3 ? 1.0f : 0.0f);
        if (k < e->Args.size() && !ok) return false;
    }
    return true;
}

} // namespace

sage::vars::Table FieldsOf(const CompileResult& r) {
    sage::vars::Table table;
    const ClassInfo* main = MainOf(r);
    if (!main) return table;
    // Поля всей цепочки: от родителя к наследнику, как их видит человек.
    std::vector<const ClassInfo*> chain;
    for (const ClassInfo* c = main; c && c->Kind != ClassInfo::K::Builtin; c = c->Parent) chain.insert(chain.begin(), c);
    for (const ClassInfo* c : chain) {
        for (const FieldInfo& f : c->Fields) {
            if (!f.Meta.Editable || f.Static) continue;
            sage::vars::Var v;
            v.Name = f.Name;
            v.Label = f.Meta.Label;
            v.Tooltip = f.Meta.Tooltip;
            v.Declared = true;
            if (f.Meta.HasRange) {
                v.Min = (float)f.Meta.Min;
                v.Max = (float)f.Meta.Max;
            }
            TypeRef t = f.T;
            if (t && t->Kind == Type::K::Optional) t = t->Args[0];
            bool ok = false;
            const double num = LiteralNumber(f.Default, ok);
            switch (t ? t->Kind : Type::K::Any) {
                case Type::K::Integer: case Type::K::EnumValue:
                    v.Data = sage::vars::Value((int)num);
                    break;
                case Type::K::Number:
                    v.Data = sage::vars::Value((float)num);
                    break;
                case Type::K::Boolean:
                    v.Data = sage::vars::Value(f.Default && f.Default->Kind == Expr::K::True);
                    break;
                case Type::K::String:
                    v.Data = sage::vars::Value(LiteralString(f.Default));
                    break;
                case Type::K::Instance: {
                    const std::string& n = t->Class->Name;
                    float xyz[4] = {0, 0, 0, 1};
                    if (n == "Vector3") {
                        LiteralVector(f.Default, xyz, 3);
                        v.Data = sage::vars::Value(glm::vec3(xyz[0], xyz[1], xyz[2]));
                    } else if (n == "Vector2") {
                        LiteralVector(f.Default, xyz, 2);
                        v.Data = sage::vars::Value(glm::vec2(xyz[0], xyz[1]));
                    } else if (n == "Color") {
                        LiteralVector(f.Default, xyz, 4);
                        v.Data = sage::vars::Value(glm::vec4(xyz[0], xyz[1], xyz[2], xyz[3]));
                    } else if (n == "Texture" || n == "Prefab" || n == "AudioClip" || n == "Material") {
                        sage::vars::AssetRef a;
                        v.Data = sage::vars::Value(a);
                        v.Hint = n == "AudioClip" ? "Audio" : n;
                    } else {
                        v.Data = sage::vars::Value(sage::vars::EntityRef{});
                        // Ссылка на компонент: скрипт получает сразу компонент.
                        if (!t->Class->EngineName.empty() && n != "Transform") v.Hint = t->Class->EngineName;
                    }
                    break;
                }
                default:
                    if (ok) v.Data = sage::vars::Value((float)num);
                    else if (f.Default && f.Default->Kind == Expr::K::String) v.Data = sage::vars::Value(LiteralString(f.Default));
                    else if (f.Default && (f.Default->Kind == Expr::K::True || f.Default->Kind == Expr::K::False))
                        v.Data = sage::vars::Value(f.Default->Kind == Expr::K::True);
                    else continue;
                    break;
            }
            table.Put(v);
        }
    }
    return table;
}

std::vector<std::string> MethodsOf(const CompileResult& r) {
    std::vector<std::string> out;
    const ClassInfo* main = MainOf(r);
    for (const ClassInfo* c = main; c && c->Kind != ClassInfo::K::Builtin; c = c->Parent)
        for (const MethodInfo& m : c->Methods) {
            if (m.Access != "public" || m.Static || m.Name == "init") continue;
            if (m.Name.size() > 2 && m.Name[0] == 'o' && m.Name[1] == 'n' && std::isupper((unsigned char)m.Name[2]))
                continue;   // хуки жизненного цикла — не действия для связей
            bool dup = false;
            for (const std::string& s : out) dup = dup || s == m.Name;
            if (!dup) out.push_back(m.Name);
        }
    return out;
}

std::string CompileResult::DescribeClasses() const {
    std::string out;
    for (const auto& c : Program.Classes) {
        out += c->Kind == ClassInfo::K::Interface ? "interface " : c->Kind == ClassInfo::K::Struct ? "struct " : "class ";
        out += c->Name;
        if (c->Parent && c->Parent->Name != "Object") out += " extends " + c->Parent->Name;
        if (c->IsComponent()) out += " [component]";
        if (c->Serializable) out += " [serializable]";
        out += "\n";
        for (const FieldInfo& f : c->Fields) {
            out += "  field " + f.Access + (f.Static ? " static" : "") + " " + f.Name + ": " + TypeName(f.T);
            if (f.Meta.Editable) out += " editable";
            if (!f.Meta.Label.empty()) out += " name=\"" + f.Meta.Label + "\"";
            if (f.Meta.HasRange) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), " range=[%g, %g, %g]", f.Meta.Min, f.Meta.Max, f.Meta.Step);
                out += buf;
            }
            if (f.Meta.Replicated) out += " replicated";
            out += "\n";
        }
        for (const MethodInfo& m : c->Methods) {
            out += "  method " + m.Access + (m.Static ? " static" : "") + (m.Override ? " override" : "") + " " + m.Name;
            if (!m.Deprecated.empty()) out += " deprecated";
            if (!m.Side.empty()) out += " @" + m.Side;
            out += "\n";
        }
    }
    return out;
}

bool ResolveModulePath(const std::string& spec, const std::string& fromFile,
                       const std::vector<std::string>& searchDirs, std::string& resolved) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> bases;
    // Папка файла и все её предки: модуль ищется от корня проекта, где бы ни
    // лежал импортирующий файл (Scripts/Player/X.slua импортирует "Game/Y").
    fs::path dir = fs::path(fromFile).parent_path();
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        bases.push_back(dir);
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    for (const std::string& d : searchDirs) bases.push_back(fs::path(d));
    bases.push_back(fs::path("."));
    // "Game/Player", "Game.Player", "./Player", "Player.slua"
    std::string rel = spec;
    if (rel.find('/') == std::string::npos && rel.find('\\') == std::string::npos &&
        rel.find(".slua") == std::string::npos && rel.find(".lua") == std::string::npos)
        for (char& c : rel)
            if (c == '.') c = '/';
    const bool hasExt = (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".slua") == 0) ||
                        (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".lua") == 0);
    std::vector<std::string> names;
    if (hasExt) names.push_back(rel);
    else names = {rel + ".slua", rel + ".lua", rel + "/init.slua", rel + "/init.lua"};
    // Корни проекта, в которых по соглашению лежат скрипты.
    static const char* kRoots[] = {"", "Scripts", "scripts", "assets/scripts", "assets/Scripts", "Modules",
                                   "assets", "Assets/Scripts"};
    for (const fs::path& base : bases) {
        for (const char* root : kRoots) {
            for (const std::string& n : names) {
                fs::path candidate = (std::string(root).empty() ? base : base / root) / n;
                // vfs, а не только диск: в собранной игре модуль лежит в пакете.
                if (fs::is_regular_file(candidate, ec) ||
                    sage::assets::vfs::Exists(candidate.lexically_normal().generic_string())) {
                    resolved = candidate.lexically_normal().generic_string();
                    return true;
                }
            }
        }
    }
    return false;
}

namespace {
std::vector<std::string>& Roots() {
    static std::vector<std::string> r;
    return r;
}
} // namespace

void SetModuleRoots(const std::vector<std::string>& roots) {
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        Roots() = roots;
    }
    // Другой проект — другие модули под теми же именами.
    ClearCompileCache();
}

ModuleLoader ProjectModuleLoader() {
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        roots = Roots();
    }
    return [roots](const std::string& spec, const std::string& from, std::string& resolved, std::string& text) {
        if (!ResolveModulePath(spec, from, roots, resolved)) return false;
        return sage::assets::vfs::ReadText(resolved, text);
    };
}

std::shared_ptr<const CompileResult> CompileForEngine(const std::string& path, const std::string& source) {
    CompileOptions options;
    options.Loader = ProjectModuleLoader();
    return CompileCached(path, source, options);
}

} // namespace sage::lang
