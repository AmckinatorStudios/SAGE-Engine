#include "TestGameLayer.h"
#include "GameIcons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include "sage/core/Math.h"

#include "sage/core/Application.h"
#include "sage/core/Paths.h"
#include "sage/core/Config.h"
#include "sage/core/Log.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/physics/Ragdoll.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/render/Screenshot.h"
#include "sage/render/Texture.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/audio/AudioEngine.h"

constexpr glm::vec3 TestGameLayer::kPlayerHalf;

TestGameLayer::TestGameLayer() : sage::Layer("TestGame") {}
TestGameLayer::~TestGameLayer() = default;

// ============================================================================
//  Построение мира
// ============================================================================

GameObject TestGameLayer::SpawnBox(Scene& scene, const std::string& name, glm::vec3 pos,
                                   glm::vec3 scale, glm::vec3 color, bool collider) {
    GameObject obj = scene.CreateObject(name);
    obj.GetTransform().Position = pos;
    obj.GetTransform().Scale = scale;
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    mr.MeshPtr = ResourceManager::Instance().GetCube();
    mr.Color = color;
    if (collider) scene.Registry().emplace<StaticColliderComponent>(obj.Entity());
    return obj;
}

// Демонстрация движковой физики в живой игре: даём полу статическое тело, затем
// роняем на него башенку динамических ящиков. Их позиции ведёт PhysicsScene
// (Jolt по умолчанию) — падение/укладка стресс-тестят физику 400 кадров в CI.
void TestGameLayer::SpawnPhysicsProps(Scene& scene, glm::vec3 origin) {
    // Пол уже есть как визуальный бокс — добавляем ему статическое физ-тело,
    // чтобы ящики было на что приземляться (верх пола на y=0).
    GameObject floor = scene.FindByName("Floor");
    if (floor.Valid()) {
        scene.Registry().emplace_or_replace<RigidBodyComponent>(
            floor.Entity(), RigidBodyComponent{sage::physics::BodyType::Static});
        scene.Registry().emplace_or_replace<ColliderComponent>(floor.Entity());
    }

    // Башенка из 5 ящиков со случайным горизонтальным сдвигом — красиво
    // рассыпается при падении.
    const glm::vec3 kColors[] = {
        {0.90f, 0.45f, 0.30f}, {0.35f, 0.70f, 0.85f}, {0.85f, 0.80f, 0.35f},
        {0.55f, 0.80f, 0.45f}, {0.80f, 0.45f, 0.75f},
    };
    for (int i = 0; i < 5; ++i) {
        float jitter = 0.12f * (float)((i % 2) ? 1 : -1);
        glm::vec3 pos = origin + glm::vec3(jitter, 1.0f + 1.05f * (float)i, jitter * 0.5f);
        GameObject box = SpawnBox(scene, "PhysBox " + std::to_string(i + 1), pos,
                                  {0.8f, 0.8f, 0.8f}, kColors[i], false);
        scene.Registry().emplace<RigidBodyComponent>(
            box.Entity(), RigidBodyComponent{sage::physics::BodyType::Dynamic, 1.0f, 0.6f, 0.15f});
        scene.Registry().emplace<ColliderComponent>(box.Entity());
    }

    // Тряпичная кукла (кости-капсулы + суставы Cone/Hinge) — падает и
    // складывается рядом с башней. Демонстрирует соединения и сочленённые тела.
    sage::physics::BuildRagdoll(scene, origin + glm::vec3(3.0f, 5.0f, 0.0f), 1.0f);

    // Составное (compound) тело: «гантель» — два сферических груза на оси-боксе,
    // один твёрдый объект из нескольких форм. Падает как единое целое.
    {
        // Uniform scale: у compound-коллайдера части задают геометрию сами
        // (неравномерный масштаб сущности исказил бы дочерние формы).
        GameObject dumbbell = SpawnBox(scene, "Dumbbell", origin + glm::vec3(-3.0f, 3.0f, 0.0f),
                                       {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.55f}, false);
        scene.Registry().emplace<RigidBodyComponent>(
            dumbbell.Entity(), RigidBodyComponent{sage::physics::BodyType::Dynamic, 2.0f, 0.6f, 0.1f});
        ColliderComponent col;
        ColliderComponent::Part bar;   bar.Shape = sage::physics::ShapeType::Box;
        bar.HalfExtents = {0.8f, 0.12f, 0.12f};
        ColliderComponent::Part left;  left.Shape = sage::physics::ShapeType::Sphere;
        left.Radius = 0.3f; left.Offset = {-0.8f, 0.0f, 0.0f};
        ColliderComponent::Part right; right.Shape = sage::physics::ShapeType::Sphere;
        right.Radius = 0.3f; right.Offset = {0.8f, 0.0f, 0.0f};
        col.Parts = {bar, left, right};
        scene.Registry().emplace<ColliderComponent>(dumbbell.Entity(), col);
    }
}

namespace {

// Игрок: сущность без меша (MeshPtr == nullptr не рендерится) — носитель
// HealthComponent в каждой комнате. Позицией управляет C++ (m_playerPos).
void SpawnPlayer(Scene& scene, glm::vec3 spawn) {
    GameObject player = scene.CreateObject("Player");
    player.GetTransform().Position = spawn;
    scene.Registry().emplace<HealthComponent>(player.Entity());
}

void SpawnPickup(Scene& scene, const std::string& name, glm::vec3 pos) {
    GameObject coin = scene.CreateObject(name);
    coin.GetTransform().Position = pos;
    coin.GetTransform().Scale = {0.35f, 0.35f, 0.35f};
    MeshRendererComponent& mr = coin.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    mr.MeshPtr = ResourceManager::Instance().GetCube();
    mr.Color = {0.95f, 0.83f, 0.25f};
    scene.Registry().emplace<PickupComponent>(coin.Entity());
    scene.Registry().emplace<ScriptComponent>(coin.Entity(),
        ScriptComponent{"assets/scripts/pickup_bob.lua"});
}

void SpawnPatrol(Scene& scene, const std::string& name, glm::vec3 a, glm::vec3 b, float speed) {
    GameObject enemy = scene.CreateObject(name);
    enemy.GetTransform().Position = a;
    enemy.GetTransform().Scale = {0.8f, 1.6f, 0.8f};
    MeshRendererComponent& mr = enemy.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    mr.MeshPtr = ResourceManager::Instance().GetCube();
    mr.Color = {0.85f, 0.25f, 0.25f};
    scene.Registry().emplace<PatrolComponent>(enemy.Entity(), PatrolComponent{a, b, speed});
}

void SpawnPortal(Scene& scene, const std::string& name, glm::vec3 pos,
                 const std::string& target, glm::vec3 spawnPos) {
    GameObject beacon = scene.CreateObject(name);
    beacon.GetTransform().Position = pos;
    beacon.GetTransform().Scale = {1.0f, 2.6f, 1.0f};
    MeshRendererComponent& mr = beacon.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    mr.MeshPtr = ResourceManager::Instance().GetCube();
    mr.Color = {0.4f, 0.6f, 1.0f};
    scene.Registry().emplace<PortalComponent>(beacon.Entity(), PortalComponent{target, spawnPos});
    scene.Registry().emplace<ScriptComponent>(beacon.Entity(),
        ScriptComponent{"assets/scripts/beacon_pulse.lua"});
}

// Собирает PBR-материал из директории с полным набором карт (albedo/normal/
// metallic/roughness/ao — по ключевым словам в именах файлов, регистронезависимо).
// Позволяет протестить движок на РЕАЛЬНОМ материале (например, скачанном PBR-сете)
// без коммита тяжёлых текстур в репозиторий: путь задаётся env SAGE_TESTGAME_PBR_DIR.
// nullptr, если директории нет или в ней не найден albedo.
static std::shared_ptr<Material> LoadPbrSetFromDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return nullptr;

    auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    // Ищет первый файл, чьё имя содержит любое из ключевых слов (но не anti — чтобы
    // «normal» не хватал «...normal-dx», когда нужен «-ogl»: предпочтение отдаётся ogl).
    auto find = [&](std::initializer_list<const char*> keys, const char* prefer = nullptr) -> std::string {
        std::string best, preferred;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string name = lower(e.path().filename().string());
            std::string ext = lower(e.path().extension().string());
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga" && ext != ".bmp") continue;
            for (const char* k : keys)
                if (name.find(k) != std::string::npos) {
                    if (prefer && name.find(prefer) != std::string::npos) preferred = e.path().string();
                    else if (best.empty()) best = e.path().string();
                }
        }
        return !preferred.empty() ? preferred : best;
    };

    std::string albedo = find({"albedo", "basecolor", "base_color", "diffuse"});
    if (albedo.empty()) return nullptr; // без базового цвета материал не собрать

    auto& rm = ResourceManager::Instance();
    auto mat = std::make_shared<Material>();
    mat->Albedo = glm::vec3(1.0f);
    mat->Metallic = 1.0f;  // карта (если есть) даёт итог; фактор 1 не гасит её
    mat->Roughness = 1.0f;
    mat->TexturePath = albedo;
    mat->NormalMapPath = find({"normal"}, /*prefer=*/"ogl"); // OpenGL-нормали (зелёный вверх)
    mat->MetallicMapPath = find({"metallic", "metalness"});
    mat->RoughnessMapPath = find({"roughness", "rough"});
    mat->AOMapPath = find({"ao", "occlusion", "ambientocclusion"});
    rm.ResolveMaterialTextures(*mat);
    if (!mat->AlbedoTex) return nullptr; // albedo не загрузился — откат к процедурному
    LOG_INFO("TestGame") << "PBR-сет загружен из " << dir
                         << " (normal=" << (mat->NormalTex ? "да" : "нет")
                         << ", metallic=" << (mat->MetallicTex ? "да" : "нет")
                         << ", roughness=" << (mat->RoughnessTex ? "да" : "нет")
                         << ", ao=" << (mat->AOTex ? "да" : "нет") << ")";
    return mat;
}

// Демонстрация PBR + normal mapping БЕЗ бинарных ассетов: процедурно генерируем
// albedo- и tangent-space normal-карты кирпичной кладки прямо в памяти и вешаем
// их на материал сферы. Так текстурный PBR-путь (RenderBatch::m_textured, TBN,
// Cook-Torrance) реально прогоняется в headless-скриншоте CI, а не только
// компилируется. Материал живёт в MaterialPtr сущности (владеет и текстурами).
// SAGE_TESTGAME_PBR_DIR переопределяет процедурный набор реальным PBR-сетом с диска.
void SpawnNormalMappedDemo(Scene& scene, glm::vec3 pos) {
    // Реальный PBR-сет с диска (если задан) — иначе процедурная кладка ниже.
    std::shared_ptr<Material> mat;
    if (const std::string d = sage::EnvString("SAGE_TESTGAME_PBR_DIR"); !d.empty())
        mat = LoadPbrSetFromDir(d);

    if (!mat) {
    const int N = 128;
    std::vector<unsigned char> albedo(N * N * 4);
    std::vector<float> height(N * N);

    // Кирпичная кладка: ряды со сдвигом, шов (mortar) темнее и «продавлен».
    auto brickAt = [&](int x, int y, float& h, glm::vec3& col) {
        const int brickH = 32, brickW = 64, mortar = 5;
        int row = y / brickH;
        int sx = (row % 2) ? brickW / 2 : 0;         // сдвиг чётных рядов
        int bx = (x + sx) % brickW;
        int by = y % brickH;
        bool seam = bx < mortar || by < mortar;
        if (seam) {
            h = 0.0f;                                 // шов утоплен
            col = glm::vec3(0.35f, 0.33f, 0.30f);     // серый раствор
        } else {
            // лёгкая зернистость кирпича + чуть выпуклая поверхность
            float n = 0.5f + 0.5f * std::sin(x * 12.9898f) * std::sin(y * 78.233f);
            h = 0.75f + 0.25f * n;
            float shade = 0.85f + 0.15f * n;
            col = glm::vec3(0.62f, 0.28f, 0.20f) * shade; // терракота
        }
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float h; glm::vec3 col;
            brickAt(x, y, h, col);
            height[y * N + x] = h;
            int i = (y * N + x) * 4;
            albedo[i + 0] = (unsigned char)(glm::clamp(col.r, 0.0f, 1.0f) * 255);
            albedo[i + 1] = (unsigned char)(glm::clamp(col.g, 0.0f, 1.0f) * 255);
            albedo[i + 2] = (unsigned char)(glm::clamp(col.b, 0.0f, 1.0f) * 255);
            albedo[i + 3] = 255;
        }

    // Tangent-space нормали из градиента высоты: N = normalize(-dH/dx, -dH/dy, 1),
    // кодируем в RGB [0..1]. strength масштабирует рельеф швов.
    std::vector<unsigned char> normal(N * N * 4);
    const float strength = 3.0f;
    auto H = [&](int x, int y) { return height[((y + N) % N) * N + ((x + N) % N)]; };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float dx = (H(x + 1, y) - H(x - 1, y)) * strength;
            float dy = (H(x, y + 1) - H(x, y - 1)) * strength;
            glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
            int i = (y * N + x) * 4;
            normal[i + 0] = (unsigned char)((n.x * 0.5f + 0.5f) * 255);
            normal[i + 1] = (unsigned char)((n.y * 0.5f + 0.5f) * 255);
            normal[i + 2] = (unsigned char)((n.z * 0.5f + 0.5f) * 255);
            normal[i + 3] = 255;
        }

    mat = std::make_shared<Material>();
    mat->Albedo = glm::vec3(1.0f);
    mat->Metallic = 0.0f;
    mat->Roughness = 0.55f;
    mat->AlbedoTex = std::make_shared<Texture>(albedo.data(), N, N);
    mat->NormalTex = std::make_shared<Texture>(normal.data(), N, N);
    } // конец процедурной ветки

    GameObject obj = scene.CreateObject("PBR Brick Sphere");
    obj.GetTransform().Position = pos;
    obj.GetTransform().Scale = glm::vec3(1.3f);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{MeshRef::Type::Sphere, ""};
    mr.MeshPtr = ResourceManager::Instance().GetSphere();
    mr.MaterialPtr = mat; // текстурный PBR-путь (HasMaps() == true)
    scene.Registry().emplace<StaticColliderComponent>(obj.Entity());
}

} // namespace

