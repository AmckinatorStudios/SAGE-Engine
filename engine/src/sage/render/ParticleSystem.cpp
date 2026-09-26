#include "sage/render/ParticleSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/Camera.h"
#include "sage/render/MeshData.h"
#include "sage/render/ParticleEffectIO.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Shader.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::fx;

namespace {

// --- Шейдеры ------------------------------------------------------------------
//
// Один шейдер квадратов на четыре режима (к камере, по скорости, лёжа, стоя):
// отличаются они только тем, какие две оси натягивают квадрат, и держать
// четыре программы ради одной строки значило бы четыре места для ошибки.

const char* kQuadVert = R"(#version 330 core
layout (location = 0) in vec2 aCorner;   // -0.5..0.5
layout (location = 1) in vec3 iPos;
layout (location = 2) in float iSize;
layout (location = 3) in vec4 iColor;
layout (location = 4) in float iRot;
layout (location = 5) in float iFrame;
layout (location = 6) in vec3 iVel;
layout (location = 7) in float iSeed;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform vec3 uCameraPos;
uniform int uMode;           // 0 к камере, 1 по скорости, 2 лёжа, 3 стоя
uniform int uAlign;          // повернуть по направлению движения на экране
uniform float uStretchLength;
uniform float uStretchBySpeed;
uniform vec2 uTiles;

out vec4 vColor;
out vec2 vLocal;
out vec2 vUV;

void main() {
    vec3 right = uCameraRight;
    vec3 up = uCameraUp;
    float rot = iRot;
    float sx = iSize, sy = iSize;
    vec2 c = aCorner;
    vec3 center = iPos;
    if (uMode == 1) {
        // Вытянут вдоль скорости; голова — в точке частицы, хвост позади.
        float sp = length(iVel);
        vec3 dir = sp > 1e-4 ? iVel / sp : uCameraUp;
        vec3 side = cross(dir, normalize(uCameraPos - iPos));
        float sl = length(side);
        right = sl > 1e-4 ? side / sl : uCameraRight;
        up = dir;
        rot = 0.0;
        sy = iSize * max(uStretchLength + uStretchBySpeed * sp, 0.05);
        center = iPos - dir * (0.5 * sy - 0.5 * iSize);
    } else if (uMode == 2) {
        right = vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 0.0, -1.0);
    } else if (uMode == 3) {
        up = vec3(0.0, 1.0, 0.0);
        vec3 r = vec3(uCameraRight.x, 0.0, uCameraRight.z);
        right = length(r) > 1e-4 ? normalize(r) : vec3(1.0, 0.0, 0.0);
    }
    if (uAlign == 1 && uMode != 1) {
        float vx = dot(iVel, right), vy = dot(iVel, up);
        if (vx * vx + vy * vy > 1e-8) rot += atan(vy, vx) - 1.5707963;
    }
    float cr = cos(rot), sr = sin(rot);
    vec2 r2 = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
    vec3 world = center + right * (r2.x * sx) + up * (r2.y * sy);
    gl_Position = uProjection * uView * vec4(world, 1.0);
    vColor = iColor;
    vLocal = aCorner * 2.0;
    // Кадр раскадровки: листы считаются сверху вниз, как их рисуют.
    float f = floor(iFrame + 0.5);
    float col = mod(f, uTiles.x);
    float row = floor(f / uTiles.x);
    vUV = vec2((col + aCorner.x + 0.5) / uTiles.x, (uTiles.y - 1.0 - row + aCorner.y + 0.5) / uTiles.y);
}
)";

const char* kQuadFrag = R"(#version 330 core
in vec4 vColor;
in vec2 vLocal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTex;
uniform int uHasTex;
uniform int uSprite;       // 0 мягкий круг, 1 круг, 2 квадрат
uniform int uPremultiply;
uniform float uIntensity;

void main() {
    vec4 c = vColor;
    if (uHasTex == 1) {
        c *= texture(uTex, vUV);
    } else {
        float d = length(vLocal);
        if (uSprite == 0) {
            if (d > 1.0) discard;
            c.a *= smoothstep(1.0, 0.25, d);
        } else if (uSprite == 1) {
            if (d > 1.0) discard;
        }
    }
    c.rgb *= uIntensity;
    if (c.a < 0.003) discard;
    if (uPremultiply == 1) c.rgb *= c.a;
    FragColor = c;
}
)";

const char* kMeshVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 iPos;
layout (location = 4) in float iSize;
layout (location = 5) in vec4 iColor;
layout (location = 6) in float iRot;
layout (location = 7) in float iFrame;
layout (location = 8) in vec3 iVel;
layout (location = 9) in float iSeed;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uMeshScale;
uniform int uAlign;
uniform vec2 uTiles;

out vec4 vColor;
out vec3 vNormal;
out vec2 vUV;

