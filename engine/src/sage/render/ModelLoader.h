#pragma once
#include "Mesh.h"
#include "MeshData.h"
#include <memory>
#include <string>
#include <vector>

namespace ModelLoader {
    // Настройки импорта модели — хранятся в JSON-сайдкаре «<модель>.sageimport»
    // рядом с файлом. Применяются при ЗАГРУЗКЕ (пекутся в вершины меша), поэтому
    // модель приходит в сцену уже нормализованной, без ручной правки масштаба
    // каждого инстанса. Простой ассет-импорт-пайплайн: настройка живёт при
    // ассете, редактируется в Inspector, действует и в редакторе, и в игре.
    struct ImportSettings {
        float Scale = 1.0f;         // равномерный множитель размера
        bool Recenter = false;      // центр AABB -> в начало координат
        bool NormalizeSize = false; // наибольшая сторона -> 1 (до умножения на Scale)
    };

    // Загружает .obj и возвращает Mesh, применив ImportSettings из сайдкара
    // (если он есть). Бросает std::runtime_error при ошибке чтения модели.
    std::shared_ptr<Mesh> LoadObj(const std::string& path);

    // CPU-стадия той же загрузки: геометрия с применёнными ImportSettings, БЕЗ
    // создания GPU-меша (не требует GL). Используется бейкером GI (sage/gi),
    // которому нужны треугольники модели для трассировки и лайтмап-развёртки.
    sage::render::MeshData LoadObjData(const std::string& path);

    // --- Сайдкар настроек импорта (GL-независимо) ---
    std::string ImportSidecarPath(const std::string& modelPath); // «<path>.sageimport»
    ImportSettings LoadImportSettings(const std::string& modelPath); // дефолт, если нет/битый
    bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s);

    // Применяет настройки к вершинам НА МЕСТЕ (recenter -> normalize -> scale).
    // Чистая CPU-функция без GL — ядро пайплайна, юнит-тестируется.
    void ApplyImportSettings(std::vector<Vertex>& vertices, const ImportSettings& s);
}
