#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>

class Texture; // загруженные PBR-карты держатся здесь как runtime-указатели

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