mat3 AxisAngle(vec3 a, float ang) {
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    return mat3(t * a.x * a.x + c,       t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c,       t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

void main() {
    // Ось вращения — своя у каждой частицы (из её постоянного случайного
    // числа): осколки кувыркаются вразнобой, а не синхронно, как на вертеле.
    vec3 axis = normalize(vec3(sin(iSeed * 91.3), cos(iSeed * 47.1), sin(iSeed * 13.7 + 1.0)) + vec3(1e-4));
    mat3 R = AxisAngle(axis, iRot);
    if (uAlign == 1 && length(iVel) > 1e-4) {
        vec3 y = normalize(iVel);
        vec3 x = abs(y.y) < 0.99 ? normalize(cross(vec3(0.0, 1.0, 0.0), y)) : vec3(1.0, 0.0, 0.0);
        vec3 z = cross(x, y);
        R = mat3(x, y, z) * AxisAngle(vec3(0.0, 1.0, 0.0), iRot);
    }
    vec3 world = iPos + R * (aPos * uMeshScale) * iSize;
    gl_Position = uProjection * uView * vec4(world, 1.0);
    vNormal = R * aNormal;
    vColor = iColor;
    float f = floor(iFrame + 0.5);
    float col = mod(f, uTiles.x);
    float row = floor(f / uTiles.x);
    vUV = vec2((col + aUV.x) / uTiles.x, (uTiles.y - 1.0 - row + aUV.y) / uTiles.y);
}
)";

const char* kMeshFrag = R"(#version 330 core
in vec4 vColor;
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTex;
uniform int uHasTex;
uniform int uPremultiply;
uniform float uIntensity;

void main() {
    vec4 c = vColor;
    if (uHasTex == 1) c *= texture(uTex, vUV);
    // Свет свой и мягкий: у фигурки должны читаться грани, а освещение сцены
    // для осколка на полсекунды — лишняя работа.
    vec3 n = normalize(vNormal);
    float lit = 0.45 + 0.55 * max(dot(n, normalize(vec3(0.4, 1.0, 0.3))), 0.0);
    c.rgb *= lit * uIntensity;
    if (c.a < 0.003) discard;
    if (uPremultiply == 1) c.rgb *= c.a;
    FragColor = c;
}
)";

const char* kTrailVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in vec4 aColor;
uniform mat4 uView;
uniform mat4 uProjection;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
}
)";

const char* kTrailFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uPremultiply;
uniform float uIntensity;
void main() {
    vec4 c = vColor;
    if (uHasTex == 1) {
        c *= texture(uTex, vUV);
    } else {
        // Без картинки лента мягкая по краям: жёсткий край читается полосой.
        float e = abs(vUV.y * 2.0 - 1.0);
        c.a *= 1.0 - e * e;
    }
    c.rgb *= uIntensity;
    if (c.a < 0.003) discard;
    if (uPremultiply == 1) c.rgb *= c.a;
    FragColor = c;
}
)";

Shader& QuadShader() {
    // Намеренно НЕ уничтожается: деструктор статика сработал бы после смерти
    // GL-контекста.
    static Shader* s = new Shader(Shader::FromSource(kQuadVert, kQuadFrag, "Particles"));
    return *s;
}
Shader& MeshShader() {
    static Shader* s = new Shader(Shader::FromSource(kMeshVert, kMeshFrag, "ParticleMeshes"));
    return *s;
}
Shader& TrailShader() {
    static Shader* s = new Shader(Shader::FromSource(kTrailVert, kTrailFrag, "ParticleTrails"));
    return *s;
}

struct InstanceData {
    glm::vec3 Position;
    float Size;
    glm::vec4 Color;
    float Rotation;
    float Frame;
    glm::vec3 Velocity;
    float Seed;
};

struct TrailVertex {
    glm::vec3 Position;
    glm::vec2 UV;
    glm::vec4 Color;
};

std::vector<sage::rhi::VertexAttribute> InstanceAttributes(int first) {
    return {
        {first + 0, 3, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Position)},
        {first + 1, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Size)},
        {first + 2, 4, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Color)},
        {first + 3, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Rotation)},
        {first + 4, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Frame)},
        {first + 5, 3, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Velocity)},
        {first + 6, 1, sage::rhi::AttribType::Float, (int)offsetof(InstanceData, Seed)},
    };
}

sage::render::MeshData BuildTetrahedron() {
    const glm::vec3 p[4] = {{0.0f, 0.6f, 0.0f}, {-0.5f, -0.3f, 0.35f}, {0.5f, -0.3f, 0.35f}, {0.0f, -0.3f, -0.55f}};
    const int f[4][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 1}, {1, 3, 2}};
    sage::render::MeshData d;
    for (const auto& tri : f) {
        const glm::vec3 n = glm::normalize(glm::cross(p[tri[1]] - p[tri[0]], p[tri[2]] - p[tri[0]]));
        const glm::vec2 uv[3] = {{0.5f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}};
        for (int k = 0; k < 3; ++k) {
            d.Indices.push_back((unsigned)d.Vertices.size());
            d.Vertices.push_back({p[tri[k]], n, uv[k]});
        }
    }
    return d;
}

} // namespace

