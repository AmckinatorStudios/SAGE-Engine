#include "sage/render/ParticleSim.h"

#include <algorithm>
#include <cmath>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace sage::fx {

namespace {

constexpr size_t kHardCap = 65536;   // потолок на эмиттер: битый файл не съест память
constexpr float kGravity = 9.81f;

// --- Шум ---------------------------------------------------------------------
float Hash(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFF * 2.0f - 1.0f;
}

float ValueNoise(const glm::vec3& p) {
    const glm::vec3 f = glm::floor(p);
    const glm::ivec3 i(f);
    glm::vec3 u = p - f;
    u = u * u * (3.0f - 2.0f * u);   // гладкая стыковка ячеек — без изломов силы
    auto h = [&](int dx, int dy, int dz) { return Hash(i.x + dx, i.y + dy, i.z + dz); };
    const float x00 = glm::mix(h(0, 0, 0), h(1, 0, 0), u.x), x10 = glm::mix(h(0, 1, 0), h(1, 1, 0), u.x);
    const float x01 = glm::mix(h(0, 0, 1), h(1, 0, 1), u.x), x11 = glm::mix(h(0, 1, 1), h(1, 1, 1), u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
}

glm::vec3 RandomUnit(float a, float b) {
    const float z = 2.0f * a - 1.0f;
    const float phi = glm::two_pi<float>() * b;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

// Поворот (без масштаба) мировой матрицы: направления и силы переводятся им.
glm::mat3 RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(r[i]);
        if (len > 1e-6f) r[i] /= len;
    }
    return r;
}

float Speed01(float speed, const Range& r) {
    const float span = r.Max - r.Min;
    return span > 1e-6f ? glm::clamp((speed - r.Min) / span, 0.0f, 1.0f) : 0.0f;
}

} // namespace

