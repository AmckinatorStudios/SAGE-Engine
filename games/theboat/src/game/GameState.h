#pragma once
#include "GameConstants.h"
#include "Inventory.h"
#include "PlayerStats.h"
#include "PlayerController.h"
#include "DayNight.h"
#include "TrashSystem.h"
#include "Fishing.h"
#include "DefaultShip.h"
#include "../voxel/World.h"
#include "../voxel/WaterPlane.h"
#include "sage/scene/Scene.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/ParticlePresets.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/audio/AudioEngine.h"
#include "GameSounds.h"
#include "sage/core/InputMap.h"
#include <string>

// ---------------------------------------------------------------------
// GameState — всё состояние партии The Boat в одном месте: мир, игрок,
// инвентарь, системы (мусор/рыбалка/сутки) и мелкие флаги UI (открыто ли
// меню крафта, что показывает тост и т.п.).
//
// Раньше все эти поля были ~20 отдельными локальными переменными прямо в
// теле main() — работало, но что угодно, принимающее "текущее состояние
// игры", вынуждено было принимать длинный список параметров. Теперь это
// один объект, который можно передать по ссылке в любую функцию геймплея
// (см. PlayerActions.h) или в будущем — сохранить/загрузить целиком.
//
// GameState специально НЕ хранит рендер-ресурсы (шейдеры/текстуры/UI) —
// это дело main.cpp и рендер-слоя. Здесь только "что происходит в мире".
// ---------------------------------------------------------------------
struct GameState {
    static constexpr float CookDuration = 4.0f;
    static constexpr float OceanHalfSize = 400.0f; // половина стороны квада океана вокруг корабля

    World Terrain;
    ShipInfo Ship;
    WaterPlane Water{OceanHalfSize, GameConstants::SeaLevel};

    Scene SceneData;
    ScriptEngine Scripts; // работает с объектами SceneData — см. BindScene/BindInput в конструкторе
    DayNightCycle DayNight;
    int DayCounter = 1;
    float PrevTimeOfDay = 0.0f;

    PlayerController Player;
    PlayerStats Stats;
    Inventory Items;

    TrashSystem Trash{GameConstants::SeaLevel, glm::vec3(0.0f)};
    FishingSystem Fishing;
    float CookTimer = 0.0f; // > 0 — на печке жарится рыба

    ParticleSystem Particles;
    static constexpr const char* kStoveSmokeStream = "stove_smoke";
    static constexpr const char* kStoveEmbersStream = "stove_embers";

    // Звук — часть партии (как Particles): геймплей триггерит его напрямую
    // (см. PlayerActions.h). При недоступном аудио-устройстве (headless/CI)
    // AudioEngine молча становится no-op — см. AudioEngine::IsAvailable().
    AudioEngine Audio;
    AudioEngine::SoundHandle OceanLoop = AudioEngine::InvalidHandle;
    FishingSystem::State PrevFishingState = FishingSystem::State::Idle; // для звука поклёвки

    bool CraftMenuOpen = false;
    int CraftSelected = 0;
    bool Noclip = false;
    bool DebugHudVisible = false;

    std::string Toast;
    float ToastTimer = 0.0f;