// --- Эмиттер внутри системы ----------------------------------------------------------

struct ParticleSystemInstance {
    ParticleEffect Fx;
    ParticleEmitter Sim;
    glm::mat4 World{1.0f};
    bool Synced = false;
    bool Orphan = false;
    int Depth = 0;
    // Дочерние эмиттеры — по одному на запись SubEmitters; пусто, пока не нужен.
    std::vector<std::unique_ptr<ParticleSystemInstance>> Subs;
    std::vector<bool> SubMissing;

    explicit ParticleSystemInstance(uint32_t seed) : Sim(seed) {}

    bool AliveTree() const {
        if (Sim.Alive()) return true;
        for (const auto& s : Subs)
            if (s && s->AliveTree()) return true;
        return false;
    }
    size_t CountTree() const {
        size_t n = Sim.Particles().size();
        for (const auto& s : Subs)
            if (s) n += s->CountTree();
        return n;
    }
};

struct TextureEntry {
    std::shared_ptr<Texture> Tex;
    glm::vec2 Tiles{1.0f, 1.0f};
};

struct ParticleSystem::Impl {
    std::unordered_map<uint64_t, std::unique_ptr<ParticleSystemInstance>> Entities;
    std::unordered_map<std::string, std::unique_ptr<ParticleSystemInstance>> Streams;
    std::unordered_map<std::string, bool> StreamActive;
    std::vector<std::unique_ptr<ParticleSystemInstance>> OneShots;
    std::unordered_map<uint64_t, int> PendingEmit;
    CollisionQuery Collision;
    std::function<bool(const std::string&, ParticleEffect&)> Loader;
    std::unordered_map<std::string, ParticleEffect> EffectCache;
    std::unordered_set<std::string> EffectMissing;
    uint32_t NextSeed = 0x9E3779B9u;

    // GPU — лениво, при первой отрисовке.
    std::unique_ptr<sage::rhi::Geometry> Quad;
    std::unique_ptr<sage::rhi::Geometry> Trails;
    std::unique_ptr<sage::rhi::Geometry> Meshes[4];
    size_t MeshIndexCount[4] = {0, 0, 0, 0};
    std::unordered_map<std::string, TextureEntry> Textures;
    std::vector<InstanceData> Scratch;
    std::vector<TrailVertex> TrailScratch;
    std::vector<uint32_t> Order;

    uint32_t Seed() {
        NextSeed = NextSeed * 1664525u + 1013904223u;
        return NextSeed;
    }

    std::unique_ptr<ParticleSystemInstance> Make(const ParticleEffect& fx, int depth = 0) {
        auto inst = std::make_unique<ParticleSystemInstance>(Seed());
        inst->Fx = fx;
        inst->Depth = depth;
        return inst;
    }

    const ParticleEffect* LoadEffect(const std::string& path) {
        auto it = EffectCache.find(path);
        if (it != EffectCache.end()) return &it->second;
        if (EffectMissing.count(path)) return nullptr;
        ParticleEffect fx;
        const bool ok = Loader ? Loader(path, fx) : false;
        if (!ok) {
            // Один раз на файл: эффект, у которого пропал дочерний файл, не
            // должен заливать журнал строкой в каждом кадре.
            LOG_WARN("Particles") << "дочерний эффект не открылся: " << path;
            EffectMissing.insert(path);
            return nullptr;
        }
        return &EffectCache.emplace(path, std::move(fx)).first->second;
    }

    void ProcessEvents(ParticleSystemInstance& inst) {
        std::vector<ParticleEvent>& events = inst.Sim.Events();
        if (events.empty() || inst.Fx.SubEmitters.empty()) return;
        // Глубже трёх уровней дочерние эффекты не порождаются: эффект, по
        // ошибке указавший сам на себя, иначе размножался бы без конца.
        if (inst.Depth >= 3) return;
        const size_t n = inst.Fx.SubEmitters.size();
        if (inst.Subs.size() != n) {
            inst.Subs.resize(n);
            inst.SubMissing.assign(n, false);
        }
        for (const ParticleEvent& ev : events) {
            for (size_t i = 0; i < n; ++i) {
                const SubEmitter& s = inst.Fx.SubEmitters[i];
                if (s.When != ev.When || s.Effect.empty() || inst.SubMissing[i]) continue;
                if (!inst.Subs[i]) {
                    const ParticleEffect* child = LoadEffect(s.Effect);
                    if (!child) {
                        inst.SubMissing[i] = true;
                        continue;
                    }
                    inst.Subs[i] = Make(*child, inst.Depth + 1);
                    inst.Subs[i]->Sim.Stop();   // рождает только по событиям
                }
                ParticleSystemInstance& sub = *inst.Subs[i];
                const glm::vec4 tint = s.InheritColor ? ev.Color : glm::vec4(1.0f);
                sub.Sim.Emit(sub.Fx, glm::translate(glm::mat4(1.0f), ev.Position), std::max(s.Count, 0), tint,
                             ev.Velocity * s.InheritVelocity);
            }
        }
    }

