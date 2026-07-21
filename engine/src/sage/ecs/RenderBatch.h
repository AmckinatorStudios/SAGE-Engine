#pragma once
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include "sage/render/Mesh.h"

class Scene;
struct LightingEnvironment;
struct Material;
namespace sage::gi { struct GIState; }

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
    // LmPage >= 0 — сущность лайтмапнута (страница атласа GI).
    struct TexturedItem { Mesh* Mesh_; glm::mat4 Model; const Material* Mat; int LmPage; };

    // Кандидат кадра: заполняется ПОСЛЕДОВАТЕЛЬНЫМ проходом по реестру (чтение
    // ECS не потокобезопасно на структурные правки), затем ПАРАЛЛЕЛЬНО
    // проверяется на видимость (frustum) — каждый поток пишет только свой
    // Visible, читая общий frustum/bounds меша только на чтение. Слияние в
    // бакеты — снова последовательно, в исходном порядке (детерминизм).
    struct CullItem {
        Mesh* Mesh_;
        glm::mat4 Model;
        const Material* Mat;
        glm::vec3 Color;
        int LmPage;     // страница лайтмапы (-1 — не запечена)
        bool Textured;
        bool Visible;
    };

    // Инстанс-группа одного меша. У запечённой статики меш уникален для
    // сущности (свои лайтмап-UV), так что LmPage один на группу.
    struct Group {
        std::vector<MeshInstance> Instances;
        int LmPage = -1;
    };

    // Сбор видимых сущностей: flat (без карт) — в m_groups по мешу (инстансинг),
    // текстурные (с картами) — в m_textured. cullMatrix — фрустум камеры/света.
    void CollectVisible(Scene& scene, const glm::mat4& cullMatrix);

    std::unordered_map<Mesh*, Group> m_groups; // flat-инстансы по мешу
    std::vector<TexturedItem> m_textured;                          // текстурные (индивидуально)
    std::vector<CullItem> m_cull;                                  // кандидаты кадра (переиспользуется)
    // Мировые матрицы кадра (Scene::ComputeWorldMatrices) — один O(n)-проход
    // на сбор вместо рекурсивного WorldMatrix на каждую сущность (см. Scene.h).
    // Переиспользуется между кадрами, чтобы не переаллоцировать хэш-таблицу.
    std::unordered_map<entt::entity, glm::mat4> m_worldCache;
    RenderStats m_stats;
};

} // namespace sage::ecs
