#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"
#include "sage/render/SkinnedModel.h"

// ---------------------------------------------------------------------------
// ModelData — модель в ПАМЯТИ, до попадания на видеокарту.
//
// Зачем нужен отдельный слой. Загрузка модели состоит из двух совершенно разных
// работ: РАЗОБРАТЬ файл (прочитать glTF, разжать PNG, пересобрать вершины) и
// ЗАГРУЗИТЬ результат в видеопамять. Первая — чистые вычисления над байтами,
// вторая требует контекста OpenGL. Пока они были слиты в одну функцию, между
// ними некуда было вставить кэш, и разбор повторялся при каждом запуске.
//
// Замер на модели 48 тысяч вершин с текстурой 2048x2048:
//     разбор glTF          4.6 мс
//     + декод текстуры    37.9 мс      <- 87% времени уходит сюда
// Именно поэтому кэш обязан хранить РАСПАКОВАННЫЕ пиксели, а не ссылку на
// исходный PNG: без этого экономится восьмая часть, а не всё.
//
// Всё в этой структуре — простые данные без указателей на GPU-объекты, поэтому
// она пишется в файл почти как есть (см. sage/assets/AssetCache.h).
// ---------------------------------------------------------------------------
namespace sage::render {

// Распакованное изображение, RGBA8.
struct ModelImage {
    int Width = 0;
    int Height = 0;
    std::vector<unsigned char> Pixels; // Width * Height * 4
};

// Материал подмеша в ПАМЯТИ: факторы как есть, карты — индексами в
// ModelData::Images (-1 — карты нет). Индексами, а не копиями пикселей: одна
// картинка обслуживает по десятку материалов, и хранить её десять раз значило
// бы раздуть и память, и кэш ровно во столько же раз.
//
// Набор ТОТ ЖЕ, что у статического материала (sage/render/Material.h). Пока
// здесь лежала одна текстура и два числа, всё остальное, что несёт файл
// модели, — карта металличности, шероховатости, нормалей, свечение,
// прозрачность, двусторонность — молча выбрасывалось при загрузке.
struct ModelSubMeshMaterial {
    std::string Name;
    int Albedo = -1;
    int Normal = -1;
    int MetallicMap = -1;
    int RoughnessMap = -1;
    int AOMap = -1;
    int EmissiveMap = -1;
    // Из какого канала карты брать значение (0 R, 1 G, 2 B, 3 A). glTF пакует
    // затенение, шероховатость и металличность в R, G и B ОДНОЙ текстуры —
    // хранить три её копии с переложенными каналами значило бы утроить и
    // память, и кэш (см. uMetallicMask в PbrShader.h).
    int MetallicChannel = 0;
    int RoughnessChannel = 0;
    int AOChannel = 0;

    glm::vec3 Tint{1.0f};
    float Opacity = 1.0f;
    float Metallic = 0.0f;
    float Roughness = 0.6f;
    glm::vec3 Emissive{0.0f};

    int AlphaMode = 0;       // 0 Opaque, 1 Mask, 2 Blend (см. SkinnedMaterial::Alpha)
    float AlphaCutoff = 0.5f;
    bool DoubleSided = false;
    bool Unlit = false;      // KHR_materials_unlit: цвет как есть, без света
};

struct ModelSubMeshData {
    std::vector<SkinnedVertex> Vertices;
    std::vector<unsigned int> Indices;
    ModelSubMeshMaterial Material;

    // Морф-цели: дельты позиций и нормалей, разложенные по строкам текстуры.
    // Раскладка (ширина, строк на цель) считается при разборе и сохраняется —
    // пересчитывать её при загрузке из кэша значило бы держать одну и ту же
    // формулу в двух местах.
    int MorphCount = 0;
    int MorphWidth = 0;
    int MorphRows = 0;
    std::vector<float> MorphPositions;
    std::vector<float> MorphNormals;
};

struct ModelData {
    sage::anim::Skeleton Skeleton;
    std::vector<sage::anim::AnimationClip> Clips;
    std::vector<ModelImage> Images;
    std::vector<ModelSubMeshData> SubMeshes;
    std::vector<std::string> MorphNames;
    std::vector<float> MorphDefaults;

    bool Empty() const { return SubMeshes.empty(); }
};

} // namespace sage::render
