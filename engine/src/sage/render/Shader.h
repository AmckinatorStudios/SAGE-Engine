#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "sage/rhi/Resources.h"

// Пути к шейдеру векторного debug-текста (stb_easy_font) — используется и
// UIRenderer, и DebugOverlay, поэтому вынесено в одно место вместо двух
// одинаковых строковых литералов в разных .cpp.
namespace ShaderPaths {
    constexpr const char* DebugTextVert = "assets/shaders/debug_text.vert";
    constexpr const char* DebugTextFrag = "assets/shaders/debug_text.frag";
}

// Загружает вершинный+фрагментный шейдеры из файлов, компилирует их через
// текущий графический бэкенд (rhi::GraphicsDevice) и даёт удобные методы для
// передачи uniform-переменных. Сам класс не знает, какой API под капотом —
// это тонкая обёртка над rhi::ShaderProgram, добавляющая чтение файлов.
class Shader {
public:
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    // Владеет GPU-программой единолично — копирование запрещено, перемещение
    // безопасно (владение переезжает вместе с unique_ptr).
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept = default;
    Shader& operator=(Shader&&) noexcept = default;

    void Use() const { m_program->Use(); }

    void SetMat4(const std::string& name, const glm::mat4& value) const { m_program->SetMat4(name, value); }
    void SetVec3(const std::string& name, const glm::vec3& value) const { m_program->SetVec3(name, value); }
    void SetVec4(const std::string& name, const glm::vec4& value) const { m_program->SetVec4(name, value); }
    void SetVec2(const std::string& name, const glm::vec2& value) const { m_program->SetVec2(name, value); }
    void SetFloat(const std::string& name, float value) const { m_program->SetFloat(name, value); }
    void SetInt(const std::string& name, int value) const { m_program->SetInt(name, value); }

private:
    static std::string ReadFile(const std::string& path);

    std::unique_ptr<sage::rhi::ShaderProgram> m_program;
};