    void StepTree(ParticleSystemInstance& inst, float dt) {
        inst.Sim.Step(inst.Fx, inst.World, dt, Collision ? &Collision : nullptr);
        ProcessEvents(inst);
        for (auto& s : inst.Subs) {
            if (!s) continue;
            s->Sim.Step(s->Fx, s->World, dt, Collision ? &Collision : nullptr);
            ProcessEvents(*s);
            for (auto& g : s->Subs)
                if (g) StepTree(*g, dt);
        }
    }

    template <typename Fn>
    void ForEachTree(ParticleSystemInstance& inst, Fn&& fn) {
        fn(inst);
        for (auto& s : inst.Subs)
            if (s) ForEachTree(*s, fn);
    }
    template <typename Fn>
    void ForEach(Fn&& fn) {
        for (auto& [k, i] : Entities) ForEachTree(*i, fn);
        for (auto& [k, i] : Streams) ForEachTree(*i, fn);
        for (auto& i : OneShots) ForEachTree(*i, fn);
    }

    void EnsureGpu();
    const TextureEntry* TextureFor(const ParticleEffect& fx);
    void DrawInstance(ParticleSystemInstance& inst, const glm::vec3& camRight, const glm::vec3& camUp,
                      const glm::vec3& camPos, const glm::mat4& view, const glm::mat4& proj);
};

// --- Жизнь -------------------------------------------------------------------------------

ParticleSystem::ParticleSystem() : m(std::make_unique<Impl>()) {
    m->Loader = [](const std::string& path, ParticleEffect& out) {
        const std::string real = sage::AssetDatabase::Instance().LocatePath(path);
        return LoadEffectFile(real, out, nullptr);
    };
}

ParticleSystem::~ParticleSystem() = default;

void ParticleSystem::SyncEmitter(uint64_t key, const ParticleEffect& fx, const glm::mat4& world, bool playing) {
    std::unique_ptr<ParticleSystemInstance>& slot = m->Entities[key];
    if (!slot) slot = m->Make(fx);
    ParticleSystemInstance& inst = *slot;
    inst.Fx = fx;
    inst.World = world;
    inst.Synced = true;
    if (inst.Orphan) {
        inst.Orphan = false;
        inst.Sim.Play();
    }
    if (playing && !inst.Sim.Playing()) inst.Sim.Play();
    if (!playing && inst.Sim.Playing()) inst.Sim.Stop();
}

void ParticleSystem::EmitNow(uint64_t key, int count) { m->PendingEmit[key] += std::max(count, 0); }

void ParticleSystem::Restart(uint64_t key) {
    auto it = m->Entities.find(key);
    if (it != m->Entities.end()) it->second->Sim.Play();
}

void ParticleSystem::ClearEmitter(uint64_t key) {
    auto it = m->Entities.find(key);
    if (it == m->Entities.end()) return;
    const bool playing = it->second->Sim.Playing();
    it->second->Sim.Stop(true);
    it->second->Subs.clear();
    if (playing) it->second->Sim.Play();
}

const ParticleEmitter* ParticleSystem::FindEmitter(uint64_t key) const {
    auto it = m->Entities.find(key);
    return it == m->Entities.end() ? nullptr : &it->second->Sim;
}

void ParticleSystem::Burst(const ParticleEffect& fx, glm::vec3 position, int count) {
    if (AliveCount() >= kMaxParticles) return;
    auto inst = m->Make(fx);
    inst->World = glm::translate(glm::mat4(1.0f), position);
    inst->Sim.Stop();   // сам не рождает — только этот залп
    inst->Sim.Emit(inst->Fx, inst->World, std::min(count, (int)kMaxParticles));
    m->OneShots.push_back(std::move(inst));
}

void ParticleSystem::CreateStream(const std::string& id, const ParticleEffect& fx, glm::vec3 position) {
    std::unique_ptr<ParticleSystemInstance>& slot = m->Streams[id];
    if (!slot) {
        slot = m->Make(fx);
        slot->Sim.Stop();   // струя создаётся выключенной (SetStreamActive)
    }
    slot->Fx = fx;
    slot->World = glm::translate(glm::mat4(1.0f), position);
}

