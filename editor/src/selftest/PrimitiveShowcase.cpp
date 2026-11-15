// ---------------------------------------------------------------------------
// ВИТРИНА ПРИМИТИВОВ для снимков (SAGE_EDITOR_PRIMITIVES=low|high[,wire]) —
// проверочный хук, а не возможность редактора: в меню его нет.
//
// Шесть процедурных форм в ряд с одной детализацией — низкой (видны плоские
// грани и редкая сетка) или высокой (гладко, плотно); «wire» — каркасом,
// чтобы на снимке была видна сама сетка. Форма у всех одного масштаба:
// размер — Transform → Scale, детализация на него не влияет.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include <vector>

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"

void EditorLayer::RunPrimitiveShowcaseHook(const std::string& mode) {
    const bool high = mode.find("high") != std::string::npos;
    // Убираем демо-объекты с мешем, кроме пола: на их месте встанет витрина.
    std::vector<int> drop;
    m_scene->Registry().view<IdComponent, MeshRendererComponent, NameComponent>().each(
        [&](const IdComponent& id, const MeshRendererComponent&, const NameComponent& n) {
            if (n.Name != "Ground") drop.push_back(id.Id);
        });
    for (int id : drop) m_scene->RemoveObject(id);

    struct Item {
        const char* Name;
        MeshRef::Type Type;
    };
    const Item items[6] = {{"Cube", MeshRef::Type::Cube},         {"Sphere", MeshRef::Type::Sphere},
                           {"Plane", MeshRef::Type::Plane},       {"Cylinder", MeshRef::Type::Cylinder},
                           {"Cone", MeshRef::Type::Cone},         {"Capsule", MeshRef::Type::Capsule}};
    for (int i = 0; i < 6; ++i) {
        GameObject g = CreatePrimitiveEntity(items[i].Name, items[i].Type);
        if (!g.Valid()) continue;
        Transform& t = g.GetTransform();
        t.Position = glm::vec3(-3.75f + 1.5f * (float)i, items[i].Type == MeshRef::Type::Capsule ? 1.0f : 0.55f, 0.0f);
        if (items[i].Type == MeshRef::Type::Plane) {
            t.Position.y = 0.55f;
            t.Rotation = glm::vec3(60.0f, 0.0f, 0.0f);   // наклон к камере: сетку видно
        }
        sage::render::PrimitiveParams& p = g.Renderer().Ref.prim;
        if (high) {
            p.SubX = p.SubY = p.SubZ = 4;
            p.Segments = 48;
            p.Rings = items[i].Type == MeshRef::Type::Capsule ? 12 : 32;
            p.HeightSegments = 4;
            p.Height = 3.0f;
        } else {
            p.SubX = p.SubY = p.SubZ = 1;
            p.Segments = 8;
            p.Rings = items[i].Type == MeshRef::Type::Capsule ? 2 : 6;
            p.HeightSegments = 1;
            p.Height = 1.5f;
            p.Flat = true;
        }
        if (items[i].Type == MeshRef::Type::Capsule) t.Position.y = (high ? 3.0f : 1.5f) * 0.5f + 0.05f;
        g.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(g.Renderer().Ref.type, p);
    }
    m_camera.Position = {0.0f, 2.6f, 6.5f};
    m_camera.Yaw = -90.0f;
    m_camera.Pitch = -14.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);
    if (mode.find("wire") != std::string::npos) m_tools.RenderMode = EditorRenderMode::Wireframe;
    LOG_INFO("Editor") << "Витрина примитивов: " << mode;
}
