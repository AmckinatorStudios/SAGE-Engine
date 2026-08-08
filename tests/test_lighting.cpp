// Освещение: солнце как СУЩНОСТЬ, раздача мест в атласе теней и разложение
// куба точечного света на грани.
//
// ЗАЧЕМ ЭТОТ ФАЙЛ. У освещения три места, где ошибка не видна ни сборке, ни
// глазу, ни эталонным кадрам:
//
//   • сбор кадра. Направленный свет-сущность обязан ПЕРЕКРЫВАТЬ солнце из
//     настроек сцены, а не складываться с ним. Ошибка здесь даёт сцену, которая
//     светит вдвое, — и объяснить это по картинке невозможно;
//
//   • миграция. Старая сцена хранила солнце тремя числами в настройках. Если
//     перенос в сущность потеряет направление или заведёт солнце дважды, сцена
//     просто откроется «не такой», и виноватым будет казаться рендер;
//
//   • грани куба точечного света. Их выбирают ДВЕ независимые реализации: на
//     процессоре строятся матрицы, в шейдере грань ищется по наибольшей
//     компоненте вектора. Разъехались — и тень уезжает на соседнюю грань, то
//     есть ложится поперёк комнаты. Залезть в GLSL тест не может, поэтому здесь
//     проверяется зеркало шейдерной формулы против настоящих матриц.
#include "TestFramework.h"

#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/ShadowAtlas.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace {

GameObject MakeLight(Scene& scene, const char* name, LightComponent::Type kind) {
    GameObject obj = scene.CreateObject(name);
    LightComponent& lc = obj.Registry()->emplace<LightComponent>(obj.Entity());
    lc.Kind = kind;
    return obj;
}

} // namespace

// --- Солнце как сущность ----------------------------------------------------

TEST(Lighting_directional_entity_overrides_the_scene_sun) {
    Scene scene("Солнце");
    scene.Lighting.Sun.Direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
    scene.Lighting.Sun.Intensity = 1.0f;
    scene.Lighting.Sun.Color = glm::vec3(1.0f);

    GameObject sun = MakeLight(scene, "Солнце", LightComponent::Type::Directional);
    sun.GetTransform().Rotation = glm::vec3(-90.0f, 0.0f, 0.0f); // «вперёд» строго вниз
    LightComponent& lc = sun.Registry()->get<LightComponent>(sun.Entity());
    lc.Intensity = 3.0f;
    lc.Color = glm::vec3(0.5f, 0.25f, 0.125f);

    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    // ПЕРЕКРЫЛО, а не сложилось: яркость ровно сущности, а не 1 + 3.
    CHECK_NEAR(env.Sun.Intensity, 3.0f, 1e-4);
    CHECK_NEAR(env.Sun.Color.r, 0.5f, 1e-4);
    CHECK_NEAR(env.Sun.Direction.y, -1.0f, 1e-3);
    // И не попало в списки локальных источников: направленный свет не точечный.
    CHECK_EQ((int)env.PointLights.size(), 0);
    CHECK_EQ((int)env.SpotLights.size(), 0);
}

TEST(Lighting_scene_sun_survives_without_a_directional_entity) {
    // Код, собирающий сцену без редактора (игры, превью, тесты), ставит свет
    // одной строкой — и обязан продолжать работать.
    Scene scene("БезСолнца");
    scene.Lighting.Sun.Intensity = 1.75f;
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    CHECK_NEAR(env.Sun.Intensity, 1.75f, 1e-4);
}

TEST(Lighting_second_directional_light_does_not_add_a_second_sun) {
    Scene scene("ДваСолнца");
    GameObject first = MakeLight(scene, "Первое", LightComponent::Type::Directional);
    first.Registry()->get<LightComponent>(first.Entity()).Intensity = 2.0f;
    GameObject second = MakeLight(scene, "Второе", LightComponent::Type::Directional);
    second.Registry()->get<LightComponent>(second.Entity()).Intensity = 9.0f;

    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    // Шейдер знает ОДНО направленное освещение (и одну каскадную карту под
    // него). Второе не складывается и не заменяет первое молча — оно просто не
    // участвует. Солнцем становится источник с меньшим id, то есть первый в
    // иерархии; полагаться на порядок обхода ECS нельзя — он не обещан.
    CHECK_NEAR(env.Sun.Intensity, 2.0f, 1e-4);

    // И выбор не зависит от того, в каком порядке сущности легли в registry:
    // удаление соседа не должно менять, какое солнце светит.
    scene.RemoveObject(second.Id());
    GameObject third = MakeLight(scene, "Третье", LightComponent::Type::Directional);
    third.Registry()->get<LightComponent>(third.Entity()).Intensity = 5.0f;
    CHECK_NEAR(sage::ecs::CollectLighting(scene).Sun.Intensity, 2.0f, 1e-4);
}