void ParticleSystem::SetStreamActive(const std::string& id, bool active) {
    auto it = m->Streams.find(id);
    if (it == m->Streams.end()) return;
    if (active && !it->second->Sim.Playing()) it->second->Sim.Play();
    if (!active && it->second->Sim.Playing()) it->second->Sim.Stop();
}

void ParticleSystem::SetStreamPosition(const std::string& id, glm::vec3 position) {
    auto it = m->Streams.find(id);
    if (it != m->Streams.end()) it->second->World[3] = glm::vec4(position, 1.0f);
}

void ParticleSystem::RemoveStream(const std::string& id) { m->Streams.erase(id); }

void ParticleSystem::Update(float dt) {
    for (auto it = m->Entities.begin(); it != m->Entities.end();) {
        ParticleSystemInstance& inst = *it->second;
        if (!inst.Synced && !inst.Orphan) {
            // Объект пропал: эмиттер больше не рождает, живые доживают.
            inst.Orphan = true;
            inst.Sim.Stop();
        }
        auto pending = m->PendingEmit.find(it->first);
        if (pending != m->PendingEmit.end()) {
            inst.Sim.Emit(inst.Fx, inst.World, pending->second);
            m->PendingEmit.erase(pending);
        }
        m->StepTree(inst, dt);
        inst.Synced = false;
        if (inst.Orphan && !inst.AliveTree()) it = m->Entities.erase(it);
        else ++it;
    }
    m->PendingEmit.clear();
    for (auto& [id, inst] : m->Streams) m->StepTree(*inst, dt);
    for (auto& inst : m->OneShots) m->StepTree(*inst, dt);
    m->OneShots.erase(std::remove_if(m->OneShots.begin(), m->OneShots.end(),
                                     [](const std::unique_ptr<ParticleSystemInstance>& i) { return !i->AliveTree(); }),
                      m->OneShots.end());
}

void ParticleSystem::Clear() {
    m->Entities.clear();
    m->Streams.clear();
    m->OneShots.clear();
    m->PendingEmit.clear();
    m->EffectCache.clear();
    m->EffectMissing.clear();
}

void ParticleSystem::SetCollisionQuery(CollisionQuery query) { m->Collision = std::move(query); }

void ParticleSystem::SetEffectLoader(std::function<bool(const std::string&, ParticleEffect&)> loader) {
    m->Loader = std::move(loader);
    m->EffectCache.clear();
    m->EffectMissing.clear();
}

size_t ParticleSystem::AliveCount() const {
    size_t n = 0;
    for (const auto& [k, i] : m->Entities) n += i->CountTree();
    for (const auto& [k, i] : m->Streams) n += i->CountTree();
    for (const auto& i : m->OneShots) n += i->CountTree();
    return n;
}

size_t ParticleSystem::StreamCount() const { return m->Streams.size(); }
size_t ParticleSystem::EmitterCount() const { return m->Entities.size(); }

// --- Отрисовка ----------------------------------------------------------------------------

