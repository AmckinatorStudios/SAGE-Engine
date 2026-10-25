#include "SelfTestCommon.h"

#include "panels/GIBakeSection.h"

// ЗАПЕКАНИЕ ОСВЕЩЕНИЯ из редактора: разметка статики по правилу (динамика —
// мимо), фоновое запекание, результат на сцене, статус «устарело» после сдвига
// статики и очистка. Свет проверяется по месту: пол у освещённой красной стены
// краснее, чем вдали от неё, — так ловится развёртка, которая читает тексели
// не той точки.
bool EditorLayer::SelfTestGIBake() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: gi-bake: " << why;
        ok = false;
    };
    Scene& scene = *m_scene;
    std::vector<int> made;
    // В стороне от остальной сцены самопроверки: её объекты у начала координат
    // заслоняли бы и подкрашивали пол постановки.
    const glm::vec3 origin(30.0f, 0.0f, 30.0f);
    auto box = [&](const char* name, glm::vec3 pos, glm::vec3 scale, glm::vec3 color) {
        GameObject o = CreatePrimitiveEntity(name, MeshRef::Type::Cube);
        o.GetTransform().Position = origin + pos;
        o.GetTransform().Scale = scale;
        o.Renderer().Color = color;
        made.push_back(o.Id());
        return o;
    };
    // Прочую статику сцены снимаем на время шага: размечает он сам.
    std::vector<entt::entity> hadStatic;
    for (auto e : scene.Registry().view<GIStaticComponent>()) hadStatic.push_back(e);
    for (entt::entity e : hadStatic) scene.Registry().remove<GIStaticComponent>(e);

    GameObject floor = box("GI Bake Floor", {0.0f, -0.05f, 0.0f}, {12.0f, 0.1f, 12.0f}, {0.6f, 0.6f, 0.6f});
    GameObject wall = box("GI Bake Wall", {1.5f, 1.5f, 0.0f}, {0.2f, 3.0f, 6.0f}, {0.9f, 0.05f, 0.05f});
    GameObject ball = box("GI Bake Dynamic", {-2.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f});
    scene.Registry().emplace<RigidBodyComponent>(ball.Entity());

    const int marked = GIBakeSection::MarkStatic(scene);
    if (marked < 2) fail("разметка не пометила пол и стену (" + std::to_string(marked) + ")");
    if (scene.Registry().all_of<GIStaticComponent>(ball.Entity())) fail("динамическое тело помечено статикой");

    GIBakeSection section;
    if (!section.Start(*this)) fail("запекание не запустилось");
    section.Wait();
    if (!section.Poll(*this)) fail("готовое запекание не применилось к сцене");
    if (GIBakeSection::SceneStatus(scene) != GIBakeSection::Status::Baked)
        fail("после запекания статус не «запечено»");

    // Свет лёг на место: у стены пол краснее, чем в дальнем углу.
    if (scene.GI && scene.GI->Baked) {
        auto it = scene.GI->Entities.find(floor.Id());
        if (it == scene.GI->Entities.end()) {
            fail("у пола нет лайтмапы");
        } else {
            const sage::gi::LightmapPage& page = scene.GI->Pages[it->second.Page];
            const glm::mat4 world = scene.WorldMatrix(floor.Entity());
            auto redness = [&](float X, float Z) {
                for (size_t t = 0; t + 2 < it->second.Mesh.Indices.size(); t += 3) {
                    const Vertex* v[3];
                    glm::vec3 w[3];
                    bool top = true;
                    for (int k = 0; k < 3; ++k) {
                        v[k] = &it->second.Mesh.Vertices[it->second.Mesh.Indices[t + k]];
                        w[k] = glm::vec3(world * glm::vec4(v[k]->Position, 1.0f));
                        top = top && v[k]->Normal.y > 0.5f;
                    }
                    if (!top) continue;
                    const float den = (w[1].z - w[2].z) * (w[0].x - w[2].x) + (w[2].x - w[1].x) * (w[0].z - w[2].z);
                    const float b0 = ((w[1].z - w[2].z) * (X - w[2].x) + (w[2].x - w[1].x) * (Z - w[2].z)) / den;
                    const float b1 = ((w[2].z - w[0].z) * (X - w[2].x) + (w[0].x - w[2].x) * (Z - w[2].z)) / den;
                    if (b0 < 0.0f || b1 < 0.0f || b0 + b1 > 1.0f) continue;
                    const glm::vec2 uv = v[0]->TexCoords2 * b0 + v[1]->TexCoords2 * b1 +
                                         v[2]->TexCoords2 * (1.0f - b0 - b1);
                    const int tx = std::clamp((int)(uv.x * page.Size), 0, page.Size - 1);
                    const int ty = std::clamp((int)(uv.y * page.Size), 0, page.Size - 1);
                    const glm::vec3 c = page.Texels[(size_t)ty * page.Size + tx];
                    return c.r - c.g;
                }
                return 0.0f;
            };
            // С обеих сторон стены: какая из граней освещена, решает солнце сцены.
            const float nearWall = std::max(redness(origin.x + 1.0f, origin.z), redness(origin.x + 2.0f, origin.z));
            const float far = redness(origin.x - 5.0f, origin.z - 5.0f);
            LOG_INFO("Editor") << "SELFTEST: gi-bake: краснота пола у стены " << nearWall << ", вдали " << far;
            if (!(nearWall > far + 0.1f)) fail("отсвет красной стены не лёг на пол у её основания");
        }
    }

    // Сдвинули статику — запечённое устарело, и об этом сказано.
    wall.GetTransform().Position.x += 1.0f;
    if (GIBakeSection::SceneStatus(scene) != GIBakeSection::Status::Stale)
        fail("сдвиг статики не сделал запечённое устаревшим");

    scene.GI.reset();
    for (int id : made) scene.RemoveObject(id);
    for (entt::entity e : hadStatic)
        if (scene.Registry().valid(e)) scene.Registry().emplace_or_replace<GIStaticComponent>(e);
    if (ok) LOG_INFO("Editor") << "SELFTEST: gi-bake OK";
    return ok;
}
