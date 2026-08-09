#pragma once
#include "Mesh.h"
#include "MeshData.h"
#include "sage/assets/import/Importer.h"
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
    //
    // keepCpuData — сохранить копию геометрии на стороне процессора. Нужна там,
    // где по мешу ещё что-то считают, а не только рисуют: точный выбор объекта
    // мышью в редакторе, подгонка камеры, уровни детализации. По умолчанию
    // выключено — в игре эти мегабайты ни на что не работают.
    std::shared_ptr<Mesh> LoadMesh(const std::string& path, bool keepCpuData = false);

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
    // materialsOut — список материалов .mtl В ПОРЯДКЕ ФАЙЛА, если он нужен.
    // Порядок здесь не деталь: MeshData::Submeshes[i].Material — это индекс
    // именно в нём (и в том же порядке отдаёт материалы ExtractMaterials).
    sage::render::MeshData LoadObjData(
        const std::string& path,
        std::vector<sage::assets::ImportedMaterial>* materialsOut = nullptr);

    // Геометрия glTF/GLB в ОДНОМ меше — но С ГРАНИЦАМИ по материалам.
    //
    // Один меш: MeshRendererComponent держит ровно один Mesh, а в файле glTF
    // почти всегда несколько примитивов (по одному на материал). Отдать «первый
    // попавшийся» значило бы показать четверть модели.
    //
    // С границами: слить их БЕЗ разметки, как было раньше, значит покрасить всю
    // модель одним материалом — тем, что попался импортёру первым. Разметку
    // строит ImportedScene::Flatten() из узлов (см. GltfImporter.cpp).
    sage::render::MeshData LoadGltfData(const std::string& path, bool binary);

    // --- Сайдкар настроек импорта (GL-независимо) ---
    std::string ImportSidecarPath(const std::string& modelPath); // «<path>.sageimport»
    ImportSettings LoadImportSettings(const std::string& modelPath); // дефолт, если нет/битый
    bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s);

    // Применяет настройки к вершинам НА МЕСТЕ (recenter -> normalize -> scale).
    // Чистая CPU-функция без GL — ядро пайплайна, юнит-тестируется.
    void ApplyImportSettings(std::vector<Vertex>& vertices, const ImportSettings& s);
}
