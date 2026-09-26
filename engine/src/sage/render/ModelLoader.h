#pragma once
#include "Mesh.h"
#include "MeshData.h"
#include "sage/assets/import/Importer.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace ModelLoader {
    // Настройки импорта модели — хранятся в JSON-сайдкаре «<модель>.sageimport»
    // рядом с файлом. Применяются при ЗАГРУЗКЕ (пекутся в вершины меша), поэтому
    // модель приходит в сцену уже нормализованной, без ручной правки масштаба
    // каждого инстанса. Простой ассет-импорт-пайплайн: настройка живёт при
    // ассете, редактируется в Inspector, действует и в редакторе, и в игре.
    //
    // НАБОР КАК В БОЛЬШИХ ДВИЖКАХ. Раньше здесь были масштаб, центрирование и
    // нормировка — и всё. Всё остальное, что чинят при импорте чужой модели
    // (лежит на боку, вывернута наизнанку, текстура вверх ногами, листва
    // квадратами, резкие грани вместо гладких), чинилось руками в сцене — и
    // заново для каждого экземпляра. Теперь это свойства АССЕТА: заданы один
    // раз, применяются при каждой загрузке — в редакторе и в собранной игре.
    struct ImportSettings {
        // --- Трансформ (запекается в вершины) --------------------------------
        float Scale = 1.0f;         // равномерный множитель размера
        bool Recenter = false;      // центр AABB -> в начало координат
        bool NormalizeSize = false; // наибольшая сторона -> 1 (до умножения на Scale)
        glm::vec3 Rotation{0.0f};   // градусы, оси X, Y, Z (Z вверх -> Y вверх: X = -90)
        glm::vec3 Offset{0.0f};     // сдвиг после масштаба и поворота, метры
        // Персонаж в САНТИМЕТРАХ — привести к метрам (×0.01). Выгрузки из
        // Выгрузки из многих программ пишут сантиметры без масштаба в
        // узлах, и человек ростом 1.8 м приезжал башней в 180 м. Срабатывает
        // только у моделей со скелетом выше kCentimetreGuess: персонаж такого
        // роста не бывает, а большой статичный ландшафт — бывает.
        bool AutoUnits = true;

        // --- Геометрия -------------------------------------------------------
        enum class NormalMode { Import = 0, Smooth = 1, Flat = 2 };
        NormalMode Normals = NormalMode::Import;  // из файла или пересчитать
        bool FlipUV = false;        // v -> 1 - v (текстура вверх ногами)
        bool FlipWinding = false;   // модель вывернута наизнанку

        // --- Материалы (применяются при создании .sagemat) -------------------
        bool ImportMaterials = true;             // создавать .sagemat из файла
        enum class AlphaMode { Auto = 0, Opaque = 1, Cutout = 2 };
        AlphaMode Alpha = AlphaMode::Auto;       // Auto — как описал файл
        float AlphaCutoff = 0.5f;                // порог для Cutout
        enum class TwoSided { Auto = 0, On = 1, Off = 2 };
        TwoSided DoubleSided = TwoSided::Auto;

        // --- Анимация --------------------------------------------------------
        bool ImportAnimation = true;             // скелет и клипы, если они есть

        // Трогают ли настройки геометрию вообще — без этого загрузка не
        // тратит время на пересчёты.
        bool ChangesGeometry() const;
    };

    // Больше этого (метров) по наибольшей стороне персонаж со скелетом быть не
    // может — значит, файл в сантиметрах (см. ImportSettings::AutoUnits).
    constexpr float kCentimetreGuess = 50.0f;

    // Множитель единиц: 0.01 — «модель в сантиметрах», иначе 1.
    float AutoUnitScale(const ImportSettings& s, bool hasSkeleton, float maxExtent);

    // Настройки из сайдкара + решение о единицах по самой геометрии (min/max
    // её вершин). Одна точка для статики и персонажей: иначе одна и та же
    // модель в сцене и в редакторе оказалась бы разного размера.
    ImportSettings ResolveImportSettings(const std::string& path, const glm::vec3& lo,
                                         const glm::vec3& hi);

    // Центрирование, нормировка, масштаб, поворот и сдвиг — ОДНОЙ матрицей, в
    // том же порядке, что ApplyImportSettings для вершин. Её получает
    // скелетная модель: её вершины нельзя «запечь» — кости и клипы остались
    // бы в прежнем размере.
    glm::mat4 ImportMatrix(const ImportSettings& s, const glm::vec3& lo, const glm::vec3& hi);

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
    // ПУТЁМ: по нему открывают файл (см. scripts/check_paths.py).
    std::filesystem::path ImportSidecarPath(const std::string& modelPath); // «<path>.sageimport»
    ImportSettings LoadImportSettings(const std::string& modelPath); // дефолт, если нет/битый
    bool SaveImportSettings(const std::string& modelPath, const ImportSettings& s);

    // Применяет настройки к вершинам НА МЕСТЕ (recenter -> normalize -> scale).
    // Чистая CPU-функция без GL — ядро пайплайна, юнит-тестируется.
    void ApplyImportSettings(std::vector<Vertex>& vertices, const ImportSettings& s);
    // Полное применение к мешу: трансформ (с нормалями и касательными), пересчёт
    // нормалей, развёртка, обход граней. Порядок: геометрия файла -> нормали ->
    // центрирование/нормировка/масштаб -> поворот -> сдвиг.
    void ApplyImportSettings(sage::render::MeshData& mesh, const ImportSettings& s);
}
