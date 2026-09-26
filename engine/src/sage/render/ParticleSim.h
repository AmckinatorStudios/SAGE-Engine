#pragma once
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/ParticleEffect.h"

// ---------------------------------------------------------------------------
// Симуляция ОДНОГО эмиттера — без графики.
//
// Отделена от отрисовки намеренно: всё, что решает, как частица живёт (форма
// испускания, залпы, силы, ветер, турбулентность, столкновения, раскадровка),
// проверяется модульными тестами без видеокарты. Рендер берёт отсюда готовые
// частицы и только рисует их.
//
// Каждый эмиттер — свой пул и свой генератор случайных чисел: у разных
// эффектов разная текстура, смешивание и способ отрисовки, и общий на всю
// сцену пул (как было) не давал нарисовать их по-разному.
// ---------------------------------------------------------------------------
namespace sage::fx {

struct Particle {
    glm::vec3 Position{0.0f};  // World — в мире; Local — в осях эмиттера
    glm::vec3 Velocity{0.0f};  // собственная скорость (силы действуют на неё)
    glm::vec3 Moved{0.0f};     // фактическое смещение за прошлый шаг / dt — для вытягивания
    float Age = 0.0f;
    float Lifetime = 1.0f;
    float Size = 0.1f;         // начальный размер
    glm::vec4 Color{1.0f};     // начальный цвет
    float Rotation = 0.0f;     // радианы
    float AngularVelocity = 0.0f;  // рад/с
    float Seed = 0.0f;         // 0..1, постоянный на жизнь (случайный кадр, ось меша)
    int Frame = 0;             // кадр для FlipbookMode::Random
    // След: xyz — точка, w — время рождения точки (время эмиттера).
    std::vector<glm::vec4> Trail;

    float T() const { return Lifetime > 0.0f ? glm::clamp(Age / Lifetime, 0.0f, 1.0f) : 1.0f; }
};

// Что увидели при столкновении с миром.
struct CollisionHit {
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
};
// Луч в мировых координатах from → to; true — попал.
using CollisionQuery = std::function<bool(const glm::vec3& from, const glm::vec3& to, CollisionHit& hit)>;

// Что случилось с частицей — для дочерних эмиттеров.
struct ParticleEvent {
    SubEmitter::Trigger When = SubEmitter::Trigger::Death;
    glm::vec3 Position{0.0f};  // в мире
    glm::vec3 Velocity{0.0f};  // в мире
    glm::vec4 Color{1.0f};
};

class ParticleEmitter {
public:
    explicit ParticleEmitter(uint32_t seed = 0x5A6E1234u) : m_rng(seed) {}

    // Шаг эмиттера: рождение по времени, пути и залпам + жизнь всех частиц.
    // emitterWorld — мировая матрица объекта-эмиттера на этом шаге.
    void Step(const ParticleEffect& fx, const glm::mat4& emitterWorld, float dt,
              const CollisionQuery* world = nullptr);

    // Немедленно родить count частиц (скрипт, дочерний эмиттер, «залп по
    // кнопке»). Работает и у остановленного эмиттера. tint умножает цвет,
    // addVelocity добавляется к скорости (дочерний эмиттер наследует их от
    // погибшей частицы: осколки летят туда же, куда летел снаряд).
    void Emit(const ParticleEffect& fx, const glm::mat4& emitterWorld, int count,
              const glm::vec4& tint = glm::vec4(1.0f), const glm::vec3& addVelocity = glm::vec3(0.0f));

    void Play();                    // с начала цикла
    void Stop(bool clear = false);  // перестать рождать; clear — убрать живые
    bool Playing() const { return m_playing; }
    // Жив ли эмиттер: рождает или ещё есть что дорисовать.
    bool Alive() const;

    const std::vector<Particle>& Particles() const { return m_particles; }
    std::vector<ParticleEvent>& Events() { return m_events; }
    float Time() const { return m_time; }        // время внутри цикла
    float TotalTime() const { return m_total; }   // с начала жизни эмиттера (следы, шум)
    const glm::mat4& World() const { return m_world; }

    // --- Как частица выглядит СЕЙЧАС (общее для рендера и тестов) ----------
    static float SizeOf(const ParticleEffect& fx, const Particle& p);
    static glm::vec4 ColorOf(const ParticleEffect& fx, const Particle& p);
    // Номер кадра раскадровки 0..frames-1.
    static int FrameOf(const ParticleEffect& fx, const Particle& p, int frames);
    // Число кадров листа/набора (не меньше 1).
    static int FrameCount(const ParticleEffect& fx);

    // Точка и направление вылета в осях формы (без смещения/поворота формы) —
    // открыто ради тестов формы испускания.
    void SampleShape(const ParticleEffect& fx, glm::vec3& pos, glm::vec3& dir);

private:
    float Rand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
    void SpawnOne(const ParticleEffect& fx, const glm::mat4& world, float preAge,
                  const glm::vec4& tint = glm::vec4(1.0f), const glm::vec3& addVelocity = glm::vec3(0.0f));
    void Simulate(const ParticleEffect& fx, float dt, const CollisionQuery* world);

    std::vector<Particle> m_particles;
    std::vector<ParticleEvent> m_events;
    std::mt19937 m_rng;
    glm::mat4 m_world{1.0f};
    glm::vec3 m_lastPos{0.0f};
    glm::vec3 m_emitterVelocity{0.0f};
    bool m_hasLast = false;
    bool m_playing = true;
    bool m_started = false;   // был ли первый шаг (разогрев, задержка)
    float m_time = 0.0f;      // время в цикле (после задержки)
    float m_delay = 0.0f;     // сколько задержки осталось
    float m_total = 0.0f;
    float m_rateAccum = 0.0f;
    float m_distAccum = 0.0f;
    bool m_finished = false;  // неповторяющийся цикл кончился
    std::vector<int> m_burstDone;   // сколько повторов залпа уже было в этом цикле
};

// Детерминированный шум для турбулентности: векторное поле, гладкое в
// пространстве и во времени. Открыто ради теста «поле непрерывно».
glm::vec3 NoiseField(const glm::vec3& p, float time, int octaves);

} // namespace sage::fx
