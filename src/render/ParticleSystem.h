#pragma once
#include "Particle.h"
#include "Shader.h"
#include "Camera.h"
#include <glad/glad.h>
#include <vector>
#include <random>
#include <unordered_map>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------
// ParticleSystem — часть ядра рендера SAGE Engine, не завязана на The Boat
// или вообще на воксели: работает с произвольными мировыми позициями.
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
// Рендер — один draw call на ВСЕ живые частицы разом через инстансинг
// (glDrawArraysInstanced): геометрия — общий unit-квад, у каждой частицы
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

    ~ParticleSystem() {
        if (m_instanceVbo) glDeleteBuffers(1, &m_instanceVbo);
        if (m_quadVbo) glDeleteBuffers(1, &m_quadVbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
    }

    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // Разовый залп из `count` частиц в точке position по правилам config
    void Burst(const ParticleEmitterConfig& config, glm::vec3 position, int count) {
        for (int i = 0; i < count; ++i) {
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
            while (stream.SpawnAccumulator >= 1.0f) {
                m_particles.push_back(SpawnOne(stream.Config, stream.Position));
                stream.SpawnAccumulator -= 1.0f;
            }
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
    void Draw(Shader& shader, const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
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

        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        shader.Use();
        shader.SetMat4("uView", view);
        shader.SetMat4("uProjection", proj);
        shader.SetVec3("uCameraRight", camera.Right);
        shader.SetVec3("uCameraUp", camera.Up);
        shader.SetInt("uShape", 0);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, m_instanceScratch.size() * sizeof(InstanceData), m_instanceScratch.data(), GL_STREAM_DRAW);

        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)m_instanceScratch.size());

        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
    }

    size_t AliveCount() const { return m_particles.size(); }
    size_t StreamCount() const { return m_streams.size(); }

private:
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
        // растягиваемые в billboard прямо в шейдере
        float quad[] = {
            -0.5f, -0.5f,  0.5f, -0.5f,  0.5f, 0.5f,
            -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, 0.5f,
        };

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_quadVbo);
        glGenBuffers(1, &m_instanceVbo);

        glBindVertexArray(m_vao);

        glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

        glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo);
        GLsizei stride = sizeof(InstanceData);
        glEnableVertexAttribArray(1); // iWorldPos
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(InstanceData, Position));
        glEnableVertexAttribArray(2); // iSize
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(InstanceData, Size));
        glEnableVertexAttribArray(3); // iColor
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(InstanceData, Color));
        glEnableVertexAttribArray(4); // iRotation
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(InstanceData, Rotation));

        // divisor 1 — эти атрибуты продвигаются раз на ИНСТАНС, а не раз на вершину
        glVertexAttribDivisor(1, 1);
        glVertexAttribDivisor(2, 1);
        glVertexAttribDivisor(3, 1);
        glVertexAttribDivisor(4, 1);

        glBindVertexArray(0);
    }

    unsigned int m_vao = 0, m_quadVbo = 0, m_instanceVbo = 0;

    std::vector<Particle> m_particles;
    std::unordered_map<std::string, StreamEmitter> m_streams;
    std::vector<InstanceData> m_instanceScratch; // переиспользуемый буфер — не аллоцируем каждый кадр

    std::mt19937 m_rng;
};
