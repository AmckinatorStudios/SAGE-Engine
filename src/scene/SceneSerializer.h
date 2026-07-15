#pragma once
#include "Scene.h"
#include <string>

// Сохраняет/загружает Scene в человекочитаемый JSON-файл.
// Сохраняются: имя сцены, все объекты (имя, transform, ссылка на меш,
// цвет). GPU-ресурсы (Mesh) при загрузке пересоздаются через AssetManager.
namespace SceneSerializer {
    void Save(const Scene& scene, const std::string& path);

    // Возвращает загруженную сцену. Бросает std::runtime_error при ошибке чтения/парсинга.
    std::unique_ptr<Scene> Load(const std::string& path);
}
