#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>

#include <unordered_map>

class Texture; // загруженные PBR-карты держатся здесь как runtime-указатели
class Shader;  // собственный шейдер материала (см. VertexShaderPath)

// Значение пользовательского параметра шейдера. Тип хранится рядом со
// значением, потому что аплоад обязан вызвать ИМЕННО ту перегрузку, которую
// ждёт GLSL: залить vec4 туда, где объявлен float, — молча ничего не сделать.
struct ShaderParam {
    enum class Type { Float, Vec2, Vec3, Vec4 };
    Type Kind = Type::Float;
    glm::vec4 Value{0.0f};

    static ShaderParam Make(float v) { return {Type::Float, glm::vec4(v, 0, 0, 0)}; }
    static ShaderParam Make(const glm::vec2& v) { return {Type::Vec2, glm::vec4(v, 0, 0)}; }
    static ShaderParam Make(const glm::vec3& v) { return {Type::Vec3, glm::vec4(v, 0)}; }
    static ShaderParam Make(const glm::vec4& v) { return {Type::Vec4, v}; }
};

// ---------------------------------------------------------------------------
// Material — переиспользуемое описание внешнего вида объекта, живущее в
// файле проекта (`.sagemat`, JSON). Один материал прикрепляется к любому
// числу сущностей (MeshRendererComponent.MaterialPath / MaterialPtr):
// поменялся материал — поменялись все объекты с ним.
//
// v1 — данные без жёсткой привязки к конкретному шейдеру: движок и редактор
// гарантированно используют Albedo (базовый цвет вместо MeshRenderer.Color,
// когда материал назначен — см. EffectiveColor в Components.h); Emissive/
// Shininess/TexturePath сериализуются и доступны шейдерам игр, которые
// решают сами, что из этого поддерживать (как с PostProcessSettings).
//
// Загруженные материалы кэшируются и РАЗДЕЛЯЮТСЯ через
// ResourceManager::GetMaterial(path) — редактор правит поля прямо в общем
// экземпляре (все сущности с материалом обновляются мгновенно), а Save
// фиксирует их на диск.
// ---------------------------------------------------------------------------
struct Material {
    glm::vec3 Albedo{1.0f, 1.0f, 1.0f};
    glm::vec3 Emissive{0.0f, 0.0f, 0.0f};
    float Shininess = 32.0f; // legacy (Blinn-Phong) — оставлено для совместимости .sagemat

    // --- PBR (metallic-roughness workflow) ---
    // Скалярные факторы применяются ВСЕГДА; если задана соответствующая карта —
    // значение из карты умножается на фактор (glTF-семантика). Для чисто
    // текстурного материала держи факторы = 1.
    float Metallic = 0.0f;    // 0 — диэлектрик, 1 — металл
    float Roughness = 0.5f;   // 0 — зеркало, 1 — матовое
    float Opacity = 1.0f;     // 1 — непрозрачный; меньше — полупрозрачный проход

    // Полупрозрачное тело рисуется без отсечения задних граней: у стакана и
    // листвы видно и заднюю стенку. Но у ЗАМКНУТОГО тела (плитка воды, ящик
    // стекла) это значит два слоя смешивания на один объект — вода темнеет
    // вдвое, а на швах появляется сетка. Такому материалу ставят false.
    bool DoubleSided = true;

    // --- Собственный шейдер материала --------------------------------------
    // Пути к .vert/.frag относительно проекта. Пусто — материал рисуется
    // штатным PBR-шейдером движка. Заданный шейдер получает ТОТ ЖЕ набор
    // стандартных юниформ (камера, освещение, тени, время) и, если объект
    // попадает в инстансный путь, те же per-instance атрибуты — то есть
    // собственный шейдер не отменяет батчинг и годится не только для одиночных
    // «особенных» объектов, но и для воды, травы и прочего, чего на сцене тысячи.
    // Контракт по локациям и юниформам — в docs/custom_shaders.md.
    std::string VertexShaderPath;
    std::string FragmentShaderPath;

    // Пользовательские юниформы шейдера (имя -> значение). Сериализуются вместе
    // с материалом; скрипт может переопределить их для ОДНОЙ сущности, не трогая
    // общий материал (см. ShaderParamsComponent).
    std::unordered_map<std::string, ShaderParam> Params;

    // runtime: скомпилированная программа (ResourceManager::GetShader).
    std::shared_ptr<Shader> ShaderPtr;

    bool HasCustomShader() const {
        return !VertexShaderPath.empty() && !FragmentShaderPath.empty();
    }
    std::string TexturePath;      // albedo-карта (пусто — без текстуры)
    std::string NormalMapPath;    // normal map (tangent-space, OpenGL — зелёный вверх)
    std::string MetallicMapPath;  // metallic-карта (R-канал); пусто — только фактор
    std::string RoughnessMapPath; // roughness-карта (R-канал); пусто — только фактор
    std::string AOMapPath;        // ambient occlusion (R-канал); пусто — AO=1

    // runtime (не сериализуется): загруженные GPU-текстуры, заполняются
    // ResourceManager::GetMaterial по путям выше. nullptr — карта не задана.
    std::shared_ptr<Texture> AlbedoTex;
    std::shared_ptr<Texture> NormalTex;
    std::shared_ptr<Texture> MetallicTex;
    std::shared_ptr<Texture> RoughnessTex;
    std::shared_ptr<Texture> AOTex;
    // Рисуется текстурным PBR-путём, если задана хотя бы одна карта.
    bool HasMaps() const {
        return AlbedoTex || NormalTex || MetallicTex || RoughnessTex || AOTex;
    }

    // Читает .sagemat (JSON). Бросает std::runtime_error, если файл не
    // открывается/не парсится — вызывающий решает, чем ответить (редактор
    // показывает ошибку, ResourceManager логирует и отдаёт дефолт).
    static Material LoadFromFile(const std::string& path);

    // Пишет .sagemat (JSON, с отступами — файл правится и руками).
    // Бросает std::runtime_error, если файл не открывается на запись.
    void SaveToFile(const std::string& path) const;
};