void TestGameLayer::BuildRoomOne(Scene& scene) {
    scene.SetName("room1");
    // Атриум 24x24: холодный дневной свет.
    scene.Lighting.Sun.Direction = {-0.45f, -1.0f, -0.35f};
    scene.Lighting.Sun.Color = {1.0f, 0.97f, 0.9f};
    scene.Lighting.Sun.Intensity = 0.95f;
    scene.Lighting.SkyColor = {0.55f, 0.65f, 0.85f};
    scene.Lighting.GroundColor = {0.22f, 0.2f, 0.18f};
    PointLight lamp;
    lamp.Position = {6.0f, 2.2f, -6.0f};
    lamp.Color = {1.0f, 0.6f, 0.3f};
    lamp.Intensity = 1.4f;
    lamp.Range = 9.0f;
    scene.Lighting.PointLights.push_back(lamp);

    SpawnBox(scene, "Floor", {0, -0.25f, 0}, {24, 0.5f, 24}, {0.38f, 0.4f, 0.44f}, false);
    // Периметр (4 стены) + колонны-препятствия — всё со StaticCollider.
    SpawnBox(scene, "Wall N", {0, 1.5f, -12}, {24, 3, 0.6f}, {0.5f, 0.5f, 0.55f}, true);
    SpawnBox(scene, "Wall S", {0, 1.5f, 12}, {24, 3, 0.6f}, {0.5f, 0.5f, 0.55f}, true);
    SpawnBox(scene, "Wall W", {-12, 1.5f, 0}, {0.6f, 3, 24}, {0.5f, 0.5f, 0.55f}, true);
    SpawnBox(scene, "Wall E", {12, 1.5f, 0}, {0.6f, 3, 24}, {0.5f, 0.5f, 0.55f}, true);
    SpawnBox(scene, "Pillar A", {-4, 1.25f, -4}, {1.2f, 2.5f, 1.2f}, {0.6f, 0.55f, 0.5f}, true);
    SpawnBox(scene, "Pillar B", {4, 1.25f, -4}, {1.2f, 2.5f, 1.2f}, {0.6f, 0.55f, 0.5f}, true);
    SpawnBox(scene, "Pillar C", {0, 1.25f, 5}, {1.2f, 2.5f, 1.2f}, {0.6f, 0.55f, 0.5f}, true);

    // Модель через ECS-путь: MeshRef::Type::Model -> ResourceManager::GetModel
    // (старый одно-мешевый ModelLoader::LoadObj) — проверяет ECS+obj связку.
    GameObject statue = scene.CreateObject("Statue");
    statue.GetTransform().Position = {6.5f, 0.0f, 6.5f};
    MeshRendererComponent& statueMr = statue.Renderer();
    statueMr.Ref = MeshRef{MeshRef::Type::Model, "assets/models/monument.obj"};
    statueMr.MeshPtr = ResourceManager::Instance().GetModel(statueMr.Ref.path);
    statueMr.Color = {0.75f, 0.72f, 0.65f};
    scene.Registry().emplace<StaticColliderComponent>(statue.Entity());

    // PBR-демо: кирпичная сфера с процедурными albedo + normal картами
    // (нормал-маппинг + Cook-Torrance) — прогоняет текстурный PBR-путь в CI.
    SpawnNormalMappedDemo(scene, {-2.2f, 1.2f, 5.5f});

    SpawnPickup(scene, "Coin 1-1", {-6, 0.8f, -6});
    SpawnPickup(scene, "Coin 1-2", {6, 0.8f, -7});
    SpawnPickup(scene, "Coin 1-3", {-7, 0.8f, 5});
    SpawnPickup(scene, "Coin 1-4", {2, 0.8f, 8});
    SpawnPickup(scene, "Coin 1-5", {-2, 0.8f, -8});

    SpawnPatrol(scene, "Sentry 1", {-8, 0.8f, 0}, {8, 0.8f, 0}, 2.4f);

    SpawnPortal(scene, "Portal to room2", {10, 1.3f, -10}, "room2", {0.0f, 0.85f, 5.5f});
    SpawnPlayer(scene, {0.0f, 0.85f, 9.0f});
    m_roomSpawns["room1"] = {0.0f, 0.85f, 9.0f};

    // Боевой сложный интерфейс (инвентарь + дерево навыков) поверх комнаты —
    // реальная проверка UI-тулкита на плотном экране, не на паре панелей.
    sage::ui::BuildShowcase(scene);

    // Стресс-тест масштабирования: SAGE_TESTGAME_STRESS=N рассыпает N*N мелких
    // кубов сеткой — большинство вне поля зрения, отсекается фрустумом, а
    // видимые рисуются одним инстанс-вызовом (проверка батчинга/отсечения).
    if (const char* s = std::getenv("SAGE_TESTGAME_STRESS")) {
        int n = std::max(1, std::atoi(s));
        int made = 0;
        for (int x = 0; x < n; ++x)
            for (int z = 0; z < n; ++z) {
                glm::vec3 pos{-11.0f + 22.0f * x / (float)n, 0.3f, -11.0f + 22.0f * z / (float)n};
                glm::vec3 col{0.4f + 0.5f * ((x * 7 + z) % 3) / 2.0f, 0.5f, 0.7f};
                SpawnBox(scene, "Stress " + std::to_string(made++), pos, {0.3f, 0.3f, 0.3f}, col, false);
            }
        LOG_INFO("TestGame") << "TESTGAME: stress-test " << made << " кубов (батчинг+отсечение)";
    }

    SpawnPhysicsProps(scene, {-6.5f, 0.0f, 6.0f}); // башенка падающих ящиков

    // Скелетно-анимированная модель: процедурный «щупалец» с клипом Wave
    // (пустой Path). Демонстрирует скелетную анимацию в живой игре.
    GameObject rig = scene.CreateObject("Animated Totem");
    rig.GetTransform().Position = {3.5f, 0.0f, 4.0f};
    scene.Registry().emplace<AnimatedModelComponent>(rig.Entity());

    // Факел — эмиттер частиц (огонь) на колонне: демонстрирует систему частиц.
    GameObject torch = scene.CreateObject("Torch Fire");
    torch.GetTransform().Position = {-4.0f, 2.7f, -4.0f}; // над Pillar A
    {
        ParticleEmitterComponent em;
        em.Config = ParticlePresets::Fire();
        scene.Registry().emplace<ParticleEmitterComponent>(torch.Entity(), em);
    }
}

