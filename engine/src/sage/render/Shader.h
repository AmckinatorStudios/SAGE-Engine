#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>
#include "sage/rhi/Resources.h"
#include "sage/render/Material.h"

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

    // Компиляция прямо из исходников (не из файлов) — для встроенных шейдеров
    // движка, которые не должны зависеть от ассетов конкретной игры (напр.
    // шейдер текста UIRenderer). tag — имя для сообщений лога.
    static Shader FromSource(const std::string& vertexSrc, const std::string& fragmentSrc,
                             const std::string& tag = "inline");

    // Владеет GPU-программой единолично — копирование запрещено, перемещение
    // безопасно (владение переезжает вместе с unique_ptr).
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept = default;
    Shader& operator=(Shader&&) noexcept = default;

    void Use() const { m_program->Use(); }

    void SetMat4(const std::string& name, const glm::mat4& value) const { m_program->SetMat4(name, value); }
    void SetMat4Array(const std::string& name, const glm::mat4* values, int count) const { m_program->SetMat4Array(name, values, count); }
    void SetVec3(const std::string& name, const glm::vec3& value) const { m_program->SetVec3(name, value); }
    void SetVec4(const std::string& name, const glm::vec4& value) const { m_program->SetVec4(name, value); }
    void SetVec2(const std::string& name, const glm::vec2& value) const { m_program->SetVec2(name, value); }
    void SetFloat(const std::string& name, float value) const { m_program->SetFloat(name, value); }
    void SetInt(const std::string& name, int value) const { m_program->SetInt(name, value); }
    void SetIntArray(const std::string& name, const int* v, int count) const { m_program->SetIntArray(name, v, count); }
    void SetFloatArray(const std::string& name, const float* v, int count) const { m_program->SetFloatArray(name, v, count); }

    // Текст файла шейдера тем же путём, каким его читает сам Shader: пакет
    // игры, затем диск, затем папка программы. Бросает, если файла нет.
    static std::string LoadText(const std::string& path) { return ReadFile(path); }

    // Текст со СВОИМИ включениями: «#include "noise.glsl"» подставляет файл
    // относительно того, кто его включил (рекурсивно, каждый файл один раз).
    // Общий код шейдеров проекта — шум, освещение, свои функции — пишется
    // один раз, а не копируется в каждый материал. Встроенный блок движка
    // (#include <sage_pbr>) — отдельно, при компиляции. included — какие файлы
    // подставлены (для пересборки при их правке).
    static std::string LoadExpanded(const std::string& path, std::vector<std::string>* included = nullptr);

    // Умолчания параметров из подсказок в тексте шейдера
    // («uniform float uWaveHeight; // 0.35 [0, 2]», см. ShaderParams.h).
    // Заливаются ДО параметров материала: шейдер, которому в .sagemat ещё не
    // дали значений, получает задуманное автором, а не нули (нулевая длина
    // волны — деление на ноль, нулевой цвет — чёрный объект).
    std::unordered_map<std::string, ShaderParam> Defaults;

    // Текстуры, которые шейдер объявил параметрами (uniform sampler2D с
    // подсказкой, см. ShaderParams.h), в порядке объявления. Юниты им раздаёт
    // отрисовка из свободных.
    struct TextureSlot { std::string Name; bool Linear = false; };
    std::vector<TextureSlot> Textures;
    // Подставленные «#include "…"» файлы (для пересборки при их правке).
    std::vector<std::string> Included;

private:
    static std::string ReadFile(const std::string& path);

    // Тег-конструктор из готовых исходников (используется FromSource).
    struct FromSourceTag {};
    Shader(FromSourceTag, const std::string& vertexSrc, const std::string& fragmentSrc,
           const std::string& tag);

    std::unique_ptr<sage::rhi::ShaderProgram> m_program;
};
