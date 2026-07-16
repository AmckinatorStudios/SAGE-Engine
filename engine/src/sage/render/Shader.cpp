#include "Shader.h"
#include "sage/core/Log.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <glm/gtc/type_ptr.hpp>

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

unsigned int Shader::Compile(unsigned int type, const std::string& source) {
    unsigned int id = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    int success;
    glGetShaderiv(id, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetShaderInfoLog(id, 1024, nullptr, info);
        std::string kind = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        LOG_ERROR("Shader") << "Ошибка компиляции шейдера (" << kind << "): " << info;
        throw std::runtime_error("Ошибка компиляции шейдера (" + kind + "): " + info);
    }
    return id;
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vSrc = ReadFile(vertexPath);
    std::string fSrc = ReadFile(fragmentPath);

    unsigned int vShader = Compile(GL_VERTEX_SHADER, vSrc);
    unsigned int fShader = Compile(GL_FRAGMENT_SHADER, fSrc);

    m_id = glCreateProgram();
    glAttachShader(m_id, vShader);
    glAttachShader(m_id, fShader);
    glLinkProgram(m_id);

    int success;
    glGetProgramiv(m_id, GL_LINK_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetProgramInfoLog(m_id, 1024, nullptr, info);
        LOG_ERROR("Shader") << "Ошибка линковки шейдерной программы: " << info;
        throw std::runtime_error(std::string("Ошибка линковки шейдерной программы: ") + info);
    }

    glDeleteShader(vShader);
    glDeleteShader(fShader);

    LOG_DEBUG("Shader") << "Скомпилирован: " << vertexPath << " + " << fragmentPath;
}

Shader::~Shader() {
    glDeleteProgram(m_id);
}

void Shader::Use() const {
    glUseProgram(m_id);
}

void Shader::SetMat4(const std::string& name, const glm::mat4& value) const {
    glUniformMatrix4fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, glm::value_ptr(value));
}

void Shader::SetVec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(value));
}

void Shader::SetVec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(value));
}

void Shader::SetVec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(glGetUniformLocation(m_id, name.c_str()), 1, glm::value_ptr(value));
}

void Shader::SetFloat(const std::string& name, float value) const {
    glUniform1f(glGetUniformLocation(m_id, name.c_str()), value);
}

void Shader::SetInt(const std::string& name, int value) const {
    glUniform1i(glGetUniformLocation(m_id, name.c_str()), value);
}