void TestGameLayer::BuildRoomTwo(Scene& scene) {
    scene.SetName("room2");
    // Зал 30x14: тёплый закатный свет — комнаты явно различимы на скриншотах.
    scene.Lighting.Sun.Direction = {0.55f, -0.8f, 0.2f};
    scene.Lighting.Sun.Color = {1.0f, 0.72f, 0.45f};
    scene.Lighting.Sun.Intensity = 1.1f;
    scene.Lighting.SkyColor = {0.75f, 0.55f, 0.45f};
    scene.Lighting.GroundColor = {0.25f, 0.16f, 0.12f};
    PointLight torchA;
    torchA.Position = {-10.0f, 2.0f, 0.0f};
    torchA.Color = {0.3f, 0.7f, 1.0f};
    torchA.Intensity = 1.6f;
    torchA.Range = 10.0f;
    scene.Lighting.PointLights.push_back(torchA);
    PointLight torchB = torchA;
    torchB.Position = {10.0f, 2.0f, 0.0f};
    scene.Lighting.PointLights.push_back(torchB);

    SpawnBox(scene, "Floor", {0, -0.25f, 0}, {30, 0.5f, 14}, {0.42f, 0.36f, 0.32f}, false);
    SpawnBox(scene, "Wall N", {0, 1.5f, -7}, {30, 3, 0.6f}, {0.52f, 0.45f, 0.4f}, true);
    SpawnBox(scene, "Wall S", {0, 1.5f, 7}, {30, 3, 0.6f}, {0.52f, 0.45f, 0.4f}, true);
    SpawnBox(scene, "Wall W", {-15, 1.5f, 0}, {0.6f, 3, 14}, {0.52f, 0.45f, 0.4f}, true);
    SpawnBox(scene, "Wall E", {15, 1.5f, 0}, {0.6f, 3, 14}, {0.52f, 0.45f, 0.4f}, true);
    for (int i = 0; i < 4; ++i) {
        float x = -9.0f + i * 6.0f;
        SpawnBox(scene, "Column " + std::to_string(i), {x, 1.5f, -3.0f}, {1.0f, 3.0f, 1.0f},
                 {0.58f, 0.5f, 0.44f}, true);
        SpawnBox(scene, "Column' " + std::to_string(i), {x, 1.5f, 3.0f}, {1.0f, 3.0f, 1.0f},
                 {0.58f, 0.5f, 0.44f}, true);
    }

    SpawnPickup(scene, "Coin 2-1", {-12, 0.8f, 0});
    SpawnPickup(scene, "Coin 2-2", {-6, 0.8f, 5});
    SpawnPickup(scene, "Coin 2-3", {0, 0.8f, -5});
    SpawnPickup(scene, "Coin 2-4", {6, 0.8f, 5});
    SpawnPickup(scene, "Coin 2-5", {12, 0.8f, 0});
    SpawnPickup(scene, "Coin 2-6", {0, 0.8f, 0});
    SpawnPickup(scene, "Coin 2-7", {9, 0.8f, -5});

    SpawnPatrol(scene, "Sentry 2a", {-9, 0.8f, 0}, {9, 0.8f, 0}, 3.0f);
    SpawnPatrol(scene, "Sentry 2b", {0, 0.8f, -5}, {0, 0.8f, 5}, 2.0f);

    SpawnPortal(scene, "Portal to room1", {-13.5f, 1.3f, -5.5f}, "room1", {8.5f, 0.85f, -8.5f});
    SpawnPlayer(scene, {0.0f, 0.85f, 5.5f});
    m_roomSpawns["room2"] = {0.0f, 0.85f, 5.5f};
    // Монумент в центре зала рисуется НАПРЯМУЮ через Model::Load/Model::Draw
    // (см. OnRender) — второй, мульти-submesh путь загрузки моделей.
}

void TestGameLayer::AttachSceneScripts(const std::string& sceneName) {
    Scene* scene = m_scenes.Get(sceneName);
    auto engine = std::make_unique<ScriptEngine>();
    engine->BindScene(*scene);
    engine->BindCamera(m_camera);
    if (m_audio) engine->BindAudio(*m_audio);
    int attached = 0;
    auto view = scene->Registry().view<ScriptComponent>();
    for (auto e : view) {
        try {
            engine->AttachScript(GameObject(&scene->Registry(), e), view.get<ScriptComponent>(e).Path);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("TestGame") << "Скрипт не привязался: " << ex.what();
        }
    }
    LOG_INFO("TestGame") << sceneName << ": скриптов привязано " << attached;
    m_sceneScripts[sceneName] = std::move(engine);
}

