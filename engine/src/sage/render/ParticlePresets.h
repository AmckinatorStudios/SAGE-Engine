#pragma once
#include "Particle.h"
#include <vector>

// ---------------------------------------------------------------------
// ParticlePresets — готовые ParticleEmitterConfig под типовые визуальные
// эффекты (всплеск, дым, разлетающиеся осколки, тлеющие искры). Держим их
// отдельно от игровой логики (в render/, не в game/), потому что это чисто
// визуальные рецепты — не про правила игры, а про то, как это должно
// выглядеть. Игровой код вызывает нужный пресет в нужный момент.
// ---------------------------------------------------------------------
namespace ParticlePresets {

// Всплеск: игрок прыгнул в воду / рыба клюнула у поплавка. Быстрые белёсые
// капли, разлетающиеся вверх-в стороны и падающие обратно под гравитацией.
inline ParticleEmitterConfig WaterSplash() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-1.0f, 0.6f, -1.0f};
    c.DirectionMax = {1.0f, 1.6f, 1.0f};
    c.SpeedMin = 1.5f; c.SpeedMax = 3.2f;
    c.Gravity = -6.0f; // капли воды падают заметно быстрее дыма/искр
    c.LifetimeMin = 0.35f; c.LifetimeMax = 0.7f;
    c.StartSizeMin = 0.05f; c.StartSizeMax = 0.11f;
    c.EndSizeMin = 0.0f; c.EndSizeMax = 0.02f;
    c.StartColor = {0.75f, 0.88f, 0.95f, 0.85f};
    c.EndColor = {0.75f, 0.88f, 0.95f, 0.0f};
    c.Shape = ParticleShape::SoftCircle;
    return c;
}

// Дым из печки, пока готовится рыба: медленный, лёгкий, поднимается вверх
// (отрицательная "гравитация" = отрицательное ускорение вниз = дым всплывает),
// используется как непрерывный стрим (CreateStream + SetStreamActive).
inline ParticleEmitterConfig Smoke() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-0.15f, 0.8f, -0.15f};
    c.DirectionMax = {0.15f, 1.0f, 0.15f};
    c.SpeedMin = 0.3f; c.SpeedMax = 0.6f;
    c.Gravity = 0.4f; // положительное ускорение по Y — дым всплывает, а не падает
    c.LifetimeMin = 1.4f; c.LifetimeMax = 2.2f;
    c.StartSizeMin = 0.10f; c.StartSizeMax = 0.16f;
    c.EndSizeMin = 0.35f; c.EndSizeMax = 0.5f; // дым расширяется, растворяясь
    c.StartColor = {0.65f, 0.65f, 0.68f, 0.45f};
    c.EndColor = {0.8f, 0.8f, 0.82f, 0.0f};
    c.AngularVelocityMax = 0.6f;
    c.EmissionRate = 6.0f; // частиц в секунду для стрима
    c.Shape = ParticleShape::SoftCircle;
    return c;
}

// Искры-щепки при поломке блока корабля: короткая яркая вспышка обломков,
// разлетающихся во все стороны и быстро оседающих под гравитацией.
inline ParticleEmitterConfig BlockBreak() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-1.0f, 0.2f, -1.0f};
    c.DirectionMax = {1.0f, 1.2f, 1.0f};
    c.SpeedMin = 1.0f; c.SpeedMax = 2.5f;
    c.Gravity = -9.0f;
    c.LifetimeMin = 0.3f; c.LifetimeMax = 0.6f;
    c.StartSizeMin = 0.06f; c.StartSizeMax = 0.12f;
    c.EndSizeMin = 0.0f; c.EndSizeMax = 0.02f;
    c.StartColor = {0.55f, 0.40f, 0.24f, 1.0f}; // цвет дерева/досок по умолчанию
    c.EndColor = {0.55f, 0.40f, 0.24f, 0.0f};
    c.AngularVelocityMax = 4.0f;
    c.Shape = ParticleShape::Quad; // щепки — угловатые, не мягкие капли
    return c;
}

