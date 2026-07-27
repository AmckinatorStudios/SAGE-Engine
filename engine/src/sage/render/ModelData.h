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

struct ModelSubMeshData {
    std::vector<SkinnedVertex> Vertices;
    std::vector<unsigned int> Indices;
    int Image = -1;              // индекс в ModelData::Images, -1 — без текстуры
    glm::vec3 Tint{1.0f};
    float Metallic = 0.0f;
    float Roughness = 0.6f;

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