// Боевая проверка сериализации: движковая часть сцены переживает запись в
// .sage и чтение обратно (кастомные компоненты сознательно не сериализуются —
// известное ограничение, см. GameComponents.h).
void TestGameLayer::VerifySerializationRoundTrip() {
    Scene* room1 = m_scenes.Get("room1");
    try {
        std::string json = SceneSerializer::SaveToString(*room1);
        std::unique_ptr<Scene> restored = SceneSerializer::LoadFromString(json);
        if (restored->Count() == room1->Count() && restored->Name() == room1->Name()) {
            LOG_INFO("TestGame") << "TESTGAME: serialization round-trip PASS ("
                                 << restored->Count() << " entities)";
        } else {
            LOG_ERROR("TestGame") << "TESTGAME: serialization round-trip FAIL (saved "
                                  << room1->Count() << ", loaded " << restored->Count() << ")";
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("TestGame") << "TESTGAME: serialization round-trip FAIL: " << ex.what();
    }
}

void TestGameLayer::SetupHud() {
    m_ui.emplace();

    m_hudHealth.Anchor = UIAnchor::BottomLeft;
    m_hudHealth.Offset = {18, 18};
    m_hudHealth.Size = {220, 22};
    m_hudHealth.Label = "HP";
    m_hudHealth.FillColor = {0.8f, 0.25f, 0.25f};
    m_hudHealth.ValueSource = [this]() {
        GameObject player = ActivePlayer();
        if (!player.Valid()) return 0.0f;
        auto& hp = player.Registry()->get<HealthComponent>(player.Entity());
        return hp.Current / hp.Max;
    };

    m_hudScore.Anchor = UIAnchor::TopRight;
    m_hudScore.Offset = {18, 14};
    m_hudScore.Scale = 2.2f;
    m_hudScore.TextSource = [this]() { return "SCORE " + std::to_string(m_score); };

    m_hudRoom.Anchor = UIAnchor::TopCenter;
    m_hudRoom.Offset = {0, 14};
    m_hudRoom.Scale = 2.0f;
    m_hudRoom.Color = {0.85f, 0.9f, 1.0f};
    m_hudRoom.TextSource = [this]() { return m_activeName; };

    m_hudHint.Anchor = UIAnchor::BottomCenter;
    m_hudHint.Offset = {0, 16};
    m_hudHint.Scale = 1.5f;
    m_hudHint.Color = {0.8f, 0.8f, 0.8f};
    // Кириллица — прямая проверка TrueType-шрифта UIRenderer (раньше был
    // ASCII-only stb_easy_font, русский текст не рисовался вообще).
    m_hudHint.Text = "WASD — движение, мышь — обзор, синий маяк — портал, ESC — выход";
}

// ============================================================================
//  Жизненный цикл слоя
// ============================================================================

void TestGameLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    // Предметные иконки ЭТОЙ игры — в движок, до первого кадра интерфейса.
    // Движок держит у себя только нейтральный набор (см. sage/ui/UIIcons.h);
    // доска, бочка, парус и остальной инвентарь принадлежат игре.
    RegisterGameIcons();

    // --- env-хуки (паттерн движка) ---
    if (const std::string p = sage::EnvString("SAGE_SCREENSHOT_PATH"); !p.empty())
        m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);
    // Качество графики — из гибкого конфига (файл sage.cfg + env-оверрайды,
    // включая обратно-совместимые SAGE_NO_SHADOWS/SAGE_NO_POST).
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    m_shadowsEnabled = cfg.Shadows;
    m_postEnabled = cfg.PostProcessing;
    // PostFX (SSAO + Bloom + виньетка) — параметры из EngineConfig.
    m_postfxSettings.Exposure = cfg.Exposure;
    m_postfxSettings.Gamma = cfg.Gamma;
    m_postfxSettings.Saturation = cfg.Saturation;
    m_postfxSettings.Contrast = cfg.Contrast;
    m_postfxSettings.Vignette = cfg.Vignette;
    m_postfxSettings.BloomEnabled = cfg.Bloom;
    m_postfxSettings.BloomThreshold = cfg.BloomThreshold;
    m_postfxSettings.BloomIntensity = cfg.BloomIntensity;
    m_postfxSettings.AOEnabled = cfg.AmbientOcclusion;
    m_postfxSettings.AOStrength = cfg.AOStrength;
    m_postfxSettings.AORadius = cfg.AORadius;
    m_autopilot = (std::getenv("SAGE_TESTGAME_AUTOPILOT") != nullptr);

    // --- аудио (первым: скрипты сцен биндят его при привязке) ---
    m_audio = std::make_unique<AudioEngine>();
    LOG_INFO("TestGame") << "Аудио: " << (m_audio->IsAvailable() ? "доступно" : "нет устройства (немой режим)");

    // --- рендер ---
    m_sceneShader.emplace("assets/shaders/scene.vert", "assets/shaders/scene.frag");
    m_shadowShader.emplace("assets/shaders/shadow_depth.vert", "assets/shaders/shadow_depth.frag");
    m_shadows.emplace(cfg.Shadows ? cfg.ShadowResolution : 512, cfg.ShadowCascades);
    m_sceneFbo.emplace(window.Width(), window.Height());
    m_postfx.emplace(); // SSAO + Bloom + виньетка (полная цепочка пост-обработки)
    m_monument = Model::Load("assets/models/monument.obj"); // прямой путь Model::Load
    m_particles.emplace(); // пул частиц (эмиттеры-факелы в комнатах)

    // --- HUD ---
    SetupHud();

    // --- мир: две комнаты + скрипты ---
    BuildRoomOne(m_scenes.CreateScene("room1"));
    BuildRoomTwo(m_scenes.CreateScene("room2"));
    AttachSceneScripts("room1");
    AttachSceneScripts("room2");
    VerifySerializationRoundTrip();

    // Стартовая комната: room1, либо явно заданная (отладка конкретной
    // комнаты/скриншоты CI без прохождения пути до неё).
    std::string startRoom = "room1";
    if (const char* room = std::getenv("SAGE_TESTGAME_START_ROOM")) {
        if (m_scenes.Get(room)) startRoom = room;
        else LOG_WARN("TestGame") << "SAGE_TESTGAME_START_ROOM: нет комнаты '" << room << "', старт в room1";
    }
    m_scenes.SetActive(startRoom);
    m_activeName = startRoom;
    m_activeSpawn = m_roomSpawns[startRoom];
    m_playerPos = m_activeSpawn;

    // Физика активной комнаты — динамические ящики начинают падать сразу.
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scenes.Active());
    LOG_INFO("TestGame") << "TESTGAME: physics backend " << m_physics->BackendName()
                         << ", " << m_physics->BodyCount() << " bodies in " << startRoom;
    RebuildSystems();

    // --- ввод: именованные действия через InputSystem (первый реальный
    // потребитель; подписка на мышь — через Window, см. InputSystem::Attach) ---
    m_input.Attach(window);
    m_input.Actions().Register("MoveForward").Bind(InputBinding::Key(sage::Key::W));
    m_input.Actions().Register("MoveBack").Bind(InputBinding::Key(sage::Key::S));
    m_input.Actions().Register("MoveLeft").Bind(InputBinding::Key(sage::Key::A));
    m_input.Actions().Register("MoveRight").Bind(InputBinding::Key(sage::Key::D));
    m_input.Actions().Register("Quit").Bind(InputBinding::Key(sage::Key::Escape));

    m_camera.Position = m_playerPos + glm::vec3(0.0f, kEyeHeight, 0.0f);
    m_camera.Yaw = -90.0f; // смотрим в -Z, вглубь комнаты
    m_camera.Pitch = -8.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    // Захват курсора для мышиного обзора — не в автопилоте/скриншот-режиме,
    // чтобы headless-прогоны не зависели от курсора.
    if (!m_autopilot && m_autoScreenshotFrame < 0) {
        // Через окно движка, а не glfwSetInputMode: захват курсора — то, что
        // движок обязан уметь сам, иначе каждая игра включает GLFW ради одной
        // строки и знает, на чём движок стоит.
        window.SetCursorCaptured(true);
        m_cursorCaptured = true;
    }

    // --- звук мира ---
    m_audio->SetListener(m_camera.Position, m_camera.Front, m_camera.Up);
    m_audio->PlayLoop("assets/audio/ambient.wav", 0.35f);

    LOG_INFO("TestGame") << "TESTGAME: started (room1: " << m_scenes.Get("room1")->Count()
                         << " entities, room2: " << m_scenes.Get("room2")->Count() << ")";
}

