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

    // Загружает модель ЛЮБОГО поддерживаемого формата (.obj, .gltf, .glb) и
    // возвращает Mesh, применив ImportSettings из сайдкара.
    //
    // РАНЬШЕ ЗДЕСЬ БЫЛ ТОЛЬКО .obj, и это была не мелочь: сущность сцены умеет
    // держать один Mesh, грузился он через LoadObj, и потому в редакторе нельзя
    // было поставить в сцену НИ ОДНУ модель в glTF — то есть в формате, в
    // который экспортирует Blender по умолчанию и в котором лежит почти всё
    // бесплатное. Класс Model формат понимал, но он не Mesh и в ECS не
    // подключён; выглядело это как «модели не загружаются вообще».
    //
    // Бросает std::runtime_error с ВНЯТНЫМ текстом: неподдерживаемое
    // расширение, отсутствующий файл и битый файл — три разные причины, и
    // человеку, у которого «модель не грузится», нужна именно та, что случилась.
    std::shared_ptr<Mesh> LoadMesh(const std::string& path);

    // CPU-стадия той же загрузки: геометрия с применёнными ImportSettings, БЕЗ
    // создания GPU-меша (не требует GL). Используется бейкером GI (sage/gi),
    // которому нужны треугольники модели для трассировки и лайтмап-развёртки.
    sage::render::MeshData LoadMeshData(const std::string& path);

    // Поддерживается ли расширение файла (для панели Assets и подсказок).
    bool IsSupportedModel(const std::string& path);

    // Старые имена — только .obj. Оставлены: на них ссылается код бейкера и
    // тесты, а сужать их до подмножества нового API значило бы менять смысл
    // существующих вызовов.
    std::shared_ptr<Mesh> LoadObj(const std::string& path);
    sage::render::MeshData LoadObjData(const std::string& path);

    // Геометрия glTF/GLB, слитая в ОДИН меш.
    //
    // Слитая намеренно: MeshRendererComponent держит ровно один Mesh, а в файле
    // glTF почти всегда несколько примитивов (по одному на материал). Отдать
    // «первый попавшийся» значило бы показать четверть модели и оставить
    // человека гадать, куда делось остальное.
    sage::render::MeshData LoadGltfData(const std::string& path, bool binary);

    // --- Сайдкар настроек импорта (GL-независимо) ---
    std::string ImportSidecarPath(const std::string& modelPath); // «<path>.sageimport»
    ImportSettings LoadImportSettings(const std::string& modelPath); // дефолт, если нет/битый
    bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s);

    // Применяет настройки к вершинам НА МЕСТЕ (recenter -> normalize -> scale).
    // Чистая CPU-функция без GL — ядро пайплайна, юнит-тестируется.
    void ApplyImportSettings(std::vector<Vertex>& vertices, const ImportSettings& s);
}
