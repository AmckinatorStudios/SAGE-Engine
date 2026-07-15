#pragma once
#include "GameState.h"
#include "Crafting.h"
#include "../render/Camera.h"
#include "../voxel/WorldRaycast.h"
#include "../core/Log.h"
#include "../core/InputSystem.h"
#include "GameActions.h"
#include <GLFW/glfw3.h>

// ---------------------------------------------------------------------
// PlayerActions — реакция на игровой ввод в терминах ИГРЫ, а не клавиш:
// "сломать блок", "использовать предмет в руке", "открыть крафт". Раньше
// это был один длинный блок if/else прямо в главном цикле main.cpp;
// вынесено сюда, чтобы main.cpp читался как "кадр = ввод, симуляция,
// рендер", а не тонул в деталях каждого предмета.
//
// Каждая функция берёт GameState по ссылке и мутирует его напрямую —
// сознательно процедурный стиль (а не ООП-команды/события), потому что
// количество действий небольшое и фиксированное для альфы; усложнять
// их в отдельные классы-команды пока не оправдано.
// ---------------------------------------------------------------------
namespace PlayerActions {

// AABB блока, который собираемся поставить, не должен пересекаться с игроком
inline bool BlockIntersectsPlayer(glm::ivec3 blockPos, const PlayerController& player) {
    glm::vec3 bmin(blockPos), bmax = bmin + glm::vec3(1.0f);
    glm::vec3 pmin = player.Position - glm::vec3(PlayerController::HalfWidth, 0.0f, PlayerController::HalfWidth);
    glm::vec3 pmax = player.Position + glm::vec3(PlayerController::HalfWidth, PlayerController::Height, PlayerController::HalfWidth);
    return (pmin.x < bmax.x && pmax.x > bmin.x) &&
           (pmin.y < bmax.y && pmax.y > bmin.y) &&
           (pmin.z < bmax.z && pmax.z > bmin.z);
}

// Результат рейкаста и производные от него — считается один раз за кадр
// в main и передаётся во все обработчики ниже, чтобы не дублировать рейкаст.
struct LookContext {
    WorldVoxelHit Hit;
    bool LookingAtStove = false;
    int TargetedTrash = -1; // индекс в GameState.Trash.Items(), -1 если нет цели
};

inline LookContext ComputeLookContext(GameState& game, const Camera& camera) {
    LookContext ctx;
    ctx.Hit = RaycastWorld(game.Terrain, camera.Position, camera.Front, 6.0f);
    if (ctx.Hit.Hit) {
        BlockType looked = game.Terrain.GetBlock(ctx.Hit.BlockPos.x, ctx.Hit.BlockPos.y, ctx.Hit.BlockPos.z);
        ctx.LookingAtStove = (looked == BlockType::Stove);
    }
    ctx.TargetedTrash = game.Trash.FindTargeted(camera.Position, camera.Front);
    return ctx;
}

// F — подобрать ближайший мусор, на который смотрит игрок
inline void PickUpTrash(GameState& game, const LookContext& look) {
    if (look.TargetedTrash < 0) return;
    glm::vec3 pos = game.Trash.Items()[look.TargetedTrash].Position;
    ItemType picked = game.Trash.Take(look.TargetedTrash);
    game.Items.Add(picked, 1);
    game.ShowToast(std::string("+1 ") + GetItemName(picked));
    LOG_INFO("Game") << "Подобран мусор: " << GetItemName(picked);
    game.Particles.Burst(ParticlePresets::WaterSplash(), pos, 5);
}

// Цвет частиц-обломков подбираем под материал блока — иначе доски
// разлетались бы теми же щепками, что и парус или бочка
inline glm::vec4 BlockDebrisColor(BlockType type) {
    switch (type) {
        case BlockType::Wood:   return {0.42f, 0.30f, 0.16f, 1.0f};
        case BlockType::Plank:  return {0.69f, 0.52f, 0.32f, 1.0f};
        case BlockType::Sail:   return {0.85f, 0.83f, 0.76f, 1.0f};
        case BlockType::Stove:  return {0.30f, 0.30f, 0.32f, 1.0f};
        case BlockType::Barrel: return {0.48f, 0.33f, 0.18f, 1.0f};
        default:                return {0.6f, 0.6f, 0.6f, 1.0f};
    }
}

// ЛКМ — подсечь рыбу (если клюёт/ждём) или сломать блок, на который смотрим
inline void HandleLeftClick(GameState& game, const LookContext& look) {
    if (game.Fishing.CurrentState() == FishingSystem::State::Bite) {
        if (game.Fishing.TryHook()) {
            game.Items.Add(ItemType::RawFish, 1);
            game.ShowToast("+1 Raw Fish");
            LOG_INFO("Game") << "Рыба поймана";
            game.Particles.Burst(ParticlePresets::WaterSplash(), game.Fishing.BobberPosition(), 10);
        }
        return;
    }
    if (game.Fishing.IsActive()) {
        game.Fishing.Reel();
        game.ShowToast("Reeled in");
        return;
    }
    if (!look.Hit.Hit) return;

    BlockType broken = game.Terrain.GetBlock(look.Hit.BlockPos.x, look.Hit.BlockPos.y, look.Hit.BlockPos.z);
    game.Terrain.SetBlock(look.Hit.BlockPos.x, look.Hit.BlockPos.y, look.Hit.BlockPos.z, BlockType::Air);
    ItemType drop = GetBlockItem(broken);
    if (drop != ItemType::None) {
        game.Items.Add(drop, 1);
        game.ShowToast(std::string("+1 ") + GetItemName(drop));
    }

    ParticleEmitterConfig debris = ParticlePresets::BlockBreak();
    debris.StartColor = BlockDebrisColor(broken);
    debris.EndColor = glm::vec4(glm::vec3(debris.StartColor), 0.0f);
    glm::vec3 blockCenter = glm::vec3(look.Hit.BlockPos) + glm::vec3(0.5f);
    game.Particles.Burst(debris, blockCenter, 10);
}

// ПКМ — использовать предмет в руке (контекстно: блок/удочка/еда/питьё)
inline void HandleRightClick(GameState& game, const Camera& camera, const LookContext& look) {
    ItemType heldItem = game.Items.SelectedItem();
    BlockType placeBlock = GetItemBlock(heldItem);

    if (game.Fishing.IsActive() && heldItem == ItemType::FishingRod) {
        game.Fishing.Reel();
        game.ShowToast("Reeled in");
        return;
    }
    if (heldItem == ItemType::FishingRod) {
        game.Fishing.Cast(camera.Position, camera.Front, GameConstants::SeaLevel);
        game.ShowToast("Cast... wait for a bite");
        return;
    }
    if (placeBlock != BlockType::Air) {
        if (!game.Items.Has(heldItem)) {
            game.ShowToast("No blocks left - craft more");
        } else if (look.Hit.Hit && !BlockIntersectsPlayer(look.Hit.PlacePos, game.Player)) {
            game.Terrain.SetBlock(look.Hit.PlacePos.x, look.Hit.PlacePos.y, look.Hit.PlacePos.z, placeBlock);
            game.Items.Remove(heldItem, 1);
        }
        return;
    }
    if (heldItem == ItemType::RawFish) {
        if (!game.Items.Has(ItemType::RawFish)) {
            game.ShowToast("No raw fish");
        } else if (look.LookingAtStove && game.CookTimer <= 0.0f) {
            game.Items.Remove(ItemType::RawFish, 1);
            game.CookTimer = GameState::CookDuration;
            game.ShowToast("Cooking...");
        } else if (look.LookingAtStove) {
            game.ShowToast("Stove is busy");
        } else {
            game.ShowToast("Raw... cook it on the stove");
        }
        return;
    }
    if (heldItem == ItemType::CookedFish) {
        if (game.Items.Remove(ItemType::CookedFish, 1)) {
            game.Stats.Eat(40.0f);
            game.ShowToast("Tasty!");
        } else {
            game.ShowToast("No cooked fish");
        }
        return;
    }
    if (heldItem == ItemType::FreshWater) {
        if (game.Items.Remove(ItemType::FreshWater, 1)) {
            game.Stats.Drink(45.0f);
            game.ShowToast("Refreshing!");
        } else {
            game.ShowToast("No fresh water");
        }
    }
}

// Ввод, относящийся к меню крафта: навигация стрелками + подтверждение Enter.
// Клики мышью в этом кадре "съедаются" меню — вызывающий код (main.cpp)
// не должен читать BreakOrHook/UseItem, пока открыто меню крафта.
inline void HandleCraftMenuInput(GameState& game, InputMap& actions) {
    const auto& recipes = GetCraftRecipes();
    if (actions.WasPressed(GameActions::CraftNavigateUp)) {
        game.CraftSelected = (game.CraftSelected + (int)recipes.size() - 1) % (int)recipes.size();
    }
    if (actions.WasPressed(GameActions::CraftNavigateDown)) {
        game.CraftSelected = (game.CraftSelected + 1) % (int)recipes.size();
    }
    if (actions.WasPressed(GameActions::CraftConfirm)) {
        const CraftRecipe& recipe = recipes[game.CraftSelected];
        if (Craft(game.Items, recipe)) {
            LOG_INFO("Craft") << "Скрафчено: " << recipe.Name;
            game.ShowToast(std::string("Crafted: ") + recipe.Name);
        } else {
            game.ShowToast("Not enough materials");
        }
    }
}

} // namespace PlayerActions