void TestGameLayer::OnDetach() {
    m_physics.reset(); // держит тела, ссылающиеся на сцену — снять до сцен
    m_sceneScripts.clear(); // скрипты держат ссылки на сцены/аудио — снять до них
    m_audio.reset();
    ResourceManager::Instance().Clear();
}

// ============================================================================
//  Игровой цикл
// ============================================================================

GameObject TestGameLayer::ActivePlayer() {
    Scene* scene = m_scenes.Active();
    return scene ? scene->FindByName("Player") : GameObject();
}

bool TestGameLayer::CollidesAt(glm::vec3 playerCenter) const {
    Scene* scene = const_cast<SceneManager&>(m_scenes).Active();
    if (!scene) return false;
    auto view = scene->Registry().view<StaticColliderComponent, Transform>();
    for (auto e : view) {
        const Transform& tr = view.get<Transform>(e);
        glm::vec3 half = tr.Scale * 0.5f;
        glm::vec3 delta = playerCenter - tr.Position;
        if (std::abs(delta.x) < half.x + kPlayerHalf.x &&
            std::abs(delta.y) < half.y + kPlayerHalf.y &&
            std::abs(delta.z) < half.z + kPlayerHalf.z) {
            return true;
        }
    }
    return false;
}

void TestGameLayer::MovePlayer(glm::vec3 wishDir, float dt) {
    if (glm::dot(wishDir, wishDir) < 1e-6f) return;
    wishDir = glm::normalize(glm::vec3(wishDir.x, 0.0f, wishDir.z));
    glm::vec3 step = wishDir * kPlayerSpeed * dt;

    // Раздельное движение по осям: упёрся по X — по Z всё ещё скользишь
    // вдоль стены (классика простых AABB-коллизий, реализация — в игре).
    glm::vec3 tryX = m_playerPos + glm::vec3(step.x, 0.0f, 0.0f);
    if (!CollidesAt(tryX)) m_playerPos = tryX;
    glm::vec3 tryZ = m_playerPos + glm::vec3(0.0f, 0.0f, step.z);
    if (!CollidesAt(tryZ)) m_playerPos = tryZ;
}

void TestGameLayer::UpdatePickups() {
    Scene* scene = m_scenes.Active();
    std::vector<int> collected;
    auto view = scene->Registry().view<PickupComponent, Transform, IdComponent, NameComponent>();
    for (auto e : view) {
        const Transform& tr = view.get<Transform>(e);
        const PickupComponent& pickup = view.get<PickupComponent>(e);
        glm::vec3 d = m_playerPos - tr.Position;
        d.y = 0.0f; // монетка плавает по высоте — сбор по горизонтальной дистанции
        if (glm::dot(d, d) < pickup.Radius * pickup.Radius) {
            m_score += pickup.Score;
            LOG_INFO("TestGame") << "TESTGAME: picked up " << view.get<NameComponent>(e).Name
                                 << " (score " << m_score << ")";
            m_audio->PlaySound2D("assets/audio/pickup.wav", 0.9f);
            collected.push_back(view.get<IdComponent>(e).Id);
        }
    }
    for (int id : collected) scene->RemoveObject(id);
    if (!collected.empty() && m_autopilot) m_autopilotPickups += (int)collected.size();
}

void TestGameLayer::DamagePlayer(float amount, glm::vec3 knockFrom) {
    GameObject player = ActivePlayer();
    if (!player.Valid()) return;
    auto& hp = player.Registry()->get<HealthComponent>(player.Entity());
    hp.Current -= amount;
    m_audio->PlaySound2D("assets/audio/hurt.wav", 0.9f);

    // Отбрасывание от источника урона (и не влетаем им в стену — те же AABB).
    glm::vec3 away = m_playerPos - knockFrom;
    away.y = 0.0f;
    if (glm::dot(away, away) > 1e-6f) {
        glm::vec3 knocked = m_playerPos + glm::normalize(away) * 1.2f;
        if (!CollidesAt(knocked)) m_playerPos = knocked;
    }

    LOG_INFO("TestGame") << "TESTGAME: player hurt (hp " << hp.Current << "/" << hp.Max << ")";
    if (hp.Current <= 0.0f) {
        hp.Current = hp.Max;
        m_playerPos = m_activeSpawn;
        LOG_INFO("TestGame") << "TESTGAME: player died, respawn";
    }
}

void TestGameLayer::UpdatePatrols(float dt) {
    Scene* scene = m_scenes.Active();
    auto view = scene->Registry().view<PatrolComponent, Transform>();
    for (auto e : view) {
        PatrolComponent& patrol = view.get<PatrolComponent>(e);
        Transform& tr = view.get<Transform>(e);
        glm::vec3 target = patrol.TowardB ? patrol.PointB : patrol.PointA;
        glm::vec3 to = target - tr.Position;
        float dist = glm::length(to);
        if (dist < 0.15f) {
            patrol.TowardB = !patrol.TowardB;
            continue;
        }
        tr.Position += (to / dist) * patrol.Speed * dt;
        tr.Rotation.y = glm::degrees(std::atan2(to.x, to.z));

        // Урон при касании: AABB врага против AABB игрока + кулдаун.
        if (m_hurtCooldown <= 0.0f) {
            glm::vec3 half = tr.Scale * 0.5f;
            glm::vec3 delta = m_playerPos - tr.Position;
            if (std::abs(delta.x) < half.x + kPlayerHalf.x &&
                std::abs(delta.y) < half.y + kPlayerHalf.y &&
                std::abs(delta.z) < half.z + kPlayerHalf.z) {
                DamagePlayer(patrol.Damage, tr.Position);
                m_hurtCooldown = 1.0f;
            }
        }
    }
}