    // Строит корабль, ставит игрока на палубу, выдаёт стартовый инвентарь.
    // Вызывается один раз при старте новой партии (или после SAGE_* env
    // переопределений в main — она может тронуть Player.Position уже после).
    GameState() : Trash(GameConstants::SeaLevel, glm::vec3(0.0f)) {
        Ship = BuildDefaultShip(Terrain, glm::ivec3(0, 6, 0));
        Terrain.RebuildDirtyMeshes();
        Player.Position = Ship.Center;
        Trash = TrashSystem(GameConstants::SeaLevel, Ship.Center);

        // Стартовые значения — чисто заглушка на первый кадр до старта игры,
        // реальный hemisphere-ambient (небо/отражённый свет) каждый кадр
        // выставляет DayNightCycle::ApplyTo() ниже, в UpdateSimulation()
        SceneData.Lighting.SetFlatAmbient({1.0f, 1.0f, 1.0f}, 0.35f);
        for (const glm::vec3& pos : Ship.LanternPositions) {
            PointLight lantern;
            lantern.Position = pos;
            lantern.Color = {1.0f, 0.72f, 0.38f};
            lantern.Intensity = 1.0f; // реальная яркость выставляется DayNight.ApplyTo() каждый кадр
            lantern.Range = 13.0f;
            SceneData.Lighting.PointLights.push_back(lantern);
        }

        // Стартовый набор: удочка (сразу можно рыбачить и расслабляться),
        // немного досок на первые перестройки и запас еды/воды на разгон
        Items.Add(ItemType::FishingRod, 1);
        Items.Add(ItemType::PlankBlock, 8);
        Items.Add(ItemType::CookedFish, 1);
        Items.Add(ItemType::FreshWater, 2);

        // Дым и искры печки — привязаны к её позиции на всё время партии,
        // включаются/выключаются по CookTimer в UpdateSimulation()
        glm::vec3 stoveTop = glm::vec3(Ship.StovePos) + glm::vec3(0.5f, 1.0f, 0.5f);
        Particles.CreateStream(kStoveSmokeStream, ParticlePresets::Smoke(), stoveTop);
        Particles.CreateStream(kStoveEmbersStream, ParticlePresets::StoveEmbers(), stoveTop);

        // Звуковой фон партии: тихая музыка (стриминг) + зацикленный эмбиент
        // моря. Оба no-op, если аудио недоступно. Категории разведены, чтобы
        // при желании музыку/эффекты можно было регулировать раздельно.
        Audio.SetCategoryVolume(AudioEngine::Category::Music, 0.6f);
        Audio.SetCategoryVolume(AudioEngine::Category::Ambient, 0.8f);
        Audio.PlayMusic(GameSounds::Music, 0.5f, /*loop=*/true);
        OceanLoop = Audio.PlayLoop(GameSounds::Ocean, 0.7f, AudioEngine::Category::Ambient);

        // Скрипты работают с объектами SceneData — то, что они заспавнят
        // через SpawnObject(), рисуется тем же проходом, что и любой другой
        // GameObject (см. main.cpp, рендер SceneData.Objects()). Остальные
        // Bind* (Input/Camera/Particles/Billboards) и опциональный запуск
        // demo_features.lua делает main.cpp ПОСЛЕ конструирования GameState —
        // см. комментарий там. Раньше RunScript() вызывался прямо здесь, но
        // тогда демо-скрипт стартовал до того, как эти системы успевали
        // привязаться, и падал на первом же обращении к камере/частицам.
        Scripts.BindScene(SceneData);

        PrevTimeOfDay = DayNight.TimeOfDay();
    }

    void ShowToast(const std::string& text) {
        Toast = text;
        ToastTimer = 2.5f;
    }

    // Общий тик всего, что не завязано на конкретный ввод игрока:
    // статы, время суток, дрейф мусора, таймер готовки, респавн при смерти.
    // Действия по вводу (ломать/ставить/крафтить) — в PlayerActions.h.
    void UpdateSimulation(float dt, bool isExerting) {
        Stats.Update(dt, isExerting);
        if (Stats.IsDead()) {
            LOG_WARN("Game") << "Игрок погиб - респавн на палубе";
            Stats.Respawn();
            Player.Position = Ship.Center;
            Player.Velocity = glm::vec3(0.0f);
            Fishing.Reel();
            ShowToast("You passed out... woke up on deck");
        }

        DayNight.Update(dt);
        if (DayNight.TimeOfDay() < PrevTimeOfDay) ++DayCounter; // перескочили полночь
        PrevTimeOfDay = DayNight.TimeOfDay();
        DayNight.ApplyTo(SceneData.Lighting);

        Trash.Update(dt);
        Fishing.Update(dt);
        // Звук поклёвки — один раз в момент перехода в состояние Bite (а не
        // каждый кадр, пока рыба клюёт), у поплавка в мире (3D).
        if (Fishing.CurrentState() == FishingSystem::State::Bite &&
            PrevFishingState != FishingSystem::State::Bite) {
            Audio.PlaySound3D(GameSounds::Bite, Fishing.BobberPosition(), 0.9f);
        }
        PrevFishingState = Fishing.CurrentState();
        Scripts.UpdateAll(dt);

        bool cooking = CookTimer > 0.0f;
        Particles.SetStreamActive(kStoveSmokeStream, cooking);
        Particles.SetStreamActive(kStoveEmbersStream, cooking);

        if (CookTimer > 0.0f) {
            CookTimer -= dt;
            if (CookTimer <= 0.0f) {
                Items.Add(ItemType::CookedFish, 1);
                ShowToast("Fish is ready!");
                Audio.PlaySound2D(GameSounds::CookDone, 0.8f);
                LOG_INFO("Game") << "Рыба пожарена";
            }
        }

        Particles.Update(dt);

        if (ToastTimer > 0.0f) ToastTimer -= dt;
    }
};