TEST(Lighting_euler_from_forward_is_the_inverse_of_forward_from_euler) {
    // Перенос старого солнца в сущность считает поворот из направления. Если
    // эти две функции не обратны друг другу, мигрированная сцена откроется с
    // солнцем, светящим не туда, — и заметить это можно только по картинке.
    const glm::vec3 dirs[] = {
        glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)),
        glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f)),   // строго вниз (вырожденный случай)
        glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)),    // строго вверх
        glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f)),
        glm::normalize(glm::vec3(0.55f, -0.8f, 0.2f)),
        glm::normalize(glm::vec3(-0.2f, 0.1f, 0.97f)),
    };
    for (const glm::vec3& dir : dirs) {
        const glm::vec3 euler = sage::ecs::EulerFromForward(dir);
        const glm::vec3 back = sage::ecs::ForwardFromEuler(euler);
        CHECK_NEAR(back.x, dir.x, 1e-3);
        CHECK_NEAR(back.y, dir.y, 1e-3);
        CHECK_NEAR(back.z, dir.z, 1e-3);
    }
}

TEST(Lighting_old_scene_sun_migrates_into_an_entity) {
    const char* json = R"({
        "sage_scene_version": 4, "name": "Старая",
        "objects": [ {"id": 1, "name": "Пусто", "mesh": {"type": "none", "path": ""}} ],
        "lighting": { "sun": { "direction": {"x": 0.0, "y": -1.0, "z": 0.0},
                               "color": {"x": 0.25, "y": 0.5, "z": 0.75},
                               "intensity": 2.5 } }
    })";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(json);
    CHECK_TRUE(scene != nullptr);
    if (!scene) return;

    GameObject sun = scene->FindByName("Sun");
    CHECK_TRUE(sun.Valid());
    if (!sun.Valid()) return;
    const LightComponent* lc = sun.Registry()->try_get<LightComponent>(sun.Entity());
    CHECK_TRUE(lc != nullptr);
    if (lc) {
        CHECK_TRUE(lc->Kind == LightComponent::Type::Directional);
        CHECK_NEAR(lc->Intensity, 2.5f, 1e-4);
        CHECK_NEAR(lc->Color.g, 0.5f, 1e-4);
    }

    // Направление сохранилось — уже через поворот сущности.
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    CHECK_NEAR(env.Sun.Direction.y, -1.0f, 1e-3);
    CHECK_NEAR(env.Sun.Intensity, 2.5f, 1e-4);

    // И старое поле обнулено: удалив сущность, человек обязан остаться без
    // солнца, а не увидеть, как оно возвращается из настроек.
    CHECK_NEAR(scene->Lighting.Sun.Intensity, 0.0f, 1e-4);
}

TEST(Lighting_directional_component_round_trips_through_a_scene_file) {
    Scene scene("Сохранение");
    GameObject sun = MakeLight(scene, "Солнце", LightComponent::Type::Directional);
    LightComponent& lc = sun.Registry()->get<LightComponent>(sun.Entity());
    lc.Intensity = 4.25f;
    lc.CastShadows = false;

    const std::string path = "sage_lighting_directional.sage";
    SceneSerializer::Save(scene, path);
    std::unique_ptr<Scene> loaded = SceneSerializer::Load(path);
    std::remove(path.c_str());
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;

    GameObject back = loaded->FindByName("Солнце");
    CHECK_TRUE(back.Valid());
    if (!back.Valid()) return;
    const LightComponent& got = back.Registry()->get<LightComponent>(back.Entity());
    // Тип — самое хрупкое: пока типов было два, он писался как «spot или
    // point», и directional при сохранении превратился бы в точечный.
    CHECK_TRUE(got.Kind == LightComponent::Type::Directional);
    CHECK_NEAR(got.Intensity, 4.25f, 1e-4);
    CHECK_FALSE(got.CastShadows);
}

