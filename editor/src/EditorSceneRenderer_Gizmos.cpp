#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include "EditorSceneRenderer.h"

#include "sage/anim/AnimationComponents.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/Screenshot.h"

#include "sage/ecs/DecalSystem.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/app/Application.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Config.h"
#include "sage/render/DebugView.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/PostFX.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"



// Гизмо невидимых объектов: каркасы камер, света, зондов, коллайдеров.
// Гизмо невидимых/физических сущностей (камера/свет/эмиттер/коллайдер) — всегда
// видны и кликабельны. Накопление в DebugDraw, Flush — у вызывающего.
void EditorSceneRenderer::DrawEntityGizmos(Scene& scene, const std::vector<int>& selection, float gameAspect) {
    auto& reg = scene.Registry();
    auto isSel = [&](int id) {
        return std::find(selection.begin(), selection.end(), id) != selection.end();
    };
    // Камеры: каркас усечённой пирамиды (frustum).
    auto camView = reg.view<CameraComponent, Transform, IdComponent>();
    for (auto e : camView) {
        bool selected = isSel(camView.get<IdComponent>(e).Id);
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.8f, 0.2f) : glm::vec3(0.5f, 0.7f, 0.9f);
        glm::mat4 world = scene.WorldMatrix(e); // мировая (иерархия)
        glm::vec3 wpos = glm::vec3(world[3]);
        glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
        // Верх — тоже из матрицы, как у самой камеры (CameraFrameFor): иначе
        // каркас не показывал крен, и камера в игре смотрела не так, как на
        // гизмо.
        const glm::vec3 up = glm::vec3(world * glm::vec4(0, 1, 0, 0));
        const CameraComponent& cam = camView.get<CameraComponent>(e);
        m_debugDraw->WireFrustum(wpos, fwd, cam.Fov, gameAspect, 0.3f, 2.2f, color, up);
    }
    // Свет-сущности: маркер-сфера в позиции (у выбранного — ярче; крупная зона
    // выбранного примитива рисуется в RenderViewport поверх).
    auto lightView = reg.view<LightComponent, Transform, IdComponent>();
    for (auto e : lightView) {
        bool selected = isSel(lightView.get<IdComponent>(e).Id);
        const LightComponent& lc = lightView.get<LightComponent>(e);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, selected ? 0.30f : 0.25f, glm::vec3(lc.Color) * (selected ? 1.2f : 0.9f), 10);
    }
    // Эмиттеры частиц: маркер в позиции.
    auto fxView = reg.view<ParticleEmitterComponent, Transform, IdComponent>();
    for (auto e : fxView) {
        bool selected = isSel(fxView.get<IdComponent>(e).Id);
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.85f, 0.3f) : glm::vec3(0.9f, 0.6f, 0.3f);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, 0.18f, color, 8);
        m_debugDraw->Axes(glm::translate(glm::mat4(1.0f), wpos), 0.4f);
    }
    // Зонды отражений: сфера-маркер и КОРОБКА ВЛИЯНИЯ. Коробка важнее маркера —
    // именно она отвечает на вопрос «какой зонд подхватит камера здесь», а
    // ответить на него по числам в инспекторе нельзя.
    auto probeView = reg.view<ReflectionProbeComponent, Transform, IdComponent>();
    for (auto e : probeView) {
        const bool selected = isSel(probeView.get<IdComponent>(e).Id);
        const ReflectionProbeComponent& rp = probeView.get<ReflectionProbeComponent>(e);
        const glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        // Не снятый зонд — тускло-красный: «здесь будет отражение, но его ещё
        // нет». Молчащий маркер выглядел бы как рабочий зонд.
        const glm::vec3 color = rp.Dirty ? glm::vec3(0.85f, 0.35f, 0.30f)
                                         : (selected ? glm::vec3(0.45f, 0.95f, 1.0f)
                                                     : glm::vec3(0.30f, 0.65f, 0.75f));
        m_debugDraw->WireSphere(wpos, selected ? 0.34f : 0.26f, color, 12);
        if (selected || rp.Dirty) m_debugDraw->WireBox(wpos, rp.BoxHalfExtents, color * 0.8f);
    }

    // Коллайдеры: каркас формы в МИРОВОМ трансформе сущности.
    //
    // Раньше здесь стояли tr.Position и tr.Scale — то есть ЛОКАЛЬНЫЙ трансформ
    // и ни следа поворота. Получался осевой ящик, который у повёрнутого объекта
    // висит сам по себе, а у дочерней сущности ещё и не в том месте. Это не
    // косметика: гизмо — единственный способ увидеть, где на самом деле форма
    // столкновения, и врущее гизмо хуже отсутствующего — по нему принимают
    // решения.
    auto colView = reg.view<ColliderComponent, Transform, IdComponent>();
    const ColliderGizmoStyle& cg = m_colliderGizmo;
    // Одна форма — каркасом, заливкой или обоими, как выбрал человек (меню «…»
    // вьюпорта). Заливка чуть светлее каркаса: иначе при «заливка + линии»
    // линии тонут в собственной заливке.
    auto shape = [&](sage::physics::ShapeType type, const glm::mat4& frame, const glm::vec3& half,
                     float radius, float halfHeight, const glm::vec3& color, float alphaScale) {
        const glm::vec4 fill(glm::min(color * 1.15f, glm::vec3(1.0f)), cg.FillOpacity * alphaScale);
        const glm::vec3 center(frame[3]);
        switch (type) {
            case sage::physics::ShapeType::Box: {
                const glm::mat4 box = frame * glm::scale(glm::mat4(1.0f), half * 2.0f);
                if (cg.DrawFill()) m_debugDraw->SolidBox(box, fill);
                if (cg.DrawLines()) m_debugDraw->WireBox(box, color);
                break;
            }
            case sage::physics::ShapeType::Sphere:
                // Шар поворачивать незачем — он от поворота не меняется.
                if (cg.DrawFill()) m_debugDraw->SolidSphere(center, radius, fill);
                if (cg.DrawLines()) m_debugDraw->WireSphere(center, radius, color);
                break;
            case sage::physics::ShapeType::Capsule:
                if (cg.DrawFill()) m_debugDraw->SolidCapsule(frame, radius, halfHeight, fill);
                if (cg.DrawLines()) m_debugDraw->WireCapsule(frame, radius, halfHeight, color);
                break;
            default:
                break;   // формы по мешу рисуются рёбрами модели (см. ниже)
        }
    };
    for (auto e : colView) {
        const ColliderComponent& col = colView.get<ColliderComponent>(e);
        bool selected = isSel(colView.get<IdComponent>(e).Id);
        if (cg.OnlySelected && !selected) continue;
        glm::vec3 color = selected ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(0.25f, 0.55f, 0.30f);
        // Зона-триггер — другим цветом: «пропускает сквозь себя» и «стена»
        // выглядят одинаково только до первого разбора, почему игрок прошёл.
        if (const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e); rb && rb->Sensor)
            color = selected ? glm::vec3(1.0f, 0.78f, 0.25f) : glm::vec3(0.75f, 0.55f, 0.20f);

        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 wpos = glm::vec3(world[3]);
        // Масштаб — длины столбцов мировой матрицы: он мог прийти и от родителя.
        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                              glm::length(glm::vec3(world[2])));
        const float uniform = glm::max(scale.x, glm::max(scale.y, scale.z));
        // Мировой поворот без масштаба: масштаб уже учтён в размерах формы, и
        // умножать на него второй раз значило бы раздувать каркас квадратично.
        glm::mat4 rot = world;
        for (int i = 0; i < 3; ++i) {
            const float len = scale[i] > 1e-6f ? scale[i] : 1.0f;
            rot[i] /= len;
        }
        rot[3] = glm::vec4(wpos, 1.0f);

        // Форма по мешу — рёбрами треугольников той модели, из которой физика
        // её строит (своей или указанной в коллайдере). Для оболочки это
        // исходная модель, которую оболочка обтягивает.
        const bool meshShape = col.Shape == sage::physics::ShapeType::ConvexHull ||
                               col.Shape == sage::physics::ShapeType::Mesh;
        if (col.Parts.empty() && meshShape) {
            std::shared_ptr<Mesh> src;
            if (!col.MeshPath.empty()) src = ResourceManager::Instance().GetModel(col.MeshPath);
            else if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) src = mr->MeshPtr;
            const std::vector<Vertex>* verts = src ? src->CpuVertices() : nullptr;
            const std::vector<unsigned int>* idx = src ? src->CpuIndices() : nullptr;
            const glm::vec3 wire = color * (col.Shape == sage::physics::ShapeType::Mesh ? 1.0f : 0.75f);
            // Густую модель — габаритом: десятки тысяч линий каждый кадр
            // превращают вьюпорт в слайд-шоу, а форму всё равно не разглядеть.
            constexpr size_t kMaxWireTriangles = 4000;
            if (verts && idx && idx->size() / 3 <= kMaxWireTriangles) {
                for (size_t t = 0; t + 2 < idx->size(); t += 3) {
                    const unsigned a = (*idx)[t], b = (*idx)[t + 1], c = (*idx)[t + 2];
                    if (a >= verts->size() || b >= verts->size() || c >= verts->size()) continue;
                    const glm::vec3 pa(world * glm::vec4((*verts)[a].Position, 1.0f));
                    const glm::vec3 pb(world * glm::vec4((*verts)[b].Position, 1.0f));
                    const glm::vec3 pc(world * glm::vec4((*verts)[c].Position, 1.0f));
                    m_debugDraw->Line(pa, pb, wire);
                    m_debugDraw->Line(pb, pc, wire);
                    m_debugDraw->Line(pc, pa, wire);
                }
            } else if (src) {
                const glm::vec3 lo = src->BoundsMin(), hi = src->BoundsMax();
                m_debugDraw->WireBox(world * glm::translate(glm::mat4(1.0f), (lo + hi) * 0.5f) *
                                         glm::scale(glm::mat4(1.0f), glm::max(hi - lo, glm::vec3(1e-3f))),
                                     wire);
            }
        }

        // Одиночная форма — только без частей: с частями физика её поля не
        // читает, и рисовать её значит показывать форму, которой нет.
        if (col.Parts.empty() && !meshShape)
            shape(col.Shape, rot * glm::translate(glm::mat4(1.0f), col.Center * scale), col.HalfExtents * scale,
                  col.Radius * uniform, col.HalfHeight * scale.y, color, selected ? 1.0f : 0.7f);

        // Составная форма: каждая часть в своём локальном смещении и повороте.
        // Без этого у молотка или Т-образной детали гизмо показывало ОДИН ящик
        // по полям, которые физика в этом случае вообще игнорирует.
        for (const ColliderComponent::Part& p : col.Parts) {
            const glm::mat4 partWorld =
                rot * glm::translate(glm::mat4(1.0f), p.Offset * scale) *
                sage::RotationMatrixFromEulerDegrees(p.EulerDeg);   // как у физики (EulerToQuat)
            shape(p.Shape, partWorld, p.HalfExtents * scale, p.Radius * uniform,
                  p.HalfHeight * scale.y, color * 0.8f, selected ? 1.0f : 0.7f);
        }
    }
}