glm::vec3 NoiseField(const glm::vec3& p, float time, int octaves) {
    // Три независимых канала — сдвинутые копии одного поля; время сдвигает
    // поле по диагонали, и завихрения «текут», а не мигают.
    glm::vec3 sum(0.0f);
    float amp = 1.0f, freq = 1.0f, norm = 0.0f;
    octaves = std::clamp(octaves, 1, 6);
    for (int o = 0; o < octaves; ++o) {
        const glm::vec3 q = p * freq + glm::vec3(time * 0.37f, time * 0.23f, -time * 0.31f);
        sum += amp * glm::vec3(ValueNoise(q + glm::vec3(17.1f, 3.3f, 5.7f)),
                               ValueNoise(q + glm::vec3(-9.2f, 11.4f, 1.9f)),
                               ValueNoise(q + glm::vec3(4.4f, -7.8f, 13.6f)));
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

// --- Форма испускания ----------------------------------------------------------

void ParticleEmitter::SampleShape(const ParticleEffect& fx, glm::vec3& pos, glm::vec3& dir) {
    const float arc = glm::radians(glm::clamp(fx.Arc, 0.0f, 360.0f));
    const float R = std::max(fx.Radius, 0.0f);
    switch (fx.Shape) {
        case EmitShape::Point:
            pos = glm::vec3(0.0f);
            dir = RandomUnit(Rand(), Rand());
            break;
        case EmitShape::Sphere:
        case EmitShape::Hemisphere: {
            const float phi = arc * Rand();
            const float cosT = fx.Shape == EmitShape::Sphere ? 2.0f * Rand() - 1.0f : Rand();
            const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
            dir = glm::vec3(sinT * std::cos(phi), cosT, sinT * std::sin(phi));
            // Равномерно по объёму — кубический корень: иначе частицы
            // скапливаются у центра, и «шар» выглядит точкой с ореолом.
            const float r = fx.FromShell ? R : R * std::cbrt(Rand());
            pos = dir * r;
            break;
        }
        case EmitShape::Cone: {
            const float phi = arc * Rand();
            const float u = fx.FromShell ? 1.0f : std::sqrt(Rand());
            const float rr = R * u;
            pos = glm::vec3(rr * std::cos(phi), 0.0f, rr * std::sin(phi));
            // Наклон растёт от центра к краю основания: струя расходится, а не
            // летит пучком параллельных линий. У конуса из точки — случайный.
            const float frac = R > 1e-5f ? u : std::sqrt(Rand());
            const float a = glm::radians(glm::clamp(fx.ConeAngle, 0.0f, 90.0f)) * frac;
            dir = glm::vec3(std::sin(a) * std::cos(phi), std::cos(a), std::sin(a) * std::sin(phi));
            break;
        }
        case EmitShape::Box: {
            glm::vec3 u(Rand() - 0.5f, Rand() - 0.5f, Rand() - 0.5f);
            if (fx.FromShell) {
                const int axis = (int)(Rand() * 3.0f) % 3;
                u[axis] = Rand() < 0.5f ? -0.5f : 0.5f;
            }
            pos = u * fx.BoxSize;
            dir = glm::vec3(0.0f, 1.0f, 0.0f);
            break;
        }
        case EmitShape::Circle: {
            const float phi = arc * Rand();
            const float rr = fx.FromShell ? R : R * std::sqrt(Rand());
            dir = glm::vec3(std::cos(phi), 0.0f, std::sin(phi));
            pos = dir * rr;
            break;
        }
        case EmitShape::Edge:
            pos = glm::vec3((Rand() - 0.5f) * fx.EdgeLength, 0.0f, 0.0f);
            dir = glm::vec3(0.0f, 1.0f, 0.0f);
            break;
    }
    if (fx.RandomizeDirection > 0.0f) {
        const glm::vec3 r = RandomUnit(Rand(), Rand());
        const glm::vec3 d = glm::mix(dir, r, glm::clamp(fx.RandomizeDirection, 0.0f, 1.0f));
        dir = glm::length(d) > 1e-5f ? glm::normalize(d) : r;
    }
}

// --- Рождение ------------------------------------------------------------------

int ParticleEmitter::FrameCount(const ParticleEffect& fx) {
    if (!fx.Frames.empty()) return (int)fx.Frames.size();
    if (fx.Texture.empty()) return 1;
    return std::max(1, fx.TilesX) * std::max(1, fx.TilesY);
}

void ParticleEmitter::SpawnOne(const ParticleEffect& fx, const glm::mat4& world, float preAge,
                               const glm::vec4& tint, const glm::vec3& addVelocity) {
    const size_t cap = std::min((size_t)std::max(fx.MaxParticles, 0), kHardCap);
    if (m_particles.size() >= cap) return;

    glm::vec3 pos, dir;
    SampleShape(fx, pos, dir);
    const glm::mat4 shapeXf =
        glm::translate(glm::mat4(1.0f), fx.ShapeOffset) *
        glm::eulerAngleXYZ(glm::radians(fx.ShapeRotation.x), glm::radians(fx.ShapeRotation.y),
                           glm::radians(fx.ShapeRotation.z));
    pos = glm::vec3(shapeXf * glm::vec4(pos, 1.0f));
    dir = glm::mat3(shapeXf) * dir;

    const glm::mat3 rot = RotationOf(world);
    Particle p;
    if (fx.Space == SimulationSpace::World) {
        p.Position = glm::vec3(world * glm::vec4(pos, 1.0f));
        dir = rot * dir;
        p.Velocity = dir * fx.StartSpeed.Lerp(Rand()) + m_emitterVelocity * fx.InheritVelocity + addVelocity;
    } else {
        p.Position = pos;
        p.Velocity = dir * fx.StartSpeed.Lerp(Rand()) +
                     glm::transpose(rot) * (m_emitterVelocity * fx.InheritVelocity + addVelocity);
    }
    p.Lifetime = std::max(fx.StartLifetime.Lerp(Rand()), 0.01f);
    p.Size = std::max(fx.StartSize.Lerp(Rand()), 0.0f);
    p.Color = glm::mix(fx.StartColorA, fx.StartColorB, Rand()) * tint;
    p.Rotation = glm::radians(fx.StartRotation.Lerp(Rand()));
    p.AngularVelocity = fx.UseRotation ? glm::radians(fx.AngularVelocity.Lerp(Rand())) : 0.0f;
    p.Seed = Rand();
    p.Frame = std::min((int)(Rand() * (float)FrameCount(fx)), FrameCount(fx) - 1);
    p.Moved = p.Velocity;
    // Родилась ВНУТРИ шага, а не в его начале: при редком кадре иначе все
    // частицы шага вылетают одним сгустком, и струя становится пунктиром.
    p.Age = preAge;
    p.Position += p.Velocity * preAge;

    for (const SubEmitter& s : fx.SubEmitters) {
        if (s.When != SubEmitter::Trigger::Birth) continue;
        const glm::vec3 wp = fx.Space == SimulationSpace::World ? p.Position
                                                                 : glm::vec3(world * glm::vec4(p.Position, 1.0f));
        const glm::vec3 wv = fx.Space == SimulationSpace::World ? p.Velocity : rot * p.Velocity;
        m_events.push_back({SubEmitter::Trigger::Birth, wp, wv, p.Color});
        break;
    }
    m_particles.push_back(std::move(p));
}

void ParticleEmitter::Emit(const ParticleEffect& fx, const glm::mat4& emitterWorld, int count,
                           const glm::vec4& tint, const glm::vec3& addVelocity) {
    if (!m_hasLast) {
        m_world = emitterWorld;
        m_lastPos = glm::vec3(emitterWorld[3]);
        m_hasLast = true;
    }
    count = std::clamp(count, 0, (int)kHardCap);
    for (int i = 0; i < count; ++i) SpawnOne(fx, emitterWorld, 0.0f, tint, addVelocity);
}

void ParticleEmitter::Play() {
    m_playing = true;
    m_finished = false;
    m_started = false;
    m_time = 0.0f;
    m_rateAccum = m_distAccum = 0.0f;
}

void ParticleEmitter::Stop(bool clear) {
    m_playing = false;
    if (clear) m_particles.clear();
}

bool ParticleEmitter::Alive() const {
    return !m_particles.empty() || (m_playing && !m_finished);
}

// --- Шаг -------------------------------------------------------------------------

void ParticleEmitter::Step(const ParticleEffect& fx, const glm::mat4& emitterWorld, float dt,
                           const CollisionQuery* world) {
    m_events.clear();
    dt *= std::max(fx.SimulationSpeed, 0.0f);
    if (!(dt > 0.0f)) return;
    dt = std::min(dt, 0.25f);   // провал кадра не должен выстреливать сотню частиц разом

    const glm::vec3 pos = glm::vec3(emitterWorld[3]);
    const glm::vec3 lastPos = m_hasLast ? m_lastPos : pos;
    m_emitterVelocity = m_hasLast ? (pos - lastPos) / dt : glm::vec3(0.0f);
    m_world = emitterWorld;

    if (!m_started) {
        m_started = true;
        m_delay = std::max(fx.StartDelay, 0.0f);
        m_burstDone.assign(fx.Bursts.size(), 0);
        // РАЗОГРЕВ: костёр в начале уровня уже горит, а не разгорается на
        // глазах. Прокручиваем один цикл заранее мелкими шагами.
        if (fx.Prewarm && fx.Loop && m_playing && fx.Duration > 0.0f) {
            m_hasLast = true;
            m_lastPos = pos;
            const float step = 1.0f / 30.0f;
            for (float t = 0.0f; t < fx.Duration; t += step) {
                Simulate(fx, step, nullptr);
                m_rateAccum += fx.RateOverTime * step;
                while (m_rateAccum >= 1.0f) {
                    SpawnOne(fx, emitterWorld, 0.0f);
                    m_rateAccum -= 1.0f;
                }
                m_total += step;
            }
            m_events.clear();
        }
    }

    Simulate(fx, dt, world);

    if (m_playing && !m_finished) {
        float emitDt = dt;
        if (m_delay > 0.0f) {
            const float spend = std::min(m_delay, emitDt);
            m_delay -= spend;
            emitDt -= spend;
        }
        if (emitDt > 0.0f) {
            // По времени.
            m_rateAccum += std::max(fx.RateOverTime, 0.0f) * emitDt;
            m_rateAccum = std::min(m_rateAccum, (float)kHardCap);
            const int n = (int)m_rateAccum;
            m_rateAccum -= (float)n;
            for (int i = 0; i < n; ++i) SpawnOne(fx, emitterWorld, emitDt * (n - i - 0.5f) / (float)n);

            // По пройденному пути — вдоль пути, а не кучкой в конце: след за
            // бегущим персонажем при низкой частоте кадров иначе рвётся.
            if (fx.RateOverDistance > 0.0f) {
                m_distAccum += fx.RateOverDistance * glm::length(pos - lastPos);
                m_distAccum = std::min(m_distAccum, (float)kHardCap);
                const int m = (int)m_distAccum;
                m_distAccum -= (float)m;
                for (int i = 0; i < m; ++i) {
                    glm::mat4 at = emitterWorld;
                    at[3] = glm::vec4(glm::mix(lastPos, pos, (i + 0.5f) / (float)m), 1.0f);
                    SpawnOne(fx, at, 0.0f);
                }
            }

            // Залпы.
            if (m_burstDone.size() != fx.Bursts.size()) m_burstDone.assign(fx.Bursts.size(), 0);
            const float end = m_time + emitDt;
            for (size_t b = 0; b < fx.Bursts.size(); ++b) {
                const Burst& burst = fx.Bursts[b];
                const float interval = std::max(burst.Interval, 0.01f);
                while ((burst.Cycles <= 0 || m_burstDone[b] < burst.Cycles) &&
                       burst.Time + m_burstDone[b] * interval < end &&
                       m_burstDone[b] < 100000) {
                    const int lo = std::min(burst.CountMin, burst.CountMax);
                    const int hi = std::max(burst.CountMin, burst.CountMax);
                    const int count = lo + (int)(Rand() * (float)(hi - lo + 1));
                    for (int i = 0; i < std::min(count, hi); ++i) SpawnOne(fx, emitterWorld, 0.0f);
                    ++m_burstDone[b];
                    if (burst.Cycles <= 0 && burst.Time + m_burstDone[b] * interval >= fx.Duration) break;
                }
            }

            m_time = end;
            if (fx.Duration > 0.0f && m_time >= fx.Duration) {
                if (fx.Loop) {
                    m_time = std::fmod(m_time, fx.Duration);
                    std::fill(m_burstDone.begin(), m_burstDone.end(), 0);
                } else {
                    m_finished = true;
                }
            }
        }
    }

    m_total += dt;
    m_lastPos = pos;
    m_hasLast = true;
}

void ParticleEmitter::Simulate(const ParticleEffect& fx, float dt, const CollisionQuery* worldQuery) {
    const bool local = fx.Space == SimulationSpace::Local;
    const glm::mat3 rot = RotationOf(m_world);
    const glm::mat3 inv = glm::transpose(rot);
    const glm::vec3 origin = local ? glm::vec3(0.0f) : glm::vec3(m_world[3]);
    const glm::vec3 up = local ? glm::vec3(0.0f, 1.0f, 0.0f) : rot * glm::vec3(0.0f, 1.0f, 0.0f);
    auto toSim = [&](const glm::vec3& worldVec) { return local ? inv * worldVec : worldVec; };
    auto toWorldPos = [&](const glm::vec3& p) { return local ? glm::vec3(m_world * glm::vec4(p, 1.0f)) : p; };
    auto toWorldDir = [&](const glm::vec3& v) { return local ? rot * v : v; };

    const glm::vec3 gravity = toSim(glm::vec3(0.0f, -kGravity * fx.GravityScale, 0.0f));
    const glm::vec3 force = fx.UseForces ? toSim(fx.Force) : glm::vec3(0.0f);
    const glm::vec3 windBase = fx.UseForces ? toSim(fx.Wind) : glm::vec3(0.0f);
    const bool windOn = fx.UseForces && glm::length(fx.Wind) > 1e-5f;
    const bool trails = fx.Render == RenderMode::Trail;
    const float time = m_total;
    const bool needDeath = std::any_of(fx.SubEmitters.begin(), fx.SubEmitters.end(),
                                       [](const SubEmitter& s) { return s.When == SubEmitter::Trigger::Death; });

    size_t alive = 0;
    for (size_t i = 0; i < m_particles.size(); ++i) {
        Particle& p = m_particles[i];
        const float t = p.T();

        glm::vec3 accel = gravity + force;
        if (windOn) {
            // Порывы: ветер дышит во времени и чуть по-разному в разных местах,
            // иначе снег сносит одним жёстким листом.
            const float gust = 1.0f + glm::clamp(fx.Gustiness, 0.0f, 1.0f) *
                                          NoiseField(toWorldPos(p.Position) * 0.15f, time * 0.8f, 1).x * 1.6f;
            accel += (windBase * gust - p.Velocity) * std::max(fx.WindInfluence, 0.0f);
        }
        if (fx.UseForces && fx.Attraction != 0.0f) {
            const glm::vec3 d = origin - p.Position;
            const float len = glm::length(d);
            if (len > 1e-4f) accel += d / len * fx.Attraction;
        }
        if (fx.UseNoise && fx.NoiseStrength != 0.0f) {
            accel += toSim(NoiseField(toWorldPos(p.Position) * std::max(fx.NoiseFrequency, 0.0f),
                                      time * fx.NoiseScroll, fx.NoiseOctaves)) *
                     fx.NoiseStrength * 2.0f;
        }
        p.Velocity += accel * dt;
        if (fx.UseForces && fx.Drag > 0.0f) p.Velocity /= (1.0f + fx.Drag * dt);
        if (fx.UseForces && fx.MaxSpeed > 0.0f) {
            const float s = glm::length(p.Velocity);
            if (s > fx.MaxSpeed) p.Velocity *= fx.MaxSpeed / s;
        }

        glm::vec3 vel = p.Velocity;
        if (fx.UseVelocity) {
            vel *= fx.SpeedOverLifetime.Evaluate(t);
            vel += fx.LinearVelocity;   // в осях симуляции: у Local — в осях объекта
            glm::vec3 r = p.Position - origin;
            r -= up * glm::dot(r, up);
            if (fx.Orbital != 0.0f) vel += glm::cross(up, r) * glm::radians(fx.Orbital);
            if (fx.Radial != 0.0f && glm::length(r) > 1e-4f) vel += glm::normalize(r) * fx.Radial;
        }

        const glm::vec3 from = p.Position;
        glm::vec3 to = from + vel * dt;

        if (fx.UseCollision) {
            const glm::vec3 wFrom = toWorldPos(from), wTo = toWorldPos(to);
            const float r = std::max(fx.CollisionRadius, 0.0f);
            bool hit = false;
            CollisionHit h;
            if (fx.Collision == CollisionMode::Plane) {
                const float plane = fx.PlaneHeight + r;
                if (wFrom.y >= plane - 1e-4f && wTo.y < plane) {
                    const float span = wFrom.y - wTo.y;
                    const float u = span > 1e-6f ? (wFrom.y - plane) / span : 0.0f;
                    h.Point = glm::mix(wFrom, wTo, u);
                    h.Point.y = fx.PlaneHeight;
                    h.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
                    hit = true;
                }
            } else if (worldQuery && *worldQuery && glm::length(wTo - wFrom) > 1e-6f) {
                const glm::vec3 dir = glm::normalize(wTo - wFrom);
                hit = (*worldQuery)(wFrom, wTo + dir * r, h);
            }
            if (hit) {
                const glm::vec3 n = glm::normalize(h.Normal);
                glm::vec3 v = toWorldDir(p.Velocity);
                const glm::vec3 vn = n * glm::dot(v, n);
                const glm::vec3 vt = v - vn;
                // Отскок — только если летела В поверхность: частица, уже
                // отскочившая, не должна «прилипать» при следующей проверке.
                if (glm::dot(v, n) < 0.0f)
                    v = vt * (1.0f - glm::clamp(fx.Friction, 0.0f, 1.0f)) - vn * std::max(fx.Bounce, 0.0f);
                p.Velocity = local ? inv * v : v;
                const glm::vec3 wp = h.Point + n * (r + 1e-3f);
                to = local ? glm::vec3(glm::inverse(m_world) * glm::vec4(wp, 1.0f)) : wp;
                p.Age += p.Lifetime * glm::clamp(fx.LifetimeLoss, 0.0f, 1.0f);
                for (const SubEmitter& s : fx.SubEmitters) {
                    if (s.When != SubEmitter::Trigger::Collision) continue;
                    m_events.push_back({SubEmitter::Trigger::Collision, wp, v, p.Color});
                    break;
                }
            }
        }

        p.Moved = (to - from) / dt;
        p.Position = to;
        const float spin = fx.UseRotation ? fx.RotationOverLifetime.Evaluate(t) : 1.0f;
        p.Rotation += p.AngularVelocity * spin * dt;
        p.Age += dt;

        if (trails) {
            const glm::vec3 wp = toWorldPos(p.Position);
            const float minDist = std::max(fx.TrailMinDistance, 0.0f);
            if (p.Trail.empty() || glm::length(glm::vec3(p.Trail.back()) - wp) >= minDist)
                p.Trail.push_back(glm::vec4(wp, time));
            const float keep = std::max(fx.TrailLifetime, 0.0f);
            size_t drop = 0;
            while (drop < p.Trail.size() && time - p.Trail[drop].w > keep) ++drop;
            const size_t maxPts = (size_t)std::clamp(fx.TrailMaxPoints, 2, 256);
            if (p.Trail.size() - drop > maxPts) drop = p.Trail.size() - maxPts;
            if (drop) p.Trail.erase(p.Trail.begin(), p.Trail.begin() + (long)drop);
        }

        if (p.Age >= p.Lifetime) {
            if (needDeath)
                m_events.push_back({SubEmitter::Trigger::Death, toWorldPos(p.Position),
                                    toWorldDir(p.Velocity), ColorOf(fx, p)});
            continue;
        }
        if (alive != i) m_particles[alive] = std::move(p);
        ++alive;
    }
    m_particles.resize(alive);
}

// --- Внешний вид ------------------------------------------------------------------

float ParticleEmitter::SizeOf(const ParticleEffect& fx, const Particle& p) {
    float s = p.Size;
    if (fx.UseSizeOverLifetime) s *= fx.SizeOverLifetime.Evaluate(p.T());
    if (fx.UseSizeBySpeed) s *= fx.SizeBySpeed.Evaluate(Speed01(glm::length(p.Moved), fx.SizeBySpeedRange));
    return std::max(s, 0.0f);
}

glm::vec4 ParticleEmitter::ColorOf(const ParticleEffect& fx, const Particle& p) {
    glm::vec4 c = p.Color;
    if (fx.UseColorOverLifetime) c *= fx.ColorOverLifetime.Evaluate(p.T());
    if (fx.UseColorBySpeed) c *= fx.ColorBySpeed.Evaluate(Speed01(glm::length(p.Moved), fx.ColorBySpeedRange));
    return c;
}

int ParticleEmitter::FrameOf(const ParticleEffect& fx, const Particle& p, int frames) {
    if (frames <= 1) return 0;
    const int offset = fx.RandomStartFrame ? p.Frame : std::max(fx.StartFrame, 0);
    int idx = 0;
    switch (fx.Flipbook) {
        case FlipbookMode::OverLifetime: {
            const float f = p.T() * std::max(fx.Cycles, 0.0f) * (float)frames;
            // Последнее мгновение жизни — последний кадр, а не снова первый.
            idx = p.T() >= 1.0f && fx.Cycles <= 1.0f ? frames - 1 : std::min((int)f, INT32_MAX / 2);
            idx += offset;
            break;
        }
        case FlipbookMode::Speed:
            idx = (int)(p.Age * std::max(fx.FrameRate, 0.0f)) + offset;
            break;
        case FlipbookMode::Random:
            idx = p.Frame;
            break;
        case FlipbookMode::Fixed:
            idx = std::max(fx.StartFrame, 0);
            break;
    }
    return ((idx % frames) + frames) % frames;
}

} // namespace sage::fx
