#pragma once
#include <algorithm>

// Статы игрока — именно в этом порядке: здоровье, энергия, голод, жажда.
// Все значения 0..100.
//
// Правила выживания (мягкие — игра про расслабленность, а не хардкор):
//  - Голод и жажда медленно убывают со временем; жажда чуть быстрее.
//  - Бег и плавание тратят энергию; отдых её восстанавливает.
//  - Если голод/жажда на нуле — здоровье понемногу утекает.
//  - Если игрок сыт и напоен (>60) — здоровье понемногу восстанавливается.
struct PlayerStats {
    float Health = 100.0f;
    float Energy = 100.0f;
    float Hunger = 100.0f;
    float Thirst = 100.0f;

    // isExerting — бежит или плывёт прямо сейчас
    void Update(float dt, bool isExerting) {
        Hunger = std::max(0.0f, Hunger - 0.12f * dt);           // ~14 минут от 100 до 0
        Thirst = std::max(0.0f, Thirst - 0.18f * dt);           // ~9 минут

        if (isExerting) {
            Energy = std::max(0.0f, Energy - 6.0f * dt);
        } else {
            // Отдых. Восстановление медленнее, если голоден/жаждет.
            float regenScale = (Hunger > 20.0f && Thirst > 20.0f) ? 1.0f : 0.3f;
            Energy = std::min(100.0f, Energy + 4.0f * regenScale * dt);
        }

        if (Hunger <= 0.0f || Thirst <= 0.0f) {
            Health = std::max(0.0f, Health - 1.5f * dt);
        } else if (Hunger > 60.0f && Thirst > 60.0f) {
            Health = std::min(100.0f, Health + 0.8f * dt);
        }
    }

    bool CanRun() const { return Energy > 5.0f; }
    bool IsDead() const { return Health <= 0.0f; }

    void Eat(float amount)   { Hunger = std::min(100.0f, Hunger + amount); }
    void Drink(float amount) { Thirst = std::min(100.0f, Thirst + amount); }

    // Респавн после смерти: половина всего, кроме здоровья
    void Respawn() {
        Health = 50.0f;
        Energy = 50.0f;
        Hunger = 50.0f;
        Thirst = 50.0f;
    }
};
