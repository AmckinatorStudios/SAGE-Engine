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

    // Перечитывает и перекомпилирует шейдер из тех же файлов (hot-reload).
    // При успехе подменяет GL-программу НА МЕСТЕ — все, кто держит ссылку на
    // этот Shader, продолжают работать без изменений, просто с новым кодом.
    // При ошибке (файл не открылся / не скомпилировался) СТАРАЯ программа
    // сохраняется и продолжает работать, а метод возвращает false — так
    // опечатка в шейдере во время hot-reload не роняет игру.
    bool Reload();

    const std::string& VertexPath() const { return m_vertexPath; }
    const std::string& FragmentPath() const { return m_fragmentPath; }

private:
    unsigned int m_id = 0;
    std::string m_vertexPath;
    std::string m_fragmentPath;

    static std::string ReadFile(const std::string& path);
    static unsigned int Compile(unsigned int type, const std::string& source);
    // Собирает программу из исходников. Возвращает 0 при ошибке (не бросает) и
    // кладёт причину в *error — общий путь и для конструктора, и для Reload().
    static unsigned int BuildProgram(const std::string& vSrc, const std::string& fSrc, std::string* error);
};
