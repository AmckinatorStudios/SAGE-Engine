#include "sage/render/ShaderLibrary.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/render/PbrShader.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {
namespace {

// Общие функции своих шейдеров. Короткие и без униформ: включённый файл не
// должен навязывать программе ничего, кроме функций.
const char* kCommonGlsl = R"GLSL(
// sage/common.glsl — общие функции своих шейдеров SAGE.
// Полноэкранный треугольник без вершинного буфера: DrawArrays(3) и в
// вершинном шейдере sage_fullscreen_pos(gl_VertexID).
vec4 sage_fullscreen_pos(int id) {
    vec2 p = vec2((id << 1) & 2, id & 2);
    return vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
vec2 sage_fullscreen_uv(int id) { return vec2((id << 1) & 2, id & 2); }
vec3 sage_srgb_to_linear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 sage_linear_to_srgb(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 sage_aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
float sage_hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
)GLSL";

struct IncludeRegistry {
    std::mutex Lock;
    std::unordered_map<std::string, std::string> Texts;
};

IncludeRegistry& Includes() {
    static IncludeRegistry* r = [] {
        auto* reg = new IncludeRegistry();
        reg->Texts["sage/common.glsl"] = kCommonGlsl;
        reg->Texts["sage/pbr.glsl"] = kPbrSharedGlsl;
        return reg;
    }();
    return *r;
}

bool ReadText(const std::string& path, std::string& out) {
    // Сначала пакет игры (в собранной игре шейдера на диске нет), затем диск,
    // затем папка программы — тот же порядок, что у Shader::ReadFile.
    if (sage::assets::vfs::ReadText(path, out)) return true;
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f.is_open()) {
        const std::string nearExe = sage::EngineAssetPath(path);
        if (nearExe != path) f.open(sage::PathFromUtf8(nearExe), std::ios::binary);
    }
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

long long Stamp(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(sage::PathFromUtf8(path), ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}

std::string DirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Разбор строки «#include "x"» / «#include <x>». false — это не #include.
bool ParseInclude(const std::string& line, std::string& name, bool& angled) {
    std::string t = Trim(line);
    if (t.rfind("#", 0) != 0) return false;
    t = Trim(t.substr(1));
    if (t.rfind("include", 0) != 0) return false;
    t = Trim(t.substr(7));
    if (t.size() < 2) return false;
    const char open = t[0];
    const char close = open == '<' ? '>' : '"';
    if (open != '<' && open != '"') return false;
    const size_t end = t.find(close, 1);
    if (end == std::string::npos) return false;
    name = t.substr(1, end - 1);
    angled = open == '<';
    return true;
}

struct Expander {
    const ShaderIncludeResolver& Resolver;
    PreprocessedShader& Out;
    std::set<std::string> Seen;

    bool Expand(const std::string& code, int fileIndex, int depth) {
        if (depth > 16) {
            Out.Error = Out.Files[(size_t)fileIndex] + ": слишком глубокая вложенность #include";
            return false;
        }
        std::istringstream in(code);
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string name;
            bool angled = false;
            if (!ParseInclude(line, name, angled)) {
                Out.Code += line;
                Out.Code += '\n';
                continue;
            }
            // <sage_pbr> — прежнее имя встроенного PBR (render/Shader.cpp).
            if (name == "sage_pbr") name = "sage/pbr.glsl";
            std::string text;
            std::string key = name;
            bool found = false;
            if (const std::string* builtin = FindShaderInclude(name)) {
                text = *builtin;
                found = true;
            } else if (!angled && Resolver) {
                found = Resolver(name, text);
            }
            if (!found) {
                Out.Error = Out.Files[(size_t)fileIndex] + ":" + std::to_string(lineNo) +
                            ": не найден #include \"" + name + "\"";
                return false;
            }
            if (!Seen.insert(key).second) {
                Out.Code += "// (уже включён: " + name + ")\n";
                continue;
            }
            const int child = (int)Out.Files.size();
            Out.Files.push_back(name);
            Out.Code += "#line 1 " + std::to_string(child) + "\n";
            if (!Expand(text, child, depth + 1)) return false;
            Out.Code += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(fileIndex) + "\n";
        }
        return true;
    }
};

// Одна строка ответа драйвера -> (номер исходника, строка, важность, текст).
bool ParseLogLine(const std::string& raw, int& source, int& line, bool& error, std::string& msg) {
    const std::string s = Trim(raw);
    auto readInt = [&](size_t& i, int& v) {
        if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
        v = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
        return true;
    };
    // «error: текст» / «warning: текст» / «error C1008: текст» — важность
    // из первого слова, а само слово из текста убирается: его и так скажет
    // ShaderDiagnostic::ToString.
    auto severity = [&](std::string& rest) {
        std::string low = rest;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        error = low.rfind("warning", 0) != 0;
        for (const char* word : {"error", "warning"}) {
            if (low.rfind(word, 0) == 0) {
                size_t colon = rest.find(':');
                if (colon != std::string::npos && colon < 24) rest = Trim(rest.substr(colon + 1));
                break;
            }
        }
    };
    // AMD/Intel: «ERROR: 0:12: текст» / «WARNING: 0:12: текст»
    for (const char* tag : {"ERROR:", "WARNING:"}) {
        if (s.rfind(tag, 0) == 0) {
            size_t i = std::string(tag).size();
            while (i < s.size() && s[i] == ' ') ++i;
            if (!readInt(i, source) || i >= s.size() || s[i] != ':') return false;
            ++i;
            if (!readInt(i, line)) return false;
            if (i < s.size() && s[i] == ':') ++i;
            msg = Trim(s.substr(i));
            error = tag[0] == 'E';
            return true;
        }
    }
    size_t i = 0;
    if (!readInt(i, source) || i >= s.size()) return false;
    if (s[i] == ':') {
        // Mesa: «0:12(5): error: текст»
        ++i;
        if (!readInt(i, line)) return false;
        if (i < s.size() && s[i] == '(') {
            const size_t close = s.find(')', i);
            if (close == std::string::npos) return false;
            i = close + 1;
        }
    } else if (s[i] == '(') {
        // NVIDIA: «0(12) : error C1008: текст»
        ++i;
        if (!readInt(i, line) || i >= s.size() || s[i] != ')') return false;
        ++i;
    } else {
        return false;
    }
    while (i < s.size() && (s[i] == ' ' || s[i] == ':')) ++i;
    msg = Trim(s.substr(i));
    severity(msg);
    return true;
}

} // namespace

std::string ShaderDiagnostic::ToString() const {
    std::string out = File.empty() ? std::string(sage::rhi::ShaderStageName(Stage)) : File;
    if (Line > 0) out += ":" + std::to_string(Line);
    out += Link ? ": link: " : (Error ? ": error: " : ": warning: ");
    out += Message;
    return out;
}

void RegisterShaderInclude(const std::string& name, std::string text) {
    IncludeRegistry& r = Includes();
    std::lock_guard<std::mutex> lock(r.Lock);
    r.Texts[name] = std::move(text);
}

const std::string* FindShaderInclude(const std::string& name) {
    IncludeRegistry& r = Includes();
    std::lock_guard<std::mutex> lock(r.Lock);
    auto it = r.Texts.find(name);
    return it == r.Texts.end() ? nullptr : &it->second;
}

PreprocessedShader PreprocessShader(const std::string& code, const std::string& origin,
                                    const ShaderIncludeResolver& resolver) {
    PreprocessedShader out;
    out.Files.push_back(origin);
    Expander ex{resolver, out, {}};
    out.Ok = ex.Expand(code, 0, 0);
    if (!out.Ok) out.Code.clear();
    return out;
}

std::vector<ShaderDiagnostic> ParseShaderLog(const std::string& text, sage::rhi::ShaderStage stage,
                                             bool link, const std::vector<std::string>& files) {
    std::vector<ShaderDiagnostic> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (Trim(line).empty()) continue;
        ShaderDiagnostic d;
        d.Stage = stage;
        d.Link = link;
        int source = 0, ln = 0;
        bool error = true;
        std::string msg;
        if (!link && ParseLogLine(line, source, ln, error, msg)) {
            d.Line = ln;
            d.Error = error;
            d.Message = msg;
            if (source >= 0 && (size_t)source < files.size()) d.File = files[(size_t)source];
        } else {
            d.Message = Trim(line);
            if (!files.empty()) d.File = files[0];
            // Незнакомая строка без слова warning — считаем ошибкой: лучше
            // показать лишнее, чем промолчать о настоящей причине.
            std::string low = d.Message;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            d.Error = low.find("warning") == std::string::npos;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// ============================================================================
//  ShaderLibrary
// ============================================================================

ShaderLibrary::ShaderLibrary(sage::rhi::GraphicsDevice* device) : m_device(device) {}
ShaderLibrary::~ShaderLibrary() = default;

sage::rhi::ShaderProgram* ShaderLibrary::Load(const std::string& name, const std::string& vertexPath,
                                              const std::string& fragmentPath) {
    return LoadStages(name, {{sage::rhi::ShaderStage::Vertex, vertexPath, {}},
                             {sage::rhi::ShaderStage::Fragment, fragmentPath, {}}});
}

sage::rhi::ShaderProgram* ShaderLibrary::LoadCompute(const std::string& name, const std::string& path) {
    return LoadStages(name, {{sage::rhi::ShaderStage::Compute, path, {}}});
}

sage::rhi::ShaderProgram* ShaderLibrary::LoadStages(const std::string& name,
                                                    std::vector<ShaderStageFile> stages) {
    auto it = m_entries.find(name);
    if (it == m_entries.end()) {
        auto entry = std::make_unique<ShaderEntry>();
        entry->Name = name;
        it = m_entries.emplace(name, std::move(entry)).first;
        m_order.push_back(name);
    }
    ShaderEntry& e = *it->second;
    e.Stages = std::move(stages);
    Build(e);
    return e.Program.get();
}

bool ShaderLibrary::Build(ShaderEntry& e) {
    sage::rhi::GraphicsDevice& device = m_device ? *m_device : sage::rhi::GraphicsDevice::Get();
    e.Diagnostics.clear();
    e.Watched.clear();

    sage::rhi::ShaderProgramDesc desc;
    desc.Name = e.Name;
    std::vector<std::vector<std::string>> filesPerStage;
    bool ok = true;
    for (const ShaderStageFile& st : e.Stages) {
        std::string code = st.Source;
        const std::string origin = st.Path.empty() ? e.Name + "." + sage::rhi::ShaderStageName(st.Stage)
                                                   : st.Path;
        if (!st.Path.empty()) {
            e.Watched.emplace_back(st.Path, Stamp(st.Path));
            if (!ReadText(st.Path, code)) {
                ShaderDiagnostic d;
                d.Stage = st.Stage;
                d.File = st.Path;
                d.Message = "файл шейдера не найден";
                e.Diagnostics.push_back(d);
                ok = false;
                continue;
            }
        }
        const std::string baseDir = DirOf(st.Path);
        ShaderIncludeResolver resolver = [&](const std::string& inc, std::string& out) {
            std::vector<std::string> candidates;
            if (!baseDir.empty()) candidates.push_back(baseDir + inc);
            for (const std::string& dir : m_includeDirs)
                candidates.push_back(dir.empty() || dir.back() == '/' ? dir + inc : dir + "/" + inc);
            candidates.push_back(inc);
            for (const std::string& c : candidates) {
                if (ReadText(c, out)) {
                    e.Watched.emplace_back(c, Stamp(c));
                    return true;
                }
            }
            return false;
        };
        PreprocessedShader pre = PreprocessShader(code, origin, resolver);
        if (!pre.Ok) {
            ShaderDiagnostic d;
            d.Stage = st.Stage;
            d.File = origin;
            d.Message = pre.Error;
            e.Diagnostics.push_back(d);
            ok = false;
            continue;
        }
        desc.Stages.push_back({st.Stage, std::move(pre.Code)});
        filesPerStage.push_back(std::move(pre.Files));
    }

    if (ok) {
        sage::rhi::ShaderBuildLog log;
        std::unique_ptr<sage::rhi::ShaderProgram> program = device.CreateProgram(desc, log);
        for (const sage::rhi::ShaderBuildLog::Entry& entry : log.Entries) {
            // Номер стадии в desc — чтобы взять ЕЁ список файлов.
            size_t idx = 0;
            for (size_t i = 0; i < desc.Stages.size(); ++i)
                if (desc.Stages[i].Stage == entry.Stage) idx = i;
            const std::vector<std::string> none;
            const auto& files = idx < filesPerStage.size() ? filesPerStage[idx] : none;
            auto parsed = ParseShaderLog(entry.Text, entry.Stage, entry.Link, files);
            e.Diagnostics.insert(e.Diagnostics.end(), parsed.begin(), parsed.end());
        }
        ok = log.Ok && program;
        if (ok) {
            e.Program = std::move(program);
            ++e.Version;
        }
    }

    e.LastBuildOk = ok;
    for (const ShaderDiagnostic& d : e.Diagnostics) {
        if (d.Error) LOG_ERROR("Shader") << e.Name << ": " << d.ToString();
        else LOG_WARN("Shader") << e.Name << ": " << d.ToString();
    }
    if (!ok && e.Program) {
        LOG_WARN("Shader") << e.Name << ": сборка не удалась — рисуется прежняя версия";
    }
    return ok;
}

sage::rhi::ShaderProgram* ShaderLibrary::Get(const std::string& name) const {
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : it->second->Program.get();
}

const ShaderEntry* ShaderLibrary::Find(const std::string& name) const {
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : it->second.get();
}

std::vector<const ShaderEntry*> ShaderLibrary::All() const {
    std::vector<const ShaderEntry*> out;
    for (const std::string& n : m_order) out.push_back(m_entries.at(n).get());
    return out;
}

int ShaderLibrary::ReloadChanged() {
    int rebuilt = 0;
    for (const std::string& n : m_order) {
        ShaderEntry& e = *m_entries.at(n);
        bool changed = false;
        for (const auto& [path, stamp] : e.Watched)
            if (Stamp(path) != stamp) changed = true;
        if (changed && Build(e)) {
            ++rebuilt;
            LOG_INFO("Shader") << e.Name << ": пересобран (версия " << e.Version << ")";
        }
    }
    return rebuilt;
}

bool ShaderLibrary::Rebuild(const std::string& name) {
    auto it = m_entries.find(name);
    return it != m_entries.end() && Build(*it->second);
}

int ShaderLibrary::ErrorCount() const {
    int n = 0;
    for (const auto& [name, e] : m_entries)
        for (const ShaderDiagnostic& d : e->Diagnostics)
            if (d.Error) ++n;
    return n;
}

} // namespace sage::render