void TestGameLayer::SwitchRoom(const std::string& target, glm::vec3 spawnPos) {
    GameObject oldPlayer = ActivePlayer();
    float carriedHp = 100.0f, carriedMax = 100.0f;
    if (oldPlayer.Valid()) {
        auto& hp = oldPlayer.Registry()->get<HealthComponent>(oldPlayer.Entity());
        carriedHp = hp.Current;
        carriedMax = hp.Max;
    }

    if (!m_scenes.SetActive(target)) {
        LOG_ERROR("TestGame") << "Портал в несуществующую сцену: " << target;
        return;
    }
    m_activeName = target;
    m_activeSpawn = spawnPos;
    m_playerPos = spawnPos;
    m_portalCooldown = 1.5f;

    GameObject newPlayer = ActivePlayer();
    if (newPlayer.Valid()) {
        auto& hp = newPlayer.Registry()->get<HealthComponent>(newPlayer.Entity());
        hp.Current = carriedHp;
        hp.Max = carriedMax;
        newPlayer.GetTransform().Position = spawnPos;
    }

    // Физику пересобираем под новую активную сцену (у каждой комнаты — свои тела).
    m_physics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scenes.Active());

    // Комната сменилась — вместе с ней сменились и физика, и скрипты.
    RebuildSystems();

    m_audio->PlaySound2D("assets/audio/portal.wav", 0.9f);
    LOG_INFO("TestGame") << "TESTGAME: portal -> " << target;
}

// Пересобирает состав кадра под АКТИВНУЮ комнату: у каждой свои тела и свой
// набор скриптов, а неактивная заморожена. Регистрация по именам — повторная
// заменяет прежнюю, поэтому дубликатов при смене комнаты не копится.
void TestGameLayer::RebuildSystems() {
    sage::CoreSystems core;
    core.Physics = m_physics.get();
    core.Particles = m_particles ? &*m_particles : nullptr;
    auto it = m_sceneScripts.find(m_activeName);
    core.Scripts = it != m_sceneScripts.end() ? it->second.get() : nullptr;
    sage::RegisterCoreSystems(m_systems, core);
    if (!core.Scripts) m_systems.Remove("scripts");
}

void TestGameLayer::UpdatePortals(float dt) {
    m_portalCooldown = std::max(0.0f, m_portalCooldown - dt);
    if (m_portalCooldown > 0.0f) return;

    Scene* scene = m_scenes.Active();
    auto view = scene->Registry().view<PortalComponent, Transform>();
    for (auto e : view) {
        const PortalComponent& portal = view.get<PortalComponent>(e);
        glm::vec3 d = m_playerPos - view.get<Transform>(e).Position;
        d.y = 0.0f;
        if (glm::dot(d, d) < portal.Radius * portal.Radius) {
            SwitchRoom(portal.TargetScene, portal.SpawnPos);
            return; // сцена сменилась — view невалиден
        }
    }
}

// Автопилот для CI: идёт к ближайшей монете, когда монет нет — к порталу.
// Проходит room1 -> room2 и продолжает собирать — лог набирает маркеры
// pickup/portal, которые проверяет smoke-тест.
void TestGameLayer::UpdateAutopilot(float dt) {
    Scene* scene = m_scenes.Active();

    glm::vec3 target;
    bool haveTarget = false;
    float bestDist = 1e9f;
    auto pickups = scene->Registry().view<PickupComponent, Transform>();
    for (auto e : pickups) {
        glm::vec3 pos = pickups.get<Transform>(e).Position;
        glm::vec3 d = pos - m_playerPos;
        d.y = 0.0f;
        float dist = glm::dot(d, d);
        if (dist < bestDist) { bestDist = dist; target = pos; haveTarget = true; }
    }
    if (!haveTarget) {
        auto portals = scene->Registry().view<PortalComponent, Transform>();
        for (auto e : portals) { target = portals.get<Transform>(e).Position; haveTarget = true; break; }
    }
    if (!haveTarget) return;

    glm::vec3 to = target - m_playerPos;
    to.y = 0.0f;
    if (glm::dot(to, to) < 0.05f) return;
    MovePlayer(glm::normalize(to), dt);

    // Камера плавно довернётся в сторону движения — скриншот смотрит "в дело".
    float wantYaw = glm::degrees(std::atan2(to.z, to.x));
    float delta = wantYaw - m_camera.Yaw;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    m_camera.Yaw += delta * std::min(1.0f, dt * 4.0f);
    m_camera.Pitch = -10.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);
}

void TestGameLayer::OnUpdate(float dt) {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    m_input.Update(window.Handle());
    if (m_input.Actions().WasPressed("Quit")) app.Close();
    if (m_cursorCaptured) m_input.ApplyMouseDelta(m_camera);

    m_hurtCooldown = std::max(0.0f, m_hurtCooldown - dt);

    if (m_autopilot) {
        UpdateAutopilot(dt);
    } else {
        // Направление движения — из ориентации камеры, спроецированной на XZ.
        glm::vec3 fwd = glm::vec3(m_camera.Front.x, 0.0f, m_camera.Front.z);
        glm::vec3 right = glm::vec3(m_camera.Right.x, 0.0f, m_camera.Right.z);
        glm::vec3 wish{0.0f};
        if (m_input.Actions().IsDown("MoveForward")) wish += fwd;
        if (m_input.Actions().IsDown("MoveBack")) wish -= fwd;
        if (m_input.Actions().IsDown("MoveRight")) wish += right;
        if (m_input.Actions().IsDown("MoveLeft")) wish -= right;
        MovePlayer(wish, dt);
    }

    UpdatePickups();
    UpdatePatrols(dt);
    UpdatePortals(dt); // может сменить комнату (пересобирает m_physics)

    // Кадр — планировщиком. Раньше эти вызовы стояли здесь руками, и скрипты
    // шли ПОСЛЕДНИМИ — то есть заданная скриптом скорость применялась физикой
    // на следующем кадре, в отличие от рантайма. Разница на глаз незаметна и
    // проявлялась как «управление иногда чуть залипает».
    m_systems.Run(*m_scenes.Active(), dt);

    // Сущность Player следует за C++-позицией (для сериализации/инспекции).
    GameObject player = ActivePlayer();
    if (player.Valid()) player.GetTransform().Position = m_playerPos;

    m_camera.Position = m_playerPos + glm::vec3(0.0f, kEyeHeight, 0.0f);
    m_audio->SetListener(m_camera.Position, m_camera.Front, m_camera.Up);
    m_audio->Update();
}

// ============================================================================
//  Рендер: тени -> HDR-сцена -> пост-процесс -> HUD
// ============================================================================