// --- Грани куба точечного света ---------------------------------------------

TEST(PointShadowFace_matches_the_matrix) {
    // Зеркало шейдерной формулы против НАСТОЯЩИХ матриц граней. Обе стороны
    // читают одну таблицу (CubeFaces), но считают по-разному: матрица — через
    // lookAt и perspective, формула — делением на наибольшую компоненту. Если
    // они разойдутся, тень точечного света ляжет на соседнюю грань.
    const glm::vec3 lightPos(2.0f, 3.0f, -1.0f);
    const float nearZ = 0.05f, farZ = 20.0f;
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, nearZ, farZ);

    const glm::vec3 samples[] = {
        {5.0f, 3.2f, -1.1f},   // +X
        {-4.0f, 3.1f, -0.9f},  // -X
        {2.1f, 9.0f, -1.0f},   // +Y
        {1.9f, -2.0f, -1.2f},  // -Y
        {2.2f, 3.3f, 6.0f},    // +Z
        {1.8f, 2.7f, -8.0f},   // -Z
        {4.0f, 4.4f, 0.9f},    // наискось, чтобы грань выбиралась не тривиально
        {-1.0f, 1.0f, -5.5f},
    };

    int checked = 0;
    for (const glm::vec3& world : samples) {
        const glm::vec3 v = world - lightPos;
        glm::vec2 uv;
        const int face = sage::render::PointFaceUV(v, uv);

        const sage::render::CubeFaceBasis& basis = sage::render::CubeFaces()[face];
        const glm::mat4 view = glm::lookAt(lightPos, lightPos + basis.Forward, basis.Up);
        const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
        CHECK_TRUE(clip.w > 0.0f); // точка обязана быть ПЕРЕД выбранной гранью
        if (clip.w <= 0.0f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;

        CHECK_NEAR(uv.x, ndc.x * 0.5f + 0.5f, 1e-4);
        CHECK_NEAR(uv.y, ndc.y * 0.5f + 0.5f, 1e-4);
        // И записанная глубина считается той же формулой, что в шейдере.
        const float axis = std::max(std::max(std::abs(v.x), std::abs(v.y)), std::abs(v.z));
        CHECK_NEAR(sage::render::PerspectiveDepth01(axis, nearZ, farZ), ndc.z * 0.5f + 0.5f, 1e-4);
        ++checked;
    }
    CHECK_EQ(checked, 8); // проверка того, что проверка что-то проверила
}

TEST(PointShadowFace_covers_every_face) {
    // Шесть направлений по осям обязаны дать шесть РАЗНЫХ граней. Без этого
    // ошибка в таблице (две одинаковые строки) прошла бы предыдущий тест: он
    // сравнивает формулу с матрицей ТОЙ ЖЕ грани, а какая она — не спрашивает.
    const glm::vec3 axes[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    bool seen[6] = {false, false, false, false, false, false};
    for (const glm::vec3& a : axes) {
        glm::vec2 uv;
        const int face = sage::render::PointFaceUV(a, uv);
        CHECK_TRUE(face >= 0 && face < 6);
        if (face >= 0 && face < 6) seen[face] = true;
        // Направление точно в центр грани — центр её плитки.
        CHECK_NEAR(uv.x, 0.5f, 1e-4);
        CHECK_NEAR(uv.y, 0.5f, 1e-4);
    }
    for (bool s : seen) CHECK_TRUE(s);
}

TEST(PerspectiveDepth_spans_the_whole_range) {
    const float n = 0.05f, f = 12.0f;
    CHECK_NEAR(sage::render::PerspectiveDepth01(n, n, f), 0.0f, 1e-4);
    CHECK_NEAR(sage::render::PerspectiveDepth01(f, n, f), 1.0f, 1e-4);
    // Нелинейность — не деталь реализации, а причина, по которой запас по
    // глубине считается из расстояния: середина диапазона по МЕТРАМ лежит
    // далеко за серединой по глубине.
    CHECK_TRUE(sage::render::PerspectiveDepth01((n + f) * 0.5f, n, f) > 0.9f);
}
