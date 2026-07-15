#pragma once
#include "../scene/Light.h"
#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <string>

// Цикл дня и ночи. Держит "игровое время" 0..1 (0 = полночь, 0.25 = рассвет,
// 0.5 = полдень, 0.75 = закат) и каждый кадр обновляет LightingEnvironment:
// направление/цвет/яркость солнца, фон (ambient), тинт skybox'а и яркость
// корабельных фонарей (ночью они разгораются).
class DayNightCycle {
public:
    // Полный цикл суток — 10 минут реального времени: достаточно долго,
    // чтобы "пожить" в каждом времени суток, и достаточно коротко, чтобы
    // за одну сессию увидеть и закат, и ночь под фонарями.
    float DayLengthSeconds = 600.0f;

    void SetTimeOfDay(float t) { m_time = t - std::floor(t); }
    float TimeOfDay() const { return m_time; }

    void Update(float dt) {
        m_time += dt / DayLengthSeconds;
        m_time -= std::floor(m_time);
    }

    // Высота солнца над горизонтом: -1 (глубокая ночь) .. +1 (зенит)
    float SunElevation() const {
        return std::sin((m_time - 0.25f) * 2.0f * 3.14159265f);
    }

    // "HH:MM" для интерфейса
    std::string ClockString() const {
        int totalMinutes = (int)(m_time * 24.0f * 60.0f);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", (totalMinutes / 60) % 24, totalMinutes % 60);
        return buf;
    }

    // Тинт неба — умножается на текстуру skybox
    glm::vec3 SkyTint() const {
        float e = SunElevation();
        glm::vec3 day{1.0f, 1.0f, 1.0f};
        glm::vec3 night{0.10f, 0.13f, 0.24f};
        glm::vec3 dusk{1.0f, 0.62f, 0.42f}; // рассвет/закат

        float dayness = glm::clamp(e * 3.0f, 0.0f, 1.0f);      // как быстро "включается" день
        float duskness = glm::clamp(1.0f - std::abs(e) * 4.0f, 0.0f, 1.0f); // пик у горизонта

        glm::vec3 base = glm::mix(night, day, dayness);
        return glm::mix(base, dusk, duskness * 0.6f);
    }

    // lanternPositions обновляются снаружи (если фонарь сломали/поставили)
    void ApplyTo(LightingEnvironment& env) const {
        float e = SunElevation();

        // Солнце ездит по дуге восток->запад; ночью светит "из-под горизонта"
        // (яркость всё равно нулевая, так что направление не важно)
        float angle = (m_time - 0.25f) * 2.0f * 3.14159265f;
        env.Sun.Direction = glm::normalize(glm::vec3(-std::cos(angle), -std::sin(angle), -0.25f));

        float dayness = glm::clamp(e * 3.0f, 0.0f, 1.0f);
        float duskness = glm::clamp(1.0f - std::abs(e) * 4.0f, 0.0f, 1.0f);

        env.Sun.Intensity = glm::clamp(e * 2.0f, 0.0f, 1.0f);
        glm::vec3 noonColor{1.0f, 0.97f, 0.90f};
        glm::vec3 duskColor{1.0f, 0.55f, 0.30f};
        env.Sun.Color = glm::mix(noonColor, duskColor, duskness);

        // Ночь не должна быть кромешной — луна/звёзды дают слабый холодный фон.
        // Небо (верхние грани) и отражённый от воды свет (нижние грани) —
        // разные цвета: небо холоднее и ярче, вода темнее и слегка зеленее
        // из-за собственного цвета океана (см. water.frag deepColor).
        env.AmbientStrength = glm::mix(0.12f, 0.40f, dayness);

        glm::vec3 nightSky{0.10f, 0.14f, 0.28f};
        glm::vec3 daySky{0.55f, 0.68f, 0.92f};
        glm::vec3 duskSky{0.85f, 0.55f, 0.45f};
        glm::vec3 sky = glm::mix(glm::mix(nightSky, daySky, dayness), duskSky, duskness * 0.5f);

        glm::vec3 nightGround{0.03f, 0.05f, 0.09f};
        glm::vec3 dayGround{0.10f, 0.22f, 0.26f}; // холодный зеленовато-синий отблеск воды
        glm::vec3 ground = glm::mix(nightGround, dayGround, dayness);

        env.SkyColor = sky;
        env.GroundColor = ground;

        // Фонари: тлеют днём, разгораются ночью
        float lanternIntensity = glm::mix(2.6f, 0.25f, dayness);
        for (auto& light : env.PointLights) {
            light.Intensity = lanternIntensity;
        }
    }

private:
    float m_time = 0.35f; // старт утром — самое расслабленное время
};
