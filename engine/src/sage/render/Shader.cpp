#include "Shader.h"
#include "PbrShader.h"
#include <cstring>
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/assets/Pack.h"
#include "sage/rhi/GraphicsDevice.h"
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

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vSrc = ExpandEngineIncludes(ReadFile(vertexPath));
    std::string fSrc = ExpandEngineIncludes(ReadFile(fragmentPath));

    // Компиляция/линковка — забота бэкенда; при ошибке он бросает исключение
    // с логом компилятора (текст видно и в консоли, и в панели Console редактора).
    m_program = sage::rhi::GraphicsDevice::Get().CreateShaderProgram(vSrc, fSrc);

    LOG_DEBUG("Shader") << "Скомпилирован: " << vertexPath << " + " << fragmentPath;
}

Shader::Shader(FromSourceTag, const std::string& vertexSrc, const std::string& fragmentSrc,
               const std::string& tag) {
    m_program = sage::rhi::GraphicsDevice::Get().CreateShaderProgram(vertexSrc, fragmentSrc);
    LOG_DEBUG("Shader") << "Скомпилирован из исходников: " << tag;
}

Shader Shader::FromSource(const std::string& vertexSrc, const std::string& fragmentSrc,
                          const std::string& tag) {
    return Shader(FromSourceTag{}, vertexSrc, fragmentSrc, tag);
}