void ParticleSystem::Impl::EnsureGpu() {
    if (Quad) return;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    const float quad[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    sage::rhi::VertexLayout ql;
    ql.Stride = 2 * sizeof(float);
    ql.Attributes = {{0, 2, sage::rhi::AttribType::Float, 0}};
    ql.InstanceStride = sizeof(InstanceData);
    ql.InstanceAttributes = InstanceAttributes(1);
    Quad = device.CreateGeometry(ql);
    Quad->SetVertexData(quad, sizeof(quad), false);

    sage::rhi::VertexLayout tl;
    tl.Stride = sizeof(TrailVertex);
    tl.Attributes = {{0, 3, sage::rhi::AttribType::Float, (int)offsetof(TrailVertex, Position)},
                     {1, 2, sage::rhi::AttribType::Float, (int)offsetof(TrailVertex, UV)},
                     {2, 4, sage::rhi::AttribType::Float, (int)offsetof(TrailVertex, Color)}};
    Trails = device.CreateGeometry(tl);

    const sage::render::MeshData shapes[4] = {sage::render::BuildCube(), sage::render::BuildSphere(8, 12),
                                              BuildTetrahedron(), sage::render::BuildPlane(1)};
    for (int i = 0; i < 4; ++i) {
        std::vector<float> v;
        v.reserve(shapes[i].Vertices.size() * 8);
        for (const Vertex& vx : shapes[i].Vertices) {
            v.insert(v.end(), {vx.Position.x, vx.Position.y, vx.Position.z, vx.Normal.x, vx.Normal.y,
                               vx.Normal.z, vx.TexCoords.x, vx.TexCoords.y});
        }
        sage::rhi::VertexLayout ml;
        ml.Stride = 8 * sizeof(float);
        ml.Attributes = {{0, 3, sage::rhi::AttribType::Float, 0},
                         {1, 3, sage::rhi::AttribType::Float, 3 * (int)sizeof(float)},
                         {2, 2, sage::rhi::AttribType::Float, 6 * (int)sizeof(float)}};
        ml.InstanceStride = sizeof(InstanceData);
        ml.InstanceAttributes = InstanceAttributes(3);
        Meshes[i] = device.CreateGeometry(ml);
        Meshes[i]->SetVertexData(v.data(), v.size() * sizeof(float), false);
        Meshes[i]->SetIndexData(shapes[i].Indices.data(), shapes[i].Indices.size(), false);
        MeshIndexCount[i] = shapes[i].Indices.size();
    }
}

const TextureEntry* ParticleSystem::Impl::TextureFor(const ParticleEffect& fx) {
    if (fx.Texture.empty() && fx.Frames.empty()) return nullptr;
    std::string key = fx.PixelArt ? "px|" : "sm|";
    if (!fx.Frames.empty()) {
        for (const std::string& f : fx.Frames) key += f + "\n";
    } else {
        key += fx.Texture + "|" + std::to_string(fx.TilesX) + "x" + std::to_string(fx.TilesY);
    }
    auto it = Textures.find(key);
    if (it != Textures.end()) return it->second.Tex ? &it->second : nullptr;

    TextureEntry e;
    const TextureFilter filter = fx.PixelArt ? TextureFilter::Nearest : TextureFilter::Bilinear;
    if (fx.Frames.empty()) {
        // Лист кадров без мип-уровней: на мелком уровне соседние кадры
        // смешиваются, и по краю частицы проступает чужой кадр.
        const bool sheet = fx.TilesX > 1 || fx.TilesY > 1;
        e.Tex = ResourceManager::Instance().GetTexture(fx.Texture, filter, !sheet && !fx.PixelArt, false, true);
        e.Tiles = glm::vec2((float)std::max(fx.TilesX, 1), (float)std::max(fx.TilesY, 1));
    } else {
        // КАДРЫ ОТДЕЛЬНЫМИ ФАЙЛАМИ — склеиваем в лист. Наборы часто приходят
        // именно так (smoke_0.png…smoke_11.png), и резать их руками в лист —
        // та работа, которую движок обязан делать сам.
        std::vector<std::vector<unsigned char>> px(fx.Frames.size());
        std::vector<glm::ivec2> size(fx.Frames.size(), glm::ivec2(0));
        int fw = 0, fh = 0;
        for (size_t i = 0; i < fx.Frames.size(); ++i) {
            const std::string real = sage::AssetDatabase::Instance().LocatePath(fx.Frames[i]);
            if (!ResourceManager::DecodeImageFile(real, px[i], size[i].x, size[i].y)) {
                LOG_WARN("Particles") << "кадр не открылся: " << fx.Frames[i];
                continue;
            }
            fw = std::max(fw, size[i].x);
            fh = std::max(fh, size[i].y);
        }
        if (fw > 0 && fh > 0) {
            const int n = (int)fx.Frames.size();
            const int cols = (int)std::ceil(std::sqrt((float)n));
            const int rows = (n + cols - 1) / cols;
            const int W = cols * fw, H = rows * fh;
            std::vector<unsigned char> atlas((size_t)W * H * 4, 0);
            for (int i = 0; i < n; ++i) {
                if (px[i].empty()) continue;
                const int col = i % cols, rowTop = i / cols;
                // Строки картинки уже снизу вверх (DecodeImageFile переворачивает
                // для GL) — кладём кадр в ячейку, считая листы сверху.
                const int ox = col * fw, oy = (rows - 1 - rowTop) * fh;
                for (int y = 0; y < size[i].y; ++y) {
                    const unsigned char* src = px[i].data() + (size_t)y * size[i].x * 4;
                    unsigned char* dst = atlas.data() + ((size_t)(oy + y) * W + ox) * 4;
                    std::copy(src, src + (size_t)size[i].x * 4, dst);
                }
            }
            e.Tex = std::make_shared<Texture>(atlas.data(), W, H, filter, false, true);
            e.Tiles = glm::vec2((float)cols, (float)rows);
        }
    }
    auto& slot = Textures[key];
    slot = e;
    return slot.Tex ? &slot : nullptr;
}

void ParticleSystem::Impl::DrawInstance(ParticleSystemInstance& inst, const glm::vec3& camRight,
                                        const glm::vec3& camUp, const glm::vec3& camPos,
                                        const glm::mat4& view, const glm::mat4& proj) {
    const ParticleEffect& fx = inst.Fx;
    const std::vector<Particle>& ps = inst.Sim.Particles();
    if (ps.empty()) return;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    const bool local = fx.Space == SimulationSpace::Local;
    const glm::mat4& W = inst.Sim.World();
    glm::mat3 rot(W);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(rot[i]);
        if (len > 1e-6f) rot[i] /= len;
    }
    const TextureEntry* tex = TextureFor(fx);
    const int frames = tex ? std::max(1, (int)(tex->Tiles.x * tex->Tiles.y)) : 1;
    const int usedFrames = std::min(ParticleEmitter::FrameCount(fx), frames);

    switch (fx.Blend) {
        case BlendMode::Additive: device.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Additive); break;
        case BlendMode::Premultiplied: device.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Premultiplied); break;
        default: device.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Alpha); break;
    }
    const int premultiply = fx.Blend == BlendMode::Premultiplied ? 1 : 0;
    if (tex) tex->Tex->Bind(0);

    // Экземпляры частиц — в мировых координатах, уже со своим видом на сейчас.
    Scratch.clear();
    Scratch.reserve(ps.size());
    for (const Particle& p : ps) {
        InstanceData d;
        d.Position = local ? glm::vec3(W * glm::vec4(p.Position, 1.0f)) : p.Position;
        d.Velocity = local ? rot * p.Moved : p.Moved;
        d.Size = ParticleEmitter::SizeOf(fx, p);
        d.Color = ParticleEmitter::ColorOf(fx, p);
        d.Rotation = p.Rotation;
        d.Frame = (float)ParticleEmitter::FrameOf(fx, p, usedFrames);
        d.Seed = p.Seed;
        Scratch.push_back(d);
    }
    // Порядок «дальние первыми» — только там, где он виден: при сложении
    // света порядок неважен, и сортировка была бы работой впустую.
    if (fx.SortByDistance && fx.Blend != BlendMode::Additive) {
        std::sort(Scratch.begin(), Scratch.end(), [&](const InstanceData& a, const InstanceData& b) {
            return glm::dot(a.Position - camPos, a.Position - camPos) > glm::dot(b.Position - camPos, b.Position - camPos);
        });
    }

    auto quadUniforms = [&](int mode) {
        Shader& s = QuadShader();
        s.Use();
        s.SetMat4("uView", view);
        s.SetMat4("uProjection", proj);
        s.SetVec3("uCameraRight", camRight);
        s.SetVec3("uCameraUp", camUp);
        s.SetVec3("uCameraPos", camPos);
        s.SetInt("uMode", mode);
        s.SetInt("uAlign", fx.AlignToVelocity ? 1 : 0);
        s.SetFloat("uStretchLength", fx.StretchLength);
        s.SetFloat("uStretchBySpeed", fx.StretchBySpeed);
        s.SetVec2("uTiles", tex ? tex->Tiles : glm::vec2(1.0f));
        s.SetInt("uTex", 0);
        s.SetInt("uHasTex", tex ? 1 : 0);
        s.SetInt("uSprite", (int)fx.Sprite);
        s.SetInt("uPremultiply", premultiply);
        s.SetFloat("uIntensity", std::max(fx.Intensity, 0.0f));
    };

    if (fx.Render == RenderMode::Mesh) {
        const int shape = std::clamp((int)fx.Mesh, 0, 3);
        Shader& s = MeshShader();
        s.Use();
        s.SetMat4("uView", view);
        s.SetMat4("uProjection", proj);
        s.SetVec3("uMeshScale", fx.MeshScale);
        s.SetInt("uAlign", fx.AlignToVelocity ? 1 : 0);
        s.SetVec2("uTiles", tex ? tex->Tiles : glm::vec2(1.0f));
        s.SetInt("uTex", 0);
        s.SetInt("uHasTex", tex ? 1 : 0);
        s.SetInt("uPremultiply", premultiply);
        s.SetFloat("uIntensity", std::max(fx.Intensity, 0.0f));
        Meshes[shape]->SetInstanceData(Scratch.data(), Scratch.size() * sizeof(InstanceData));
        Meshes[shape]->DrawIndexedInstanced(MeshIndexCount[shape], Scratch.size());
        return;
    }

    if (fx.Render == RenderMode::Trail) {
        // Лента: точки следа от хвоста к голове плюс текущее положение.
        TrailScratch.clear();
        for (const Particle& p : ps) {
            std::vector<glm::vec3> pts;
            pts.reserve(p.Trail.size() + 1);
            for (const glm::vec4& t : p.Trail) pts.push_back(glm::vec3(t));
            const glm::vec3 head = local ? glm::vec3(W * glm::vec4(p.Position, 1.0f)) : p.Position;
            if (pts.empty() || glm::length(pts.back() - head) > 1e-5f) pts.push_back(head);
            if (pts.size() < 2) continue;
            const float size = ParticleEmitter::SizeOf(fx, p);
            const glm::vec4 color = ParticleEmitter::ColorOf(fx, p);
            const size_t n = pts.size();
            glm::vec3 prevL(0.0f), prevR(0.0f);
            glm::vec4 prevC(0.0f);
            float prevU = 0.0f;
            for (size_t k = 0; k < n; ++k) {
                const glm::vec3 a = pts[k > 0 ? k - 1 : 0], b = pts[k + 1 < n ? k + 1 : n - 1];
                glm::vec3 tangent = b - a;
                if (glm::length(tangent) < 1e-6f) tangent = glm::vec3(0.0f, 1.0f, 0.0f);
                glm::vec3 side = glm::cross(glm::normalize(tangent), glm::normalize(camPos - pts[k]));
                side = glm::length(side) > 1e-6f ? glm::normalize(side) : camRight;
                // u: 0 — голова, 1 — хвост (так читают кривую ширины и градиент).
                const float u = 1.0f - (float)k / (float)(n - 1);
                const float width = size * (fx.TrailWidth.Keys.empty() ? 1.0f : fx.TrailWidth.Evaluate(u));
                const glm::vec4 c = color * fx.TrailColor.Evaluate(u);
                const glm::vec3 L = pts[k] - side * (0.5f * width), R = pts[k] + side * (0.5f * width);
                if (k > 0) {
                    const TrailVertex quad[6] = {
                        {prevL, {prevU, 0.0f}, prevC}, {prevR, {prevU, 1.0f}, prevC}, {R, {u, 1.0f}, c},
                        {prevL, {prevU, 0.0f}, prevC}, {R, {u, 1.0f}, c},              {L, {u, 0.0f}, c}};
                    TrailScratch.insert(TrailScratch.end(), quad, quad + 6);
                }
                prevL = L;
                prevR = R;
                prevC = c;
                prevU = u;
            }
        }
        if (!TrailScratch.empty()) {
            Shader& s = TrailShader();
            s.Use();
            s.SetMat4("uView", view);
            s.SetMat4("uProjection", proj);
            s.SetInt("uTex", 0);
            s.SetInt("uHasTex", tex ? 1 : 0);
            s.SetInt("uPremultiply", premultiply);
            s.SetFloat("uIntensity", std::max(fx.Intensity, 0.0f));
            Trails->SetVertexData(TrailScratch.data(), TrailScratch.size() * sizeof(TrailVertex), true);
            Trails->DrawArrays(TrailScratch.size());
        }
        if (!fx.TrailHead) return;
        quadUniforms(0);
        Quad->SetInstanceData(Scratch.data(), Scratch.size() * sizeof(InstanceData));
        Quad->DrawInstanced(6, Scratch.size());
        return;
    }

    int mode = 0;
    if (fx.Render == RenderMode::Stretched) mode = 1;
    else if (fx.Render == RenderMode::Horizontal) mode = 2;
    else if (fx.Render == RenderMode::Vertical) mode = 3;
    quadUniforms(mode);
    Quad->SetInstanceData(Scratch.data(), Scratch.size() * sizeof(InstanceData));
    Quad->DrawInstanced(6, Scratch.size());
}

