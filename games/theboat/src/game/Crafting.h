#pragma once
#include "Inventory.h"
#include <vector>
#include <string>

// Рецепт: до 3 ингредиентов -> результат. Крафт мгновенный (альфа).
struct CraftIngredient {
    ItemType Item = ItemType::None;
    int Amount = 0;
};

struct CraftRecipe {
    std::string Name; // отображаемое имя (ASCII)
    std::vector<CraftIngredient> Ingredients;
    ItemType Result = ItemType::None;
    int ResultAmount = 1;
};

// Все рецепты альфы The Boat. Идея экономики: из океана приходит мусор
// пяти видов, из него собирается всё остальное — стройблоки для корабля,
// удочка (открывает рыбалку -> еду) и пресная вода (жажда).
inline const std::vector<CraftRecipe>& GetCraftRecipes() {
    static const std::vector<CraftRecipe> recipes = {
        { "Plank",       {{ItemType::ScrapWood, 2}},                          ItemType::PlankBlock, 1 },
        { "Wood",        {{ItemType::ScrapWood, 3}},                          ItemType::WoodBlock,  1 },
        { "Sail",        {{ItemType::Cloth, 2}, {ItemType::Rope, 1}},         ItemType::SailBlock,  1 },
        { "Stove",       {{ItemType::ScrapMetal, 3}, {ItemType::ScrapWood, 1}}, ItemType::StoveBlock, 1 },
        { "Barrel",      {{ItemType::ScrapWood, 2}, {ItemType::ScrapMetal, 1}}, ItemType::BarrelBlock, 1 },
        { "Fishing Rod", {{ItemType::ScrapWood, 2}, {ItemType::Rope, 2}},     ItemType::FishingRod, 1 },
        { "Fresh Water", {{ItemType::ScrapPlastic, 2}},                       ItemType::FreshWater, 1 },
    };
    return recipes;
}

inline bool CanCraft(const Inventory& inv, const CraftRecipe& recipe) {
    for (const auto& ing : recipe.Ingredients) {
        if (!inv.Has(ing.Item, ing.Amount)) return false;
    }
    return true;
}

inline bool Craft(Inventory& inv, const CraftRecipe& recipe) {
    if (!CanCraft(inv, recipe)) return false;
    for (const auto& ing : recipe.Ingredients) inv.Remove(ing.Item, ing.Amount);
    inv.Add(recipe.Result, recipe.ResultAmount);
    return true;
}
