#pragma once
#include "GameState.h"
#include "Crafting.h"
#include "PlayerActions.h"
#include "sage/ui/UIRenderer.h"
#include "sage/render/DebugOverlay.h"
#include "sage/core/Stats.h"
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------
// GameHud — вся immediate-mode отрисовка игрового интерфейса The Boat:
// прицел, хотбар, контекстные подсказки, тост-сообщения и крафт-меню.
// Статбары и часы собраны отдельно, как UICanvas виджеты (см. main.cpp) —
// здесь то, что либо завязано на список произвольной длины (крафт-меню,
// хотбар), либо ветвится по игровой логике (подсказки) и потому не ложится
// на простой Widgets.h.
//
// DrawWorldHud рисует всё, что видно всегда во время игры.
// DrawDebugOverlay — отдельно, F3-панель (не immediate-mode UI, а
// DebugOverlay движка — технический шрифт, включается по требованию).
// ---------------------------------------------------------------------
namespace GameHud {

inline void DrawCrosshair(UIRenderer& ui, float w, float h) {
    ui.Rect(w / 2 - 8, h / 2 - 1, 16, 2, {1, 1, 1}, 0.8f);
    ui.Rect(w / 2 - 1, h / 2 - 8, 2, 16, {1, 1, 1}, 0.8f);
}

inline void DrawHotbar(UIRenderer& ui, const Inventory& inventory, float w, float h) {
    float slot = 58, gap = 6;
    float totalW = Inventory::HotbarSize * slot + (Inventory::HotbarSize - 1) * gap;
    float hx = w / 2 - totalW / 2, hy = h - slot - 14;

    for (int i = 0; i < Inventory::HotbarSize; ++i) {
        float x = hx + i * (slot + gap);
        ItemType item = inventory.HotbarItem(i);
        int count = inventory.Count(item);
        bool selected = (i == inventory.SelectedSlot());

        ui.Rect(x, hy, slot, slot, {0.08f, 0.08f, 0.1f}, selected ? 0.85f : 0.55f);
        if (selected) ui.RectOutline(x - 2, hy - 2, slot + 4, slot + 4, 2, {1, 1, 1}, 0.9f);

        glm::vec3 nameColor = (count > 0 || item == ItemType::FishingRod)
            ? glm::vec3(1.0f) : glm::vec3(0.45f);
        ui.Text(x + 4, hy + 6, 1.1f, nameColor, GetItemName(item));
        if (item != ItemType::FishingRod) {
            ui.Text(x + 4, hy + slot - 16, 1.5f, nameColor, std::to_string(count));
        }
        ui.Text(x + slot - 12, hy + slot - 14, 1.1f, {0.6f, 0.6f, 0.6f}, std::to_string(i + 1));
    }
}

// Контекстная подсказка под прицелом: зависит от того, на что игрок смотрит
// и что происходит (клюёт рыба, готовится еда, есть мусор рядом...)
inline void DrawContextHint(UIRenderer& ui, const GameState& game, const PlayerActions::LookContext& look, float w, float h) {
    std::string hint;
    glm::vec3 hintColor{1, 1, 1};
    ItemType heldItem = game.Items.SelectedItem();

    if (game.Fishing.CurrentState() == FishingSystem::State::Bite) {
        hint = "!!! PULL - LMB !!!";
        hintColor = {1.0f, 0.35f, 0.3f};
    } else if (game.Fishing.CurrentState() == FishingSystem::State::Waiting) {
        hint = "Waiting for a bite...";
        hintColor = {0.85f, 0.9f, 1.0f};
    } else if (look.TargetedTrash >= 0) {
        hint = std::string("F - pick up ") + GetItemName(game.Trash.Items()[look.TargetedTrash].Type);
    } else if (look.LookingAtStove && game.CookTimer > 0.0f) {
        int pct = (int)((1.0f - game.CookTimer / GameState::CookDuration) * 100.0f);
        hint = "Cooking... " + std::to_string(pct) + "%";
        hintColor = {1.0f, 0.75f, 0.4f};
    } else if (look.LookingAtStove && game.Items.Has(ItemType::RawFish) && heldItem == ItemType::RawFish) {
        hint = "RMB - cook raw fish";
    }

    if (hint.empty()) return;
    float scale = (game.Fishing.CurrentState() == FishingSystem::State::Bite) ? 3.0f : 2.0f;
    ui.TextCentered(w / 2, h / 2 + 36, scale, hintColor, hint);
}

inline void DrawToast(UIRenderer& ui, const GameState& game, float w, float h) {
    if (game.ToastTimer <= 0.0f) return;
    float alpha = std::min(1.0f, game.ToastTimer / 0.5f);
    float tw = UIRenderer::MeasureText(game.Toast, 2.0f);
    ui.Rect(w / 2 - tw / 2 - 10, h - 150, tw + 20, 26, {0, 0, 0}, 0.5f * alpha);
    ui.TextCentered(w / 2, h - 144, 2.0f, glm::vec3(1.0f), game.Toast);
}

inline void DrawCraftMenu(UIRenderer& ui, const GameState& game, float w, float h) {
    if (!game.CraftMenuOpen) return;
    const auto& recipes = GetCraftRecipes();
    float mw = 520, mh = 70.0f + recipes.size() * 40.0f;
    float mx = w / 2 - mw / 2, my = h / 2 - mh / 2;

    ui.Rect(mx, my, mw, mh, {0.06f, 0.07f, 0.1f}, 0.92f);
    ui.RectOutline(mx, my, mw, mh, 2, {0.8f, 0.75f, 0.6f}, 0.9f);
    ui.TextCentered(w / 2, my + 12, 2.4f, {1.0f, 0.95f, 0.8f}, "CRAFTING");

    for (size_t i = 0; i < recipes.size(); ++i) {
        const CraftRecipe& r = recipes[i];
        float ry = my + 52 + i * 40.0f;
        bool can = CanCraft(game.Items, r);
        bool sel = ((int)i == game.CraftSelected);

        if (sel) ui.Rect(mx + 8, ry - 4, mw - 16, 34, {0.25f, 0.3f, 0.4f}, 0.8f);
        glm::vec3 color = can ? glm::vec3(0.5f, 0.95f, 0.5f) : glm::vec3(0.55f);
        ui.Text(mx + 20, ry, 2.0f, color, r.Name);

        std::ostringstream ing;
        for (size_t k = 0; k < r.Ingredients.size(); ++k) {
            if (k) ing << " + ";
            ing << r.Ingredients[k].Amount << " " << GetItemName(r.Ingredients[k].Item);
        }
        ui.Text(mx + 190, ry + 3, 1.4f, can ? glm::vec3(0.85f) : glm::vec3(0.5f), ing.str());
    }
    ui.TextCentered(w / 2, my + mh - 22, 1.4f, {0.7f, 0.7f, 0.7f}, "UP/DOWN - select   ENTER - craft   TAB - close");
}

// Всё, что рисуется поверх 3D-сцены каждый кадр во время игры (без debug HUD)
inline void DrawWorldHud(UIRenderer& ui, const GameState& game, const PlayerActions::LookContext& look, int screenW, int screenH) {
    float w = (float)screenW, h = (float)screenH;
    DrawCrosshair(ui, w, h);
    DrawHotbar(ui, game.Items, w, h);
    DrawContextHint(ui, game, look, w, h);
    DrawToast(ui, game, w, h);
    DrawCraftMenu(ui, game, w, h);
}

// F3 — техническая панель поверх всего (собственный шрифт DebugOverlay)
inline void DrawDebugOverlay(DebugOverlay& overlay, const GameState& game, float fps,
                             int screenW, int screenH,
                             bool postEnabled = true, bool shadowsEnabled = true) {
    std::vector<DebugLine> lines;
    char buf[128];

    std::snprintf(buf, sizeof(buf), "FPS: %.0f", fps);
    lines.emplace_back(buf, fps < 30 ? glm::vec3(1, 0.4f, 0.4f) : glm::vec3(0.6f, 1, 0.6f));

    std::snprintf(buf, sizeof(buf), "pos: %.1f %.1f %.1f", game.Player.Position.x, game.Player.Position.y, game.Player.Position.z);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "draw calls: %d  chunks: %d", g_renderStats.DrawCalls, g_renderStats.ChunksRendered);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "particles: %zu  streams: %zu", game.Particles.AliveCount(), game.Particles.StreamCount());
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "time: %s  sun: %.2f", game.DayNight.ClockString().c_str(), game.DayNight.SunElevation());
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "onGround: %d  inWater: %d  noclip: %d", game.Player.OnGround, game.Player.InWater, game.Noclip);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "post: %s  shadows: %s", postEnabled ? "on" : "off", shadowsEnabled ? "on" : "off");
    lines.emplace_back(buf);

    overlay.Draw(lines, screenW, screenH);
}

} // namespace GameHud
