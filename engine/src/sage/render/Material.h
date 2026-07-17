#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>

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
    float Shininess = 32.0f;
    std::string TexturePath; // пусто — без текстуры; путь относительно assets игры

    // Читает .sagemat (JSON). Бросает std::runtime_error, если файл не
    // открывается/не парсится — вызывающий решает, чем ответить (редактор
    // показывает ошибку, ResourceManager логирует и отдаёт дефолт).
    static Material LoadFromFile(const std::string& path);

    // Пишет .sagemat (JSON, с отступами — файл правится и руками).
    // Бросает std::runtime_error, если файл не открывается на запись.
    void SaveToFile(const std::string& path) const;
};
