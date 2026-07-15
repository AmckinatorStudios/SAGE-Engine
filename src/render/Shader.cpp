#include "Shader.h"
#include "../core/Log.h"
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

// Компилирует один шейдерный объект. Возвращает 0 при ошибке (не бросает),
// причину кладёт в *error — чтобы вызывающий сам решил, фатально это
// (конструктор) или нет (hot-reload оставляет старую программу).
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
        glDeleteShader(id);
        return 0;
    }
    return id;
}

unsigned int Shader::BuildProgram(const std::string& vSrc, const std::string& fSrc, std::string* error) {
    unsigned int vShader = Compile(GL_VERTEX_SHADER, vSrc);
    unsigned int fShader = Compile(GL_FRAGMENT_SHADER, fSrc);
    if (!vShader || !fShader) {
        if (vShader) glDeleteShader(vShader);
        if (fShader) glDeleteShader(fShader);
        if (error) *error = "не удалось скомпилировать шейдер";
        return 0;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vShader);
    glAttachShader(program, fShader);
    glLinkProgram(program);
    glDeleteShader(vShader);
    glDeleteShader(fShader);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info[1024];
        glGetProgramInfoLog(program, 1024, nullptr, info);
        LOG_ERROR("Shader") << "Ошибка линковки шейдерной программы: " << info;
        glDeleteProgram(program);
        if (error) *error = std::string("ошибка линковки: ") + info;
        return 0;
    }
    return program;
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
    : m_vertexPath(vertexPath), m_fragmentPath(fragmentPath) {
    std::string error;
    m_id = BuildProgram(ReadFile(vertexPath), ReadFile(fragmentPath), &error);
    if (!m_id) throw std::runtime_error("Шейдер " + vertexPath + " + " + fragmentPath + ": " + error);
    LOG_DEBUG("Shader") << "Скомпилирован: " << vertexPath << " + " << fragmentPath;
}

bool Shader::Reload() {
    std::string vSrc, fSrc;
    try {
        vSrc = ReadFile(m_vertexPath);
        fSrc = ReadFile(m_fragmentPath);
    } catch (const std::exception& e) {
        LOG_WARN("Shader") << "Hot-reload пропущен (" << m_vertexPath << "): " << e.what();
        return false;
    }
    std::string error;
    unsigned int program = BuildProgram(vSrc, fSrc, &error);
    if (!program) {
        LOG_WARN("Shader") << "Hot-reload не удался (" << m_vertexPath << "): " << error
                           << " — оставляю прежнюю версию";
        return false;
    }
    if (m_id) glDeleteProgram(m_id);
    m_id = program;
    LOG_INFO("Shader") << "Hot-reload: " << m_vertexPath << " + " << m_fragmentPath;
    return true;
}

Shader::~Shader() {
    if (m_id) glDeleteProgram(m_id);
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
