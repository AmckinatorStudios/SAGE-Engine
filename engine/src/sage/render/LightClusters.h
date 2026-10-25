#pragma once
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "sage/scene/Light.h"

namespace sage::rhi { class Texture2D; }
class Shader;

// ---------------------------------------------------------------------------
// КЛАСТЕРНЫЙ СВЕТ — сколько угодно ламп в кадре, а не восемь.
//
// Раньше шейдер получал источники массивами uniform'ов: по восемь точечных и
// прожекторов, и каждый пиксель перебирал их все. Девятая лампа уровня гасла
// молча, а восемь ламп по разным углам карты стоили каждому пикселю восьми
// расчётов, хотя до него доставала одна.
//
// Теперь пирамида взгляда нарезана на ячейки (16 × 9 по экрану × 24 по
// глубине, срезы по глубине растут геометрически — вблизи мельче). Процессор
// раз в кадр раскладывает источники по ячейкам, до которых достаёт их шар
// влияния, и кладёт всё в одну текстуру данных. Пиксель находит свою ячейку и
// перебирает только её список. Цена кадра — от числа ламп, ДОСТАЮЩИХ до
// пикселя, а не от числа ламп в сцене.
//
// Одна текстура (юнит 7), а не три: у фрагментной стадии шестнадцать юнитов,
// и все они были заняты (см. раскладку в render/ShadowMap.h). Внутри неё три
// области подряд: источники (по kTexelsPerLight текселей), ячейки (смещение и
// число в списке), список номеров (по четыре в текселе).
// ---------------------------------------------------------------------------
namespace sage::render {

constexpr int kLightClusterUnit = 7;

struct ClusterGrid {
    static constexpr int TilesX = 16;
    static constexpr int TilesY = 9;
    static constexpr int Slices = 24;
    static constexpr int Cells = TilesX * TilesY * Slices;
};

// Глубина -> номер среза. ОДНА формула на процессор и шейдер (там — её копия в
// kPbrSharedGlsl): разойдись они, лампа попадала бы в соседний срез и
// обрезалась бы ровной ступенькой поперёк пола.
struct ClusterDepth {
    float Near = 0.1f;
    float Far = 100.0f;
    bool Log = true;     // перспектива — геометрические срезы; ортография — равные
    float Scale = 1.0f;  // срезов на единицу (log(d/near) или d - near)

    static ClusterDepth FromProjection(const glm::mat4& proj);
    int Slice(float depth) const;
};

// Упакованный кадр: то, что уходит в текстуру. Отдельно от GPU-части, чтобы
// раскладка проверялась тестом без видеокарты.
struct LightClusterData {
    static constexpr int kTexelsPerLight = 6;
    static constexpr int kWidth = 1024;          // ширина текстуры (минимум GL 3.3)
    static constexpr int kMaxIndices = 1 << 16;  // всего ссылок «ячейка -> лампа»

    enum LightType { Point = 0, Spot = 1, Area = 2 };

    ClusterDepth Depth;
    glm::mat4 View{1.0f};
    glm::mat4 Proj{1.0f};
    int LightCount = 0;
    int GridBase = 0;    // первый тексель ячеек
    int IndexBase = 0;   // первый тексель списка
    int IndexCount = 0;  // ссылок в списке
    int Dropped = 0;     // ссылок, не влезших в kMaxIndices
    std::vector<glm::vec4> Texels;   // ровно Rows() * kWidth

    int Rows() const { return (int)Texels.size() / kWidth; }
    // Номера ламп в ячейке (для тестов и отладки).
    std::vector<int> CellLights(int cell) const;
};

// Раскладка источников кадра по ячейкам. Тени (ShadowSlot) берутся из env как
// есть: их раздаёт атлас до этого вызова.
LightClusterData BuildLightClusters(const LightingEnvironment& env, const glm::mat4& view,
                                    const glm::mat4& proj);

// Ячейка точки мира — та же арифметика, что в шейдере (для тестов).
int ClusterCellOf(const LightClusterData& data, const glm::vec3& worldPos);

// Доля полусферы, которую закрывает прямоугольник (форм-фактор Ламберта):
// освещённость точки P с нормалью N от панели яркости L равна L·π·F. Копия
// шейдерной AreaFormFactor — для проверки знака и величины без видеокарты.
float AreaFormFactor(const glm::vec3& P, const glm::vec3& N, const AreaLight& light);

// GPU-сторона: текстура данных кадра. По одной на батч (то есть на вид):
// у вьюпорта, окон раскладки и панели Game свои камеры, и общий список ламп по
// ячейкам был бы списком чужой пирамиды.
class LightClusters {
public:
    LightClusters();
    ~LightClusters();
    // Разложить и залить. Дешёвый повтор: текстура пересоздаётся только при
    // росте, иначе перезаливается на месте.
    void Build(const LightingEnvironment& env, const glm::mat4& view, const glm::mat4& proj);
    // Привязать к юниту и выставить uniform'ы. Шейдер без кластеров (их ещё не
    // строили) получает «выключено» и читает свет массивами, как раньше.
    void Upload(Shader& shader) const;
    const LightClusterData& Data() const { return m_data; }
    bool Ready() const { return m_texture != nullptr && m_data.LightCount > 0; }

private:
    LightClusterData m_data;
    std::unique_ptr<sage::rhi::Texture2D> m_texture;
    int m_rows = 0;
};

// Выключить кластеры у шейдера (юнит сэмплера ставится всё равно — иначе он
// остался бы на нулевом вместе с albedo, а это конфликт типов на одном юните).
void DisableLightClusters(Shader& shader);

} // namespace sage::render
