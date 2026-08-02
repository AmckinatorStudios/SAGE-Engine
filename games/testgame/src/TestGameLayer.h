#pragma once
#include "sage/core/SystemScheduler.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "sage/core/Layer.h"
#include "sage/core/InputSystem.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/PostFX.h"
#include "sage/render/Model.h"
#include "sage/render/ParticleSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/scene/SceneManager.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/Widgets.h"
#include "GameComponents.h"

class ScriptEngine;
class AudioEngine;

// ---------------------------------------------------------------------------
// TestGameLayer — «боевая» тестовая игра SAGE (games/testgame).
//
// НЕ игра ради игры, а стресс-тест движка: маленький, но полный игровой цикл,
// который сознательно нагружает подсистемы, оставшиеся без потребителей после
// удаления The Boat (см. README, «games/testgame»):
//   • Игрок от первого лица: InputSystem (именованные действия WASD/мышь) +
//     СВОЯ AABB-коллизия со стенами (коллизии — забота игры, не движка).
//   • Две комнаты-сцены через SceneManager, переход по пульсирующему порталу.
//   • Кастомные ECS-компоненты (Health/Pickup/Patrol/StaticCollider/Portal) —
//     GameComponents.h, движок не правился.
//   • Десятки Lua-скриптов одновременно (монетки/маяки) — стресс ScriptEngine.
//   • HUD движковым UIRenderer/Widgets (health bar + счёт + подсказки).
//   • Аудио: эмбиент-луп + 2D-эффекты подбора/урона/портала (AudioEngine,
//     в headless CI тихо деградирует — IsAvailable()=false, Play* — no-op).
//   • Модель .obj двумя путями: ECS-энтити через ResourceManager::GetModel и
//     прямой Model::Load + Model::Draw (мульти-submesh путь).
//   • Тени (ShadowMap + shadow_depth.*) и пост-обработка (Framebuffer HDR +
//     PostFX) — первый реальный рендер-прогон после RHI-миграции.
//
// Env-хуки (паттерн движка): SAGE_SCREENSHOT_AT_FRAME/SAGE_SCREENSHOT_PATH,
// SAGE_NO_SHADOWS, SAGE_NO_POST, SAGE_TESTGAME_AUTOPILOT=1 (бот сам собирает
// монеты и проходит портал — для headless CI, лог-маркеры "TESTGAME:").
// ---------------------------------------------------------------------------
class TestGameLayer : public sage::Layer {
public:
    // Конструктор/деструктор в .cpp: у unique_ptr<ScriptEngine/AudioEngine>
    // должен быть полный тип в точке генерации деструктора (тот же приём, что
    // в SandboxLayer).
    TestGameLayer();
    ~TestGameLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    // --- построение мира ---
    void BuildRoomOne(Scene& scene);
    void BuildRoomTwo(Scene& scene);
    GameObject SpawnBox(Scene& scene, const std::string& name, glm::vec3 pos,
                        glm::vec3 scale, glm::vec3 color, bool collider);
    // Динамический ящик с движковой физикой (RigidBody+Collider): падает и
    // складывается на полу через выбранный бэкенд (Jolt/Simple) — демонстрирует
    // физику в реальном игровом цикле, а не только в редакторе.
    void SpawnPhysicsProps(Scene& scene, glm::vec3 origin);
    void AttachSceneScripts(const std::string& sceneName);
    void VerifySerializationRoundTrip(); // TESTGAME: лог-маркер PASS/FAIL
    void SetupHud();

    // --- игровой цикл ---
    GameObject ActivePlayer(); // сущность "Player" активной сцены
    void MovePlayer(glm::vec3 wishDir, float dt); // AABB-коллизия по осям
    bool CollidesAt(glm::vec3 playerCenter) const;
    void UpdatePickups();
    void UpdatePatrols(float dt);
    void UpdatePortals(float dt);
    void RebuildSystems();   // состав кадра под активную комнату
    void UpdateAutopilot(float dt);
    void SwitchRoom(const std::string& target, glm::vec3 spawnPos);
    void DamagePlayer(float amount, glm::vec3 knockFrom);

    // --- рендер ---
    // Общая геометрия кадра для обоих проходов: colorPass=false — только
    // uModel+Draw (глубина для теней), true — цвет/текстуры/модель целиком.
    void DrawSceneGeometry(Shader& shader, bool colorPass);

    // --- мир ---
    SceneManager m_scenes;
    std::string m_activeName;
    std::unordered_map<std::string, std::unique_ptr<ScriptEngine>> m_sceneScripts;
    std::unordered_map<std::string, glm::vec3> m_roomSpawns; // стартовая точка каждой комнаты
    std::unique_ptr<PhysicsScene> m_physics;
    sage::SystemScheduler m_systems;   // порядок кадра — см. SystemScheduler.h // физика активной сцены (Jolt/Simple)
    glm::vec3 m_activeSpawn{0.0f};

    // --- игрок (живёт поверх сцен; здоровье — в HealthComponent сущности
    // "Player" активной сцены и переносится при переходе) ---
    glm::vec3 m_playerPos{0.0f};      // центр AABB игрока
    int m_score = 0;
    float m_hurtCooldown = 0.0f;      // сек до следующего возможного урона
    float m_portalCooldown = 0.0f;    // сек игнора порталов после перехода
    static constexpr glm::vec3 kPlayerHalf{0.35f, 0.85f, 0.35f};
    static constexpr float kEyeHeight = 0.65f; // глаза выше центра AABB
    static constexpr float kPlayerSpeed = 5.0f;

    // --- ввод/камера ---
    InputSystem m_input;
    Camera m_camera;
    bool m_cursorCaptured = false;

    // --- рендер ---
    std::optional<Shader> m_sceneShader;
    std::optional<Shader> m_shadowShader;
    std::optional<ShadowMap> m_shadows;
    std::optional<Framebuffer> m_sceneFbo;
    std::optional<sage::render::PostFX> m_postfx;      // SSAO + Bloom + composite
    sage::render::PostFXSettings m_postfxSettings;     // экспозиция/AO/bloom/виньетка
    bool m_shadowsEnabled = true;
    bool m_postEnabled = true;
    std::unique_ptr<Model> m_monument; // прямой Model::Load путь (комната 2)
    std::optional<ParticleSystem> m_particles; // пул частиц (эмиттеры ECS)
    sage::ecs::RenderBatch m_batch;            // отсечение по фрустуму + инстансинг статики

    // --- HUD ---
    std::optional<UIRenderer> m_ui;
    UIProgressBar m_hudHealth;
    UILabel m_hudScore;
    UILabel m_hudRoom;
    UILabel m_hudHint;

    // --- аудио ---
    std::unique_ptr<AudioEngine> m_audio;

    // --- автопилот (CI) ---
    bool m_autopilot = false;
    int m_autopilotPickups = 0;

    // --- авто-скриншот (SAGE_SCREENSHOT_*) ---
    std::string m_screenshotPath = "testgame.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