void ParticleSystem::Draw(glm::vec3 camRight, glm::vec3 camUp, const glm::mat4& view, const glm::mat4& proj) {
    std::vector<ParticleSystemInstance*> list;
    m->ForEach([&](ParticleSystemInstance& i) {
        if (!i.Sim.Particles().empty()) list.push_back(&i);
    });
    if (list.empty()) return;
    m->EnsureGpu();

    const glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    // Эмиттеры — от дальнего к ближнему: полупрозрачный дым ближнего костра
    // не должен оказаться ПОД дымом дальнего.
    std::sort(list.begin(), list.end(), [&](ParticleSystemInstance* a, ParticleSystemInstance* b) {
        const glm::vec3 pa = glm::vec3(a->Sim.World()[3]) - camPos, pb = glm::vec3(b->Sim.World()[3]) - camPos;
        return glm::dot(pa, pa) > glm::dot(pb, pb);
    });

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetDepthTest(true);
    device.SetDepthWrite(false);
    device.SetBlend(true);
    device.SetCullMode(sage::rhi::CullMode::Off);   // лист и лента видны с обеих сторон
    for (ParticleSystemInstance* i : list) m->DrawInstance(*i, camRight, camUp, camPos, view, proj);
    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthWrite(true);
    device.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Alpha);
    // И СМЕШИВАНИЕ ТОЖЕ: оставленное включённым, оно доживало до
    // пост-обработки, и её буферы, живущие между кадрами, подмешивали прошлый
    // кадр — картинка темнела с каждым кадром.
    device.SetBlend(false);
}

void ParticleSystem::Draw(const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
    Draw(camera.Right, camera.Up, view, proj);
}

void ParticleSystem::DrawFromView(const glm::mat4& view, const glm::mat4& proj) {
    const glm::vec3 right = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
    const glm::vec3 up = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
    Draw(right, up, view, proj);
}
