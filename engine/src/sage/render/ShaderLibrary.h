#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/rhi/Resources.h"

namespace sage::rhi {
class GraphicsDevice;
}

// ---------------------------------------------------------------------------
// ShaderLibrary — свои шейдеры как ассеты: загрузка, #include, сборка любых
// стадий, ошибки по файлам и строкам, правка на лету.
//
// ЗАЧЕМ ОТДЕЛЬНО ОТ Shader. Shader (render/Shader.h) — обёртка для движковых
// шейдеров: пара файлов, ошибка — исключение. Для своего рендера этого мало:
//   • шейдер чанка и шейдер воды делят функции освещения — без #include их
//     копируют, и копии расходятся;
//   • ошибка «0:57(12): error» ничего не говорит, если строка 57 — это строка
//     склеенного текста, а не файла, который открыт в редакторе;
//   • шейдер правят, пока игра запущена, и ошибка в правке не должна гасить
//     кадр — рисоваться продолжает прежняя рабочая версия.
//
// ЧЕГО ОН НЕ ДЕЛАЕТ. Не транслирует языки: GLSL уходит драйверу как есть.
// SPIR-V и HLSL — вопрос второго бэкенда (Vulkan берёт GLSL через glslang в
// rhi/vulkan), а не этой библиотеки.
// ---------------------------------------------------------------------------
namespace sage::render {

// Одна ошибка или предупреждение — уже с ИМЕНЕМ ФАЙЛА и строкой в нём.
struct ShaderDiagnostic {
    sage::rhi::ShaderStage Stage = sage::rhi::ShaderStage::Vertex;
    bool Error = true;  // false — предупреждение
    bool Link = false;  // ошибка сборки программы целиком
    std::string File;   // файл, где ошибка (пусто — неизвестно)
    int Line = 0;       // строка в этом файле (0 — неизвестна)
    std::string Message;

    // «assets/shaders/chunk.frag:57: error: 'foo' undeclared»
    std::string ToString() const;
};

// Разрешает #include "имя" в текст. false — не найдено.
using ShaderIncludeResolver = std::function<bool(const std::string& name, std::string& out)>;

struct PreprocessedShader {
    bool Ok = true;
    std::string Code;
    // Номер исходника в директивах #line -> файл. Индекс 0 — сам шейдер.
    std::vector<std::string> Files;
    std::string Error; // «chunk.frag:3: не найден #include "light.glsl"»
};

// Раскрывает #include "имя" (рекурсивно, каждый файл — один раз) и ставит
// директивы #line, чтобы драйвер называл строки исходных файлов. Имя ищется
// сначала среди встроенных (RegisterShaderInclude), затем resolver'ом.
PreprocessedShader PreprocessShader(const std::string& code, const std::string& origin,
                                    const ShaderIncludeResolver& resolver);

// Разбирает ответ драйвера на ошибки с файлами и строками. Понимает форматы
// Mesa («0:12(5): error: …»), NVIDIA («0(12) : error C0000: …») и
// AMD/Intel («ERROR: 0:12: …»); строку незнакомого вида отдаёт целиком.
std::vector<ShaderDiagnostic> ParseShaderLog(const std::string& text, sage::rhi::ShaderStage stage,
                                             bool link, const std::vector<std::string>& files);

// Встроенный текст для #include — общие функции движка, доступные любому
// своему шейдеру. Движок регистрирует "sage/common.glsl" (полноэкранный
// треугольник, sRGB, ACES) и "sage/pbr.glsl" (освещение штатного PBR).
void RegisterShaderInclude(const std::string& name, std::string text);
const std::string* FindShaderInclude(const std::string& name);

// Стадия программы — файлом или готовым текстом.
struct ShaderStageFile {
    sage::rhi::ShaderStage Stage = sage::rhi::ShaderStage::Vertex;
    std::string Path;   // файл (пусто — берётся Source)
    std::string Source; // готовый текст (встроенный шейдер игры)
};

struct ShaderEntry {
    std::string Name;
    std::vector<ShaderStageFile> Stages;
    std::unique_ptr<sage::rhi::ShaderProgram> Program; // последняя РАБОЧАЯ сборка
    std::vector<ShaderDiagnostic> Diagnostics;         // ответ последней сборки
    bool LastBuildOk = false;
    int Version = 0; // растёт с каждой успешной сборкой — повод перечитать униформы

    // Файлы, из которых собрана программа (вместе с включёнными), и время их
    // правки — по нему ReloadChanged узнаёт, что пересобирать.
    std::vector<std::pair<std::string, long long>> Watched;
};

class ShaderLibrary {
public:
    // device == nullptr — текущее устройство процесса (GraphicsDevice::Get()).
    explicit ShaderLibrary(sage::rhi::GraphicsDevice* device = nullptr);
    ~ShaderLibrary();

    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Собирает программу и запоминает под именем. Возвращает рабочую программу
    // (nullptr — не собралась ни разу; причина — Find(name)->Diagnostics).
    // Повторный вызов с тем же именем пересобирает.
    sage::rhi::ShaderProgram* Load(const std::string& name, const std::string& vertexPath,
                                   const std::string& fragmentPath);
    sage::rhi::ShaderProgram* LoadCompute(const std::string& name, const std::string& path);
    sage::rhi::ShaderProgram* LoadStages(const std::string& name,
                                         std::vector<ShaderStageFile> stages);

    sage::rhi::ShaderProgram* Get(const std::string& name) const;
    const ShaderEntry* Find(const std::string& name) const;
    std::vector<const ShaderEntry*> All() const;

    // Пересобрать всё, у чего изменился хотя бы один файл (включая
    // подключённые через #include). Ошибка НЕ заменяет рабочую программу.
    // Возвращает, сколько программ пересобрано успешно.
    int ReloadChanged();
    bool Rebuild(const std::string& name);

    // Каталоги, где ищутся #include, кроме каталога самого шейдера.
    void AddIncludeDir(std::string dir) { m_includeDirs.push_back(std::move(dir)); }

    // Сколько всего ошибок в последних сборках (для строки состояния игры).
    int ErrorCount() const;

private:
    bool Build(ShaderEntry& entry);

    sage::rhi::GraphicsDevice* m_device = nullptr;
    std::unordered_map<std::string, std::unique_ptr<ShaderEntry>> m_entries;
    std::vector<std::string> m_order; // порядок загрузки — для All()
    std::vector<std::string> m_includeDirs;
};

} // namespace sage::render
