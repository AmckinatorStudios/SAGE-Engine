#pragma once
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/Mesh.h"

class Scene;
struct LightingEnvironment;
struct Material;

// ---------------------------------------------------------------------------
// RenderBatch — масштабируемый проход статических мешей: отсечение по фрустуму
// + инстансный батчинг. Вместо «один draw call на сущность» собирает видимые
// сущности, группирует по мешу и рисует каждую группу ОДНИМ инстансным вызовом
// (per-instance поток — модельная матрица + цвет). Так число draw call'ов
// зависит от числа РАЗНЫХ мешей, а не от числа сущностей, а стоимость — от
// видимых, а не от всех. Освещение идентично статическому lit.frag.
//
// Владелец (слой редактора/рантайма/игры) держит один экземпляр; шейдеры —
// внутренние (встроенные), создаются лениво.
// ---------------------------------------------------------------------------
namespace sage::ecs {

struct RenderStats {
    int Total = 0;    // всего рендерящихся сущностей
    int Drawn = 0;    // прошли фрустум-отсечение (реально нарисованы)
    int Culled = 0;   // отсечены фрустумом
    int Batches = 0;  // инстанс-групп = draw call'ов геометрии
};

class RenderBatch {
public:
    // Цветной проход: отсечение по камере + инстансная отрисовка с полным
    // освещением. shadingMode: 0 lit / 1 unlit / 2 normals. Возвращает статистику.
    RenderStats RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                            const glm::vec3& viewPos, const LightingEnvironment& env,
                            const glm::mat4& lightMatrix, unsigned int shadowMap,
                            bool shadowsEnabled, int shadingMode);

    // Depth-проход для карты теней: отсечение по фрустуму СВЕТА + инстансная
    // отрисовка только глубины (без освещения).
    void RenderDepth(Scene& scene, const glm::mat4& lightMatrix);

    const RenderStats& LastStats() const { return m_stats; }

private:
    // Одна видимая текстурная сущность (с albedo/normal-картами) — рисуется
    // индивидуально текстурным PBR-шейдером (нельзя инстансить с текстурами).
    struct TexturedItem { Mesh* Mesh_; glm::mat4 Model; const Material* Mat; };

    // Сбор видимых сущностей: flat (без карт) — в m_groups по мешу (инстансинг),
    // текстурные (с картами) — в m_textured. cullMatrix — фрустум камеры/света.
    void CollectVisible(Scene& scene, const glm::mat4& cullMatrix);

    std::unordered_map<Mesh*, std::vector<MeshInstance>> m_groups; // flat-инстансы по мешу
    std::vector<TexturedItem> m_textured;                          // текстурные (индивидуально)
    RenderStats m_stats;
};

} // namespace sage::ecs
