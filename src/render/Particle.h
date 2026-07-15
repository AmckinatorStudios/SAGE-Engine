#pragma once
#include <glm/glm.hpp>

// ---------------------------------------------------------------------
// Одна живая частица. ParticleSystem хранит их плоским вектором (без
// указателей и виртуальных функций — это горячий путь, обновляемый
// каждый кадр для потенциально тысяч частиц) и обновляет по простому
// закону: движение по Velocity + Gravity, линейная интерполяция размера
// и цвета от начального к конечному значению по мере старения.
// ---------------------------------------------------------------------
struct Particle {
    glm::vec3 Position{0.0f};
    glm::vec3 Velocity{0.0f};
    float Gravity = 0.0f; // ускорение по Y, копируется из ParticleEmitterConfig при спавне
    float Rotation = 0.0f;
    float AngularVelocity = 0.0f; // рад/сек

    float Age = 0.0f;
    float Lifetime = 1.0f;

    float StartSize = 0.1f, EndSize = 0.1f;
    glm::vec4 StartColor{1.0f}, EndColor{1.0f}; // rgba, alpha тоже интерполируется

    float T() const { return Lifetime > 0.0f ? glm::clamp(Age / Lifetime, 0.0f, 1.0f) : 1.0f; }
    bool IsAlive() const { return Age < Lifetime; }

    float CurrentSize() const { return glm::mix(StartSize, EndSize, T()); }
    glm::vec4 CurrentColor() const { return glm::mix(StartColor, EndColor, T()); }
};

// Форма отрисовки частицы (см. uShape в particle.frag)
enum class ParticleShape { SoftCircle = 0, Quad = 1 };

// ---------------------------------------------------------------------
// Конфигурация ОДНОГО "залпа" или непрерывной струи частиц — по сути,
// рецепт того, как рождается и живёт частица данного эффекта. Диапазоны
// (Min/Max) дают частицам разброс, чтобы эффект не выглядел механически
// одинаковым (важно для дыма/всплесков/искр — живой хаос вместо клонов).
// ---------------------------------------------------------------------
struct ParticleEmitterConfig {
    // Направление и разброс скорости вылета частицы
    glm::vec3 DirectionMin{-0.5f, 0.5f, -0.5f};
    glm::vec3 DirectionMax{0.5f, 1.5f, 0.5f};
    float SpeedMin = 1.0f, SpeedMax = 2.0f;

    float Gravity = -2.0f; // ускорение по Y; 0 — частицы не падают (дым), <0 — падают (искры/капли)

    float LifetimeMin = 0.5f, LifetimeMax = 1.0f;

    float StartSizeMin = 0.08f, StartSizeMax = 0.15f;
    float EndSizeMin = 0.0f, EndSizeMax = 0.02f; // обычно частицы усыхают к концу жизни

    glm::vec4 StartColor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 EndColor{1.0f, 1.0f, 1.0f, 0.0f}; // по умолчанию угасают в прозрачность

    float AngularVelocityMax = 0.0f; // 0 — частицы не вращаются

    ParticleShape Shape = ParticleShape::SoftCircle;

    // Для непрерывных эмиттеров (дым из печки): частиц в секунду.
    // Для разовых залпов (искры при поломке блока) — не используется,
    // там количество передаётся явно в ParticleSystem::Burst().
    float EmissionRate = 20.0f;
};
