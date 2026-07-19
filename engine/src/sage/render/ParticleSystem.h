#pragma once
#include "Particle.h"
#include "Shader.h"
#include "Camera.h"
#include "sage/rhi/GraphicsDevice.h"
#include <memory>
#include <vector>
#include <random>
#include <unordered_map>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------
// ParticleSystem — часть ядра рендера SAGE Engine, не завязана на конкретную
// игру: работает с произвольными мировыми позициями.
//
// Модель использования — две категории эмиттеров:
//
//   1) Разовый "залп" — Burst(config, position, count): например, искры при
//      поломке блока или всплеск воды. Частицы рождаются один раз и живут
//      своей жизнью до Lifetime, эмиттер не хранится.
//
//   2) Непрерывная струя — StreamEmitter, привязанный к месту и включаемый
//      по условию (дым из печки, пока горит): CreateStream(id, config, pos),
//      затем каждый кадр SetStreamActive(id, bool) и SetStreamPosition(id, pos)
//      если источник двигается. Система сама рождает частицы по EmissionRate,
//      пока стрим активен.
//
// Рендер — один draw call на ВСЕ живые частицы разом через инстансинг:
// геометрия — общий unit-квад, у каждой частицы
// только позиция/размер/цвет/поворот в отдельном instance-буфере,
// перезаливаемом раз в кадр. Частицы всегда развёрнуты к камере
// (billboard) — считается в шейдере через uCameraRight/uCameraUp.
//
// Использование:
//   ParticleSystem particles;
//   ...
//   particles.Burst(Presets::BlockBreak(), blockCenter, 12);
//   particles.CreateStream("stove_smoke", Presets::Smoke(), stovePos);
//   ...каждый кадр...
//   particles.SetStreamActive("stove_smoke", cookTimer > 0.0f);
//   particles.Update(dt);
//   particles.Draw(shader, camera, view, proj);
// ---------------------------------------------------------------------
class ParticleSystem {
public:
    ParticleSystem() : m_rng(std::random_device{}()) {
        SetupBuffers();
    }

    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // Жёсткий потолок живых частиц. Burst/EmissionRate доступны из Lua и из
    // сериализуемых сцен — без потолка EmitParticles(cfg, pos, 10^9) или битое
    // поле emissionRate исчерпали бы память/заморозили кадр. Новые частицы
    // сверх лимита просто не рождаются (старые доживают своё).
    static constexpr size_t kMaxParticles = 65536;

    // Разовый залп из `count` частиц в точке position по правилам config
    void Burst(const ParticleEmitterConfig& config, glm::vec3 position, int count) {
        for (int i = 0; i < count && m_particles.size() < kMaxParticles; ++i) {
            m_particles.push_back(SpawnOne(config, position));
        }
    }

    // Создаёт (или обновляет конфиг у уже существующего) непрерывный эмиттер
    // с заданным id. Стрим по умолчанию неактивен — включить через SetStreamActive.
    void CreateStream(const std::string& id, const ParticleEmitterConfig& config, glm::vec3 position) {
        StreamEmitter& stream = m_streams[id];
        stream.Config = config;
        stream.Position = position;
    }

    void SetStreamActive(const std::string& id, bool active) {
        auto it = m_streams.find(id);
        if (it != m_streams.end()) it->second.Active = active;
    }

    void SetStreamPosition(const std::string& id, glm::vec3 position) {
        auto it = m_streams.find(id);
        if (it != m_streams.end()) it->second.Position = position;
    }

    void RemoveStream(const std::string& id) { m_streams.erase(id); }

    // Обновляет все живые частицы (движение, гравитация, старение) и
    // рождает новые частицы из активных стримов согласно их EmissionRate.
    // Мёртвые частицы удаляются из пула в этом же вызове.
    void Update(float dt) {
        for (auto& [id, stream] : m_streams) {
            if (!stream.Active) continue;
            stream.SpawnAccumulator += stream.Config.EmissionRate * dt;
            // Потолок и на накопитель: безумный EmissionRate (битая сцена/скрипт)
            // не должен ни крутить цикл миллиарды итераций, ни копить «долг».
            stream.SpawnAccumulator = std::min(stream.SpawnAccumulator, 4096.0f);
            while (stream.SpawnAccumulator >= 1.0f && m_particles.size() < kMaxParticles) {
                m_particles.push_back(SpawnOne(stream.Config, stream.Position));
                stream.SpawnAccumulator -= 1.0f;
            }
            if (m_particles.size() >= kMaxParticles) stream.SpawnAccumulator = 0.0f;
        }

        for (Particle& p : m_particles) {
            p.Age += dt;
            p.Velocity.y += p.Gravity * dt;
            p.Position += p.Velocity * dt;
            p.Rotation += p.AngularVelocity * dt;
        }

        // erase-remove мёртвых частиц — порядок не важен, частицы независимы
        m_particles.erase(
            std::remove_if(m_particles.begin(), m_particles.end(), [](const Particle& p) { return !p.IsAlive(); }),
            m_particles.end());
    }

