#include "SelfTestCommon.h"

#include "sage/ecs/LightSystem.h"
#include "sage/render/PostProcessComponent.h"

// ---------------------------------------------------------------------------
// СЦЕНА-ФИКСТУРА САМОПРОВЕРКИ.
//
// Пользовательского «демо-шаблона» в редакторе больше нет: новый проект всегда
// пустой. Но самопроверка, headless-снимки и smoke-тест проверяют материалы,
// свет, камеру, выделение и Play на НАСТОЯЩЕЙ сцене, и пустая для этого не
// годится. Поэтому прежняя демо-сцена живёт здесь — как данные проверки, а не
// как функция редактора: человек её не видит, её создают только хуки
// SAGE_EDITOR_* (см. EditorLayer_StartupHooks.cpp) и SelfTestRun.cpp.
// ---------------------------------------------------------------------------

namespace {

// Материал фикстуры — ФАЙЛОМ в assets/materials, а не полем Color: проверки
// материалов (перезагрузка, правка файла) идут по тому же пути, что у человека.
// Без проекта писать некуда — тогда объект остаётся при своём цвете.
void PaintFixture(Project& project, GameObject object, const std::string& name,
                  const glm::vec3& albedo, float metallic, float roughness) {
    MeshRendererComponent& mr = object.Renderer();
    std::string ref;
    if (project.Loaded()) {
        std::error_code ec;
        const fs::path dir = project.AssetsDir() / "materials";
        fs::create_directories(dir, ec);
        Material mat;
        mat.Albedo = albedo;
        mat.Metallic = metallic;
        mat.Roughness = roughness;
        const fs::path path = dir / (name + ".sagemat");
        if (!ec) mat.SaveToFile(sage::PathToUtf8(path));
        // Ссылка ОТНОСИТЕЛЬНАЯ (assets/materials/...), как у человека.
        if (fs::exists(path, ec)) ref = project.AssetRef(path);
    }
    if (ref.empty()) {
        mr.Color = albedo;
        return;
    }
    AssignMaterial(mr, ref, ResourceManager::Instance().GetMaterial(ref));
}

} // namespace

void EditorLayer::BuildFixtureScene() {
    NewScene();
    {
        // Скайбокс включён по умолчанию — сцена сразу с атмосферным фоном.
        m_scene->Lighting.Skybox.Enabled = true;

        // Цвет — из материала-файла (см. PaintFixture); у металла (Tower)
        // другая шероховатость, чтобы проверки видели разные поверхности.
        struct Def {
            const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale;
            float metallic; float roughness;
        };
        Def defs[] = {
            {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}, 0.0f, 0.85f},
            {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Tower",      {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}, 1.0f, 0.25f},
        };
        for (const Def& d : defs) {
            GameObject obj = CreateCubeEntity(d.name);
            obj.GetTransform().Position = d.pos;
            obj.GetTransform().Scale = d.scale;
            PaintFixture(m_project, obj, d.name, d.color, d.metallic, d.roughness);
        }

        // Криволинейные примитивы — витрина форм И проверка аутлайна выделения:
        // кайма строится из СИЛУЭТА реального меша, поэтому одинаково точна для
        // сферы/цилиндра/конуса, а не только для боксов.
        struct Prim { const char* name; MeshRef::Type type; glm::vec3 pos; glm::vec3 color; };
        Prim prims[] = {
            {"Sphere",   MeshRef::Type::Sphere,   {-3.0f, 0.5f, 1.8f}, {0.85f, 0.55f, 0.25f}},
            {"Cylinder", MeshRef::Type::Cylinder, {-1.5f, 0.5f, 2.2f}, {0.55f, 0.35f, 0.80f}},
            {"Cone",     MeshRef::Type::Cone,     {1.5f,  0.5f, 2.2f}, {0.30f, 0.70f, 0.70f}},
        };
        for (const Prim& p : prims) {
            GameObject obj = CreatePrimitiveEntity(p.name, p.type);
            obj.GetTransform().Position = p.pos;
            PaintFixture(m_project, obj, p.name, p.color, 0.0f, 0.45f);
        }

        // Свет и камера — пустыми объектами (CreateEmptyObject): у них нет
        // меша. Игровая камера стоит НЕ как орбитальная камера редактора —
        // так видно, что панель Game показывает свою камеру, а не вьюпорт.
        GameObject camObj = m_scene->CreateEmptyObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.5f, 6.5f};
        camObj.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f}; // чуть вниз, вдоль -Z
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());
        // Пост-обработка кадра живёт на камере (sage/render/PostProcessComponent.h).
        m_scene->Registry().emplace<sage::render::PostProcessComponent>(camObj.Entity());

        // Солнце — такая же сущность, как всё остальное. Раньше его роль играли
        // три поля в настройках сцены; теперь его видно в иерархии, можно
        // повернуть гизмо и увидеть, как поехали тени.
        sage::ecs::CreateSunEntity(*m_scene);

        // Тёплая лампа — демонстрация точечного света-сущности (LightComponent).
        GameObject lamp = m_scene->CreateEmptyObject("Lamp");
        lamp.GetTransform().Position = {2.4f, 1.6f, 1.8f};
        m_scene->Registry().emplace<LightComponent>(lamp.Entity());

        // Прожектор сверху — демонстрация конусного света (Spot): смотрит вниз
        // (поворот -90° по X направляет «вперёд» -Z в -Y), кладёт круг света
        // на кубы и пол.
        GameObject spot = m_scene->CreateEmptyObject("Spotlight");
        spot.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        spot.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f};
        LightComponent spotLc;
        spotLc.Kind = LightComponent::Type::Spot;
        spotLc.Color = {0.55f, 0.7f, 1.0f};
        spotLc.Intensity = 4.0f;
        spotLc.Range = 12.0f;
        spotLc.InnerConeDeg = 18.0f;
        spotLc.OuterConeDeg = 30.0f;
        m_scene->Registry().emplace<LightComponent>(spot.Entity(), spotLc);

    }

    {
        // Что-то выбрано сразу — гизмо видно, Inspector не пустой. Выбираем
        // криволинейный примитив: сразу демонстрирует аутлайн на изогнутом
        // силуэте (кайма строится из силуэта меша — точна для любой формы).
        GameObject sel = m_scene->FindByName("Cone");
        if (!sel.Valid()) sel = m_scene->FindByName("Green Cube");
        if (sel.Valid()) m_selection.SetPrimary(sel.Id());
    }
    UpdateWindowTitle();
}

bool EditorLayer::CreateFixtureProject(const std::string& dir, const std::string& name,
                                       std::string& err) {
    if (!CreateProject(dir, name, err)) return false;
    BuildFixtureScene();
    const fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath)) {
        err = "Failed to write the fixture scene: " + sage::PathToUtf8(scenePath);
        return false;
    }
    return true;
}
