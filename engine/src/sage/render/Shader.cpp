#include "Shader.h"
#include "PbrShader.h"
#include <cstring>
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/assets/Pack.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/render/ShaderParams.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

std::string Shader::ReadFile(const std::string& path) {
    // Сперва — через vfs: в СОБРАННОЙ игре проект лежит одним файлом
    // game.sagepak, и шейдера игры на диске нет вовсе. Пока этой ветки не было,
    // собственный шейдер материала (та самая вода с волнами) не загружался
    // ровно в собранной игре: в редакторе и при запуске из папки проекта файл
    // читался с диска и всё работало, а игрок получал ошибку в логе и материал
    // без шейдера. vfs без смонтированного пакета читает тот же диск, поэтому
    // для всех остальных случаев ничего не меняется.
    {
        std::string packed;
        if (sage::assets::vfs::ReadText(path, packed)) return packed;
    }
    std::ifstream file(path);
    // Не нашлось от текущей папки — ищем рядом с бинарником. Шейдеры движка
    // поставляются вместе с программой, а текущая папка к моменту загрузки
    // может быть какой угодно: SAGE Player, например, СПЕЦИАЛЬНО уходит в
    // папку проекта, чтобы игровые пути были относительны ему. До этой строки
    // плеер, запущенный не из своей папки, падал с фатальной ошибкой на
    // собственном lit.vert, лежащем ровно там, где ему и положено.
    if (!file.is_open()) {
        const std::string nearExe = sage::EngineAssetPath(path);
        if (nearExe != path) file.open(nearExe);
    }
    if (!file.is_open()) {
        LOG_ERROR("Shader") << "Не удалось открыть файл шейдера: " << path;
        throw std::runtime_error("Не удалось открыть файл шейдера: " + path);
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

namespace {
// Подстановка встроенных блоков движка в шейдер игры.
//
// Освещение движка живёт в C++-строке (render/PbrShader.h), и до сих пор
// воспользоваться им мог только сам движок: игре, пишущей свой шейдер,
// оставалось либо копировать сотню строк BRDF к себе, либо освещать объект
// как умеет. Копия немедленно разошлась бы с оригиналом — а «свой шейдер»
// нужен ради своей ГЕОМЕТРИИ и своего цвета, а не ради своего PBR.
//
// Директива намеренно не похожа на обычный #include с путём: подставляется не
// файл, а ВСТРОЕННЫЙ блок движка, и версия его меняется вместе с движком.
std::string ExpandEngineIncludes(std::string src) {
    struct Block { const char* Directive; const char* Text; };
    const Block kBlocks[] = {
        {"#include <sage_pbr>", sage::render::kPbrSharedGlsl},
    };
    for (const Block& b : kBlocks) {
        for (size_t pos = src.find(b.Directive); pos != std::string::npos;
             pos = src.find(b.Directive, pos)) {
            src.replace(pos, std::strlen(b.Directive), b.Text);
            pos += std::strlen(b.Text);
        }
    }
    return src;
}
} // namespace

namespace {
// Умолчания из подсказок обоих исходников; первое объявление главное.
void CollectDefaults(Shader& sh, const std::string& v, const std::string& f) {
    sh.WantsScene = f.find("uSceneColor") != std::string::npos || f.find("uSceneDepth") != std::string::npos;
    for (const std::string* src : {&v, &f})
        for (const sage::render::ShaderUniformInfo& u : sage::render::ParseShaderUniforms(*src)) {
            if (u.Texture) {
                bool dup = false;
                for (const Shader::TextureSlot& t : sh.Textures) dup = dup || t.Name == u.Name;
                if (!dup) sh.Textures.push_back({u.Name, u.Linear});
            } else {
                sh.Defaults.emplace(u.Name, ShaderParam{u.Type, u.Default});
            }
        }
}

std::string ParentDir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string Expand(const std::string& path, std::vector<std::string>& seen, int depth) {
    std::string src = Shader::LoadText(path);
    if (depth > 32) return src;   // цикл включений — дальше не идём, компилятор скажет сам
    std::string out;
    out.reserve(src.size());
    size_t start = 0;
    int lineNo = 0;
    while (start <= src.size()) {
        size_t end = src.find('\n', start);
        if (end == std::string::npos) end = src.size();
        const std::string line = src.substr(start, end - start);
        ++lineNo;
        size_t p = line.find_first_not_of(" \t");
        const bool isInclude = p != std::string::npos && line.compare(p, 8, "#include") == 0 &&
                               line.find('"', p) != std::string::npos;
        if (isInclude) {
            const size_t q1 = line.find('"', p);
            const size_t q2 = line.find('"', q1 + 1);
            const std::string name = q2 == std::string::npos ? std::string() : line.substr(q1 + 1, q2 - q1 - 1);
            const std::string full = ParentDir(path) + name;
            bool already = false;
            for (const std::string& s : seen) already = already || s == full;
            if (!name.empty() && !already) {
                seen.push_back(full);
                out += Expand(full, seen, depth + 1);
            }
            // Номера строк ошибок — снова по этому файлу.
            out += "\n#line " + std::to_string(lineNo + 1) + "\n";
        } else {
            out += line;
            if (end < src.size()) out += '\n';
        }
        if (end >= src.size()) break;
        start = end + 1;
    }
    return out;
}
} // namespace

std::string Shader::LoadExpanded(const std::string& path, std::vector<std::string>* included) {
    std::vector<std::string> seen;
    std::string text = Expand(path, seen, 0);
    if (included) *included = std::move(seen);
    return text;
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) {
    std::vector<std::string> vInc, fInc;
    std::string vSrc = ExpandEngineIncludes(LoadExpanded(vertexPath, &vInc));
    std::string fSrc = ExpandEngineIncludes(LoadExpanded(fragmentPath, &fInc));

    // Компиляция/линковка — забота бэкенда; при ошибке он бросает исключение
    // с логом компилятора (текст видно и в консоли, и в панели Console редактора).
    m_program = sage::rhi::GraphicsDevice::Get().CreateShaderProgram(vSrc, fSrc);
    CollectDefaults(*this, vSrc, fSrc);
    Included = std::move(vInc);
    Included.insert(Included.end(), fInc.begin(), fInc.end());

    LOG_DEBUG("Shader") << "Скомпилирован: " << vertexPath << " + " << fragmentPath;
}

Shader::Shader(FromSourceTag, const std::string& vertexSrc, const std::string& fragmentSrc,
               const std::string& tag) {
    m_program = sage::rhi::GraphicsDevice::Get().CreateShaderProgram(vertexSrc, fragmentSrc);
    CollectDefaults(*this, vertexSrc, fragmentSrc);
    LOG_DEBUG("Shader") << "Скомпилирован из исходников: " << tag;
}

Shader Shader::FromSource(const std::string& vertexSrc, const std::string& fragmentSrc,
                          const std::string& tag) {
    return Shader(FromSourceTag{}, vertexSrc, fragmentSrc, tag);
}
