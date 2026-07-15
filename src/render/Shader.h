#pragma once
#include <glad/glad.h>
#include <string>
#include <glm/glm.hpp>

// Пути к шейдеру векторного debug-текста (stb_easy_font) — используется и
// UIRenderer, и DebugOverlay, поэтому вынесено в одно место вместо двух
// одинаковых строковых литералов в разных .cpp.
namespace ShaderPaths {
    constexpr const char* DebugTextVert = "assets/shaders/debug_text.vert";
    constexpr const char* DebugTextFrag = "assets/shaders/debug_text.frag";
}

// Загружает и компилирует vertex+fragment шейдеры, даёт удобные методы
// для передачи uniform-переменных.
class Shader {
public:
    Shader(const std::string& vertexPath, const std::string& fragmentPath);
    ~Shader();

    // Владеет GL-объектом программы (m_id) — копирование запрещено. Без
    // этого неявный копирующий конструктор скопировал бы голый ID программы,
    // а деструктор одной из копий удалил бы её из-под другой, ещё живой (см.
    // фикс такого же бага в Mesh). Сейчас Shader используется только как
    // именованные переменные/поля, которые сами не копируются, но явный
    // запрет защищает от того, чтобы это молча сломалось в будущем
    // (например, если появится кэш шейдеров, возвращающий Shader по значению).
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void Use() const;

    void SetMat4(const std::string& name, const glm::mat4& value) const;
    void SetVec3(const std::string& name, const glm::vec3& value) const;
    void SetVec4(const std::string& name, const glm::vec4& value) const;
    void SetVec2(const std::string& name, const glm::vec2& value) const;
    void SetFloat(const std::string& name, float value) const;
    void SetInt(const std::string& name, int value) const;

private:
    unsigned int m_id;

    static std::string ReadFile(const std::string& path);
    static unsigned int Compile(unsigned int type, const std::string& source);
};