    // Заливает инстанс-буфер и рисует все живые частицы одним draw call'ом.
    // Ожидает, что глубина уже включена (частицы сортируются относительно
    // сцены), но сама отключает запись в depth buffer, чтобы полупрозрачные
    // частицы не перекрывали друг друга жёстко по глубине.
    // Базовая отрисовка: billboard-направления камеры задаются явно (camRight/
    // camUp в мировых координатах) — так частицы рисуются и там, где нет объекта
    // Camera (напр. игровое окно редактора от CameraComponent).
    void Draw(Shader& shader, glm::vec3 camRight, glm::vec3 camUp,
              const glm::mat4& view, const glm::mat4& proj) {
        if (m_particles.empty()) return;

        m_instanceScratch.clear();
        m_instanceScratch.reserve(m_particles.size());
        for (const Particle& p : m_particles) {
            InstanceData d;
            d.Position = p.Position;
            d.Size = p.CurrentSize();
            d.Color = p.CurrentColor();
            d.Rotation = p.Rotation;
            m_instanceScratch.push_back(d);
        }

        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetDepthWrite(false);
        device.SetBlend(true);

        shader.Use();
        shader.SetMat4("uView", view);
        shader.SetMat4("uProjection", proj);
        shader.SetVec3("uCameraRight", camRight);
        shader.SetVec3("uCameraUp", camUp);
        shader.SetInt("uShape", 0);

        m_geometry->SetInstanceData(m_instanceScratch.data(), m_instanceScratch.size() * sizeof(InstanceData));
        m_geometry->DrawInstanced(6, m_instanceScratch.size());

        device.SetDepthWrite(true);
    }

    void Draw(Shader& shader, const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
        Draw(shader, camera.Right, camera.Up, view, proj);
    }

    // Отрисовка встроенным шейдером с явными camRight/camUp (см. ниже перегрузку
    // от Camera). camRight/camUp обычно берут из строк матрицы вида.
    void Draw(glm::vec3 camRight, glm::vec3 camUp, const glm::mat4& view, const glm::mat4& proj) {
        Draw(BuiltinShader(), camRight, camUp, view, proj);
    }

    // Извлекает camRight/camUp прямо из матрицы вида (мировые оси камеры) — удобно
    // там, где есть только view/proj, без объекта Camera.
    void DrawFromView(const glm::mat4& view, const glm::mat4& proj) {
        glm::vec3 right = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
        glm::vec3 up = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
        Draw(BuiltinShader(), right, up, view, proj);
    }

    // Отрисовка ВСТРОЕННЫМ шейдером частиц (billboard + soft-circle/quad) — не
    // требует ассетного particle.vert/frag, поэтому частицы рисует кто угодно
    // (редактор/рантайм/игра) без своих шейдеров. Шейдер компилируется лениво.
    void Draw(const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
        Draw(BuiltinShader(), camera.Right, camera.Up, view, proj);
    }

    size_t AliveCount() const { return m_particles.size(); }
    size_t StreamCount() const { return m_streams.size(); }

private:
    // Встроенный шейдер частиц (общий, ленивая инициализация; определён в .cpp).
    static Shader& BuiltinShader();

    struct StreamEmitter {
        ParticleEmitterConfig Config;
        glm::vec3 Position{0.0f};
        bool Active = false;
        float SpawnAccumulator = 0.0f; // дробный остаток "сколько частиц должны были родиться"
    };

    // Layout per-instance буфера — должен точно соответствовать
    // атрибутам 1..4 в particle.vert (vec3 + float + vec4 + float)
    struct InstanceData {
        glm::vec3 Position;
        float Size;
        glm::vec4 Color;
        float Rotation;
    };

    Particle SpawnOne(const ParticleEmitterConfig& c, glm::vec3 position) {
        auto rand01 = [this] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); };
        auto randRange = [&](float lo, float hi) { return lo + (hi - lo) * rand01(); };

        Particle p;
        p.Position = position;

        glm::vec3 dir{
            randRange(c.DirectionMin.x, c.DirectionMax.x),
            randRange(c.DirectionMin.y, c.DirectionMax.y),
            randRange(c.DirectionMin.z, c.DirectionMax.z),
        };
        float dirLen = glm::length(dir);
        if (dirLen > 0.0001f) dir /= dirLen;
        p.Velocity = dir * randRange(c.SpeedMin, c.SpeedMax);
        p.Gravity = c.Gravity;

        p.Lifetime = randRange(c.LifetimeMin, c.LifetimeMax);
        p.Age = 0.0f;

        p.StartSize = randRange(c.StartSizeMin, c.StartSizeMax);
        p.EndSize = randRange(c.EndSizeMin, c.EndSizeMax);
        p.StartColor = c.StartColor;
        p.EndColor = c.EndColor;

        p.Rotation = randRange(0.0f, 6.2831853f);
        p.AngularVelocity = randRange(-c.AngularVelocityMax, c.AngularVelocityMax);

        return p;
    }

    void SetupBuffers() {
        // Единичный квад (-0.5..0.5), общий для всех частиц — вершинные позиции,
        // растягиваемые в billboard прямо в шейдере. Инстансный поток (атрибуты
        // 1..4, продвигаются раз на инстанс) должен точно соответствовать
        // particle.vert — см. InstanceData выше.
        float quad[] = {
            -0.5f, -0.5f,  0.5f, -0.5f,  0.5f, 0.5f,
            -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, 0.5f,
        };

        sage::rhi::VertexLayout layout;
        layout.Stride = 2 * sizeof(float);
        layout.Attributes = {{0, 2, sage::rhi::AttribType::Float, 0}};
        layout.InstanceStride = sizeof(InstanceData);
        layout.InstanceAttributes = {
            {1, 3, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Position)}, // iWorldPos
            {2, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Size)},     // iSize
            {3, 4, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Color)},    // iColor
            {4, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Rotation)}, // iRotation
        };

        m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);
        m_geometry->SetVertexData(quad, sizeof(quad), /*dynamic=*/false);
    }

    std::unique_ptr<sage::rhi::Geometry> m_geometry;

    std::vector<Particle> m_particles;
    std::unordered_map<std::string, StreamEmitter> m_streams;
    std::vector<InstanceData> m_instanceScratch; // переиспользуемый буфер — не аллоцируем каждый кадр

    std::mt19937 m_rng;
};
