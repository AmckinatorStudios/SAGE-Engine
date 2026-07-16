#pragma once
#include "Scene.h"
#include <string>

// Сохраняет/загружает Scene в человекочитаемый JSON.
// Сохраняются: имя сцены, все сущности (имя, transform, ссылка на меш, цвет,
// скрипт) и освещение. GPU-ресурсы (Mesh) при загрузке пересоздаются через
// ResourceManager.
//
// Строковые варианты (SaveToString/LoadFromString) — та же сериализация без
// файла: снапшоты сцены в памяти (undo/redo и Play-режим редактора).
namespace SceneSerializer {
    void Save(const Scene& scene, const std::string& path);

    // Возвращает загруженную сцену. Бросает std::runtime_error при ошибке чтения/парсинга.
    std::unique_ptr<Scene> Load(const std::string& path);

    // Сериализация в строку JSON (без обращения к диску).
    std::string SaveToString(const Scene& scene);

    // Восстановление сцены из строки JSON. Бросает std::runtime_error при ошибке парсинга.
    std::unique_ptr<Scene> LoadFromString(const std::string& jsonText);
}
