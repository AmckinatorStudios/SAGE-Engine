#pragma once
#include "Mesh.h"
#include "MeshData.h"
#include "sage/assets/import/ImportSettings.h"
#include "sage/assets/import/Importer.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace ModelLoader {
    // Настройки импорта (.sageimport) и их применение — в слое ресурсов
    // (sage/assets/import/ImportSettings.h): ими пользуется и разбор .obj там же.
    // Прежние имена оставлены, на них ссылаются редактор, рендер и тесты.
    using ImportSettings = sage::assets::ImportSettings;
    using PartBounds = sage::assets::PartBounds;
    using sage::assets::kCentimetreGuess;
    using sage::assets::AutoUnitScale;
    using sage::assets::RobustBounds;
    using sage::assets::ResolveImportSettings;
    using sage::assets::ImportMatrix;
    using sage::assets::ImportSidecarPath;
    using sage::assets::LoadImportSettings;
    using sage::assets::SaveImportSettings;
    using sage::assets::ApplyImportSettings;
    // Разбор .obj — тоже в ресурсах (sage/assets/import/ObjImporter.cpp).
    using sage::assets::LoadObjData;

    // Загружает модель ЛЮБОГО поддерживаемого формата (.obj, .gltf, .glb) и
    // возвращает Mesh, применив ImportSettings из сайдкара.
    //
    // РАНЬШЕ ЗДЕСЬ БЫЛ ТОЛЬКО .obj, и это была не мелочь: сущность сцены умеет
    // держать один Mesh, грузился он через LoadObj, и потому в редакторе нельзя
    // было поставить в сцену НИ ОДНУ модель в glTF — то есть в формате, в
    // который экспортирует 3D-редактор по умолчанию и в котором лежит почти всё
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

    // --- МОДЕЛЬ ДЕРЕВОМ -------------------------------------------------------
    //
    // Модель из нескольких узлов (персонаж Blockbench: пояс, голова, руки,
    // ноги) ставится в сцену иерархией объектов, у каждого — геометрия СВОЕГО
    // узла (см. editor/src/ModelHierarchy.h). Здесь — то, что для этого нужно
    // движку: дерево узлов и меш одного узла.
    //
    // rootMatrix — настройки импорта (.sageimport: масштаб, поворот, сдвиг,
    // центрирование) одной матрицей: в иерархии их несёт корневой объект, а не
    // вершины каждой части — иначе каждая часть центрировалась бы сама по себе.
    bool LoadModelTree(const std::string& path, sage::assets::ImportedScene& scene,
                       glm::mat4& rootMatrix, std::string& err);
    // Геометрия узла group в его собственных координатах. Бросает
    // std::runtime_error, если узла нет (модель переэкспортировали иначе).
    sage::render::MeshData LoadMeshPartData(const std::string& path, int group);
    std::shared_ptr<Mesh> LoadMeshPart(const std::string& path, int group, bool keepCpuData = false);

    // Поддерживается ли расширение файла (для панели Assets и подсказок).
    bool IsSupportedModel(const std::string& path);

    // Старые имена — только .obj. Оставлены: на них ссылается код бейкера и
    // тесты, а сужать их до подмножества нового API значило бы менять смысл
    // существующих вызовов.
    std::shared_ptr<Mesh> LoadObj(const std::string& path);

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

}
