#include "Shader.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

std::string Shader::ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Shader") << "Не удалось открыть файл шейдера: " << path;
        throw std::runtime_error("Не удалось открыть файл шейдера: " + path);
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vSrc = ReadFile(vertexPath);
    std::string fSrc = ReadFile(fragmentPath);

    // Компиляция/линковка — забота бэкенда; при ошибке он бросает исключение
    // с логом компилятора (текст видно и в консоли, и в панели Console редактора).
    m_program = sage::rhi::GraphicsDevice::Get().CreateShaderProgram(vSrc, fSrc);

    LOG_DEBUG("Shader") << "Скомпилирован: " << vertexPath << " + " << fragmentPath;
}