// Рисует НЕ-ECS геометрию, привязанную к переданному шейдеру: сейчас это только
// монумент комнаты 2 (текстурная модель). ECS-меши сцены идут отдельно через
// m_batch (отсечение по фрустуму + инстансинг), см. OnRender.
void TestGameLayer::DrawSceneGeometry(Shader& shader, bool colorPass) {
    // Монумент комнаты 2 — прямой Model::Draw (сам ставит uUseTexture/
    // uObjectColor по своим submesh'ам в цветном проходе).
    if (m_activeName == "room2" && m_monument) {
        Transform monumentTr;
        monumentTr.Position = {0.0f, 0.0f, 0.0f};
        monumentTr.Scale = {1.4f, 1.4f, 1.4f};
        shader.SetMat4("uModel", monumentTr.GetMatrix());
        if (colorPass) {
            m_monument->Draw(shader);
        } else {
            for (const SubMesh& sub : m_monument->SubMeshes()) sub.MeshData->Draw();
        }
    }
}

void TestGameLayer::OnRender() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();
    Scene* scene = m_scenes.Active();

    // Гибкий конфиг: соотношение сторон (letterbox), масштаб разрешения, туман.
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    int vpX, vpY, vpW, vpH;
    cfg.LetterboxViewport(window.Width(), window.Height(), vpX, vpY, vpW, vpH);
    int renderW, renderH;
    cfg.ScaledResolution(vpW, vpH, renderW, renderH); // внутреннее разрешение (RenderScale)

    glm::mat4 view = m_camera.GetViewMatrix();
    glm::mat4 proj = m_camera.GetProjectionMatrix((float)vpW / (float)std::max(vpH, 1));

    // Копия освещения сцены с учётом флага тумана из настроек.
    LightingEnvironment lighting = scene->Lighting;
    if (!cfg.Fog) lighting.Fog.Enabled = false;

    // --- 1. Проход теней (глубина из точки зрения солнца) ---
    if (m_shadowsEnabled) {
        if (m_shadows->CascadeCount() > 1) {
            // Улица: каскады привязаны к камере, а не к игроку. Тени нужны
            // там, куда смотрят, — привязка к игроку оставляла бы полкадра
            // без теней, стоило отвести взгляд.
            ShadowMap::CameraView v;
            v.Position = m_camera.Position;
            v.Forward = m_camera.Front;
            v.Up = m_camera.Up;
            v.FovY = glm::radians(m_camera.Fov);
            v.Aspect = (float)vpW / (float)std::max(vpH, 1);
            v.Near = m_camera.NearClip;
            v.ShadowDistance = cfg.ShadowDistance;
            m_shadows->SetCascades(scene->Lighting.Sun.Direction, v);
        } else {
            m_shadows->SetLightMatrix(scene->Lighting.Sun.Direction, m_playerPos, 20.0f);
        }
        // Проход глубины — по одному на каскад: каждая карта видит свой кусок
        // дальности, и геометрию для неё надо нарисовать отдельно. Это и есть
        // цена каскадов, о которой сказано в ShadowMap.h.
        // Монумент рисуется вручную (он не в ECS) — отдаём его обработчиком,
        // а не копируем весь проход ради одной строки.
        sage::render::RenderShadowDepth(
            *m_shadows, *scene, m_batch, window.Width(), window.Height(),
            [this](const glm::mat4& light) {
                m_shadowShader->Use();
                m_shadowShader->SetMat4("uLightSpace", light);
                DrawSceneGeometry(*m_shadowShader, /*colorPass=*/false);
            });
    }

    // Чёрные полосы letterbox — очищаем всё окно перед рендером в центр.
    device.BindDefaultFramebuffer();
    device.SetViewport(0, 0, window.Width(), window.Height());
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear();

    // --- 2. Основной проход: в HDR-буфер (RenderScale) или сразу на экран ---
    if (m_postEnabled) {
        m_sceneFbo->Resize(renderW, renderH); // внутреннее разрешение с учётом масштаба
        m_sceneFbo->Bind();
    } else {
        device.SetViewport(vpX, vpY, vpW, vpH); // без поста — сразу в letterbox-viewport
        // Гамму делал композит пост-процесса — без него включаем аппаратное
        // sRGB-кодирование, иначе прямой вывод линейного цвета выглядит тёмным.
        device.SetSRGBWrite(true);
    }
    device.SetClearColor(0.09f, 0.09f, 0.12f, 1.0f);
    device.Clear();

    m_sceneShader->Use();
    m_sceneShader->SetMat4("uView", view);
    m_sceneShader->SetMat4("uProjection", proj);
    m_sceneShader->SetVec3("uViewPos", m_camera.Position);
    UploadLighting(*m_sceneShader, lighting);
    const ShadowBinding shadowBinding(*m_shadows, m_shadowsEnabled);
    BindAndUploadShadows(*m_sceneShader, shadowBinding);

    sage::render::SceneColorInput color;
    color.View = view;
    color.Proj = proj;
    color.ViewPos = m_camera.Position;
    color.Env = &lighting;
    color.Shadows = shadowBinding;
    color.OcclusionCulling = cfg.OcclusionCulling;
    sage::render::RenderSceneColor(*scene, m_batch, color);
    // Частицы (billboard) — camRight/Up из матрицы вида.
    if (m_particles) m_particles->DrawFromView(view, proj);

    // --- 3. Пост-процесс: HDR -> экран (тон-маппинг ACES и т.д.), апскейл до
    //        letterbox-viewport (внутреннее разрешение могло быть меньше) ---
    if (m_postEnabled) {
        // Полная цепочка PostFX: SSAO (из глубины сцены) + Bloom + тон-маппинг/
        // виньетка. Пишет в экран, letterbox-viewport (vpX..).
        m_postfx->Render(m_sceneFbo->ColorTexture(), m_sceneFbo->DepthTexture(),
                         m_sceneFbo->Width(), m_sceneFbo->Height(), proj, view,
                         m_postfxSettings,
                         /*output=*/nullptr, vpX, vpY, vpW, vpH);
    } else {
        device.SetSRGBWrite(false); // HUD ниже — его цвета уже в sRGB
    }

    // --- 4. HUD поверх всего (движковый UIRenderer, не ImGui) — во весь экран,
    //        поверх возможных чёрных полос letterbox ---
    device.SetViewport(0, 0, window.Width(), window.Height());
    m_ui->Begin(window.Width(), window.Height());
    // UI сцены (инвентарь + дерево навыков активной комнаты)
    // — тот же путь, что в собранной игре/редакторе; под кастомным HUD игры.
    if (m_scenes.Active())
        sage::ui::DrawSceneUI(*m_scenes.Active(), *m_ui, window.Width(), window.Height());
    m_hudHealth.Draw(*m_ui);
    m_hudScore.Draw(*m_ui);
    m_hudRoom.Draw(*m_ui);
    m_hudHint.Draw(*m_ui);
    m_ui->End();

    ++m_frameCounter;
    if (m_autoScreenshotFrame >= 0 && m_frameCounter == m_autoScreenshotFrame) {
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}
