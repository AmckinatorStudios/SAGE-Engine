#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/assets/SkeletonData.h"

// ---------------------------------------------------------------------------
// SkinnedModelData — модель в ПАМЯТИ, до попадания на видеокарту (в рендере —
// SkinnedModelData).
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
//
// Живёт в слое ресурсов: её заполняют разборы (FBX здесь, glTF в рендере) и
// кэш, а превращают в своё рендер (SkinnedModel: геометрия и текстуры на
// видеокарте) и анимация (скелет и клипы — anim::SkeletonFromData).
// ---------------------------------------------------------------------------
namespace sage::assets {

// Вершина скина: геометрия + до 4 влияющих костей (индексы) и их веса.
//
// ПОЛЕЙ СТОЛЬКО ЖЕ, СКОЛЬКО У СТАТИЧЕСКОЙ ВЕРШИНЫ, плюс кости. Так и должно
// быть: скин отличается от обычного меша ровно тем, КАК получается позиция и
// нормаль, а не тем, что он умеет меньше. Пока здесь не было касательной и
// второй развёртки, у скина не могло быть ни карты нормалей, ни лайтмапы — не
// потому, что так решили, а потому, что данные до шейдера не доезжали.
struct SkinnedVertex {
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec2 TexCoords{0.0f};
    glm::vec4 Joints{0.0f};  // индексы костей (как float — читаются в шейдере через int())
    glm::vec4 Weights{0.0f}; // веса (нормируются при загрузке; сумма ~1)
    // Касательная для карты нормалей: xyz — направление вдоль U, w — знак
    // бинормали (±1). Считается при разборе, если её нет в файле.
    glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 TexCoords2{0.0f}; // вторая развёртка (лайтмапа/AO), 0 — её нет
};

// Распакованное изображение, RGBA8.
struct ModelImage {
    int Width = 0;
    int Height = 0;
    std::vector<unsigned char> Pixels; // Width * Height * 4
};

// Материал подмеша в ПАМЯТИ: факторы как есть, карты — индексами в
// SkinnedModelData::Images (-1 — карты нет). Индексами, а не копиями пикселей: одна
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

struct SkinnedModelData {
    SkeletonData Skeleton;
    std::vector<ClipData> Clips;
    std::vector<ModelImage> Images;
    std::vector<ModelSubMeshData> SubMeshes;
    std::vector<std::string> MorphNames;
    std::vector<float> MorphDefaults;

    bool Empty() const { return SubMeshes.empty(); }
};

} // namespace sage::assets