// Тлеющие искорки над печкой во время готовки — редкие, мелкие, тёплые.
// Стрим, включается вместе с дымом на время готовки.
inline ParticleEmitterConfig StoveEmbers() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-0.3f, 0.8f, -0.3f};
    c.DirectionMax = {0.3f, 1.4f, 0.3f};
    c.SpeedMin = 0.4f; c.SpeedMax = 0.9f;
    c.Gravity = 0.1f;
    c.LifetimeMin = 0.5f; c.LifetimeMax = 0.9f;
    c.StartSizeMin = 0.02f; c.StartSizeMax = 0.04f;
    c.EndSizeMin = 0.0f; c.EndSizeMax = 0.0f;
    c.StartColor = {1.0f, 0.55f, 0.15f, 1.0f};
    c.EndColor = {1.0f, 0.2f, 0.05f, 0.0f};
    c.EmissionRate = 4.0f;
    c.Shape = ParticleShape::SoftCircle;
    return c;
}

// Огонь: тёплый непрерывный столб пламени, поднимающийся вверх и остывающий
// от жёлтого к красному, затем в дым. Универсальный факел/костёр.
inline ParticleEmitterConfig Fire() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-0.2f, 0.9f, -0.2f};
    c.DirectionMax = {0.2f, 1.4f, 0.2f};
    c.SpeedMin = 0.6f; c.SpeedMax = 1.2f;
    c.Gravity = 0.8f; // пламя всплывает
    c.LifetimeMin = 0.5f; c.LifetimeMax = 1.0f;
    c.StartSizeMin = 0.18f; c.StartSizeMax = 0.30f;
    c.EndSizeMin = 0.02f; c.EndSizeMax = 0.06f;
    c.StartColor = {1.0f, 0.85f, 0.25f, 0.9f}; // жёлто-белое ядро
    c.EndColor = {0.8f, 0.15f, 0.05f, 0.0f};   // гаснет в тёмно-красный
    c.EmissionRate = 40.0f;
    c.Shape = ParticleShape::SoftCircle;
    return c;
}

// Искры (фейерверк/электрика): быстрый разлёт ярких точек во все стороны,
// падающих под гравитацией. Разовый залп (Burst).
inline ParticleEmitterConfig Sparks() {
    ParticleEmitterConfig c;
    c.DirectionMin = {-1.0f, -0.2f, -1.0f};
    c.DirectionMax = {1.0f, 1.0f, 1.0f};
    c.SpeedMin = 2.5f; c.SpeedMax = 5.0f;
    c.Gravity = -7.0f;
    c.LifetimeMin = 0.4f; c.LifetimeMax = 0.9f;
    c.StartSizeMin = 0.03f; c.StartSizeMax = 0.06f;
    c.EndSizeMin = 0.0f; c.EndSizeMax = 0.0f;
    c.StartColor = {1.0f, 0.9f, 0.5f, 1.0f};
    c.EndColor = {1.0f, 0.4f, 0.1f, 0.0f};
    c.EmissionRate = 30.0f;
    c.Shape = ParticleShape::SoftCircle;
    return c;
}

// Реестр пресетов для UI/сериализации: имя + фабрика конфига. Индекс в этом
// списке хранится в ParticleEmitterComponent.Preset (0 — «Custom», без пресета).
struct PresetEntry { const char* Name; ParticleEmitterConfig (*Make)(); };
inline const std::vector<PresetEntry>& Registry() {
    static const std::vector<PresetEntry> kEntries = {
        {"Fire", &Fire},
        {"Smoke", &Smoke},
        {"Sparks", &Sparks},
        {"Water Splash", &WaterSplash},
        {"Embers", &StoveEmbers},
        {"Block Break", &BlockBreak},
    };
    return kEntries;
}

} // namespace ParticlePresets
