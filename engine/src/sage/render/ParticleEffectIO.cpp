#include "sage/render/ParticleEffectIO.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sage/core/Paths.h"

namespace sage::fx {

namespace {

using json = nlohmann::json;

json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json V4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
glm::vec3 V3(const json& j, glm::vec3 d) {
    if (!j.is_array() || j.size() < 3) return d;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
glm::vec4 V4(const json& j, glm::vec4 d) {
    if (!j.is_array() || j.size() < 4) return d;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

json R(const Range& r) { return json::array({r.Min, r.Max}); }
Range R(const json& j, Range d) {
    if (!j.is_array() || j.size() < 2) return d;
    return {j[0].get<float>(), j[1].get<float>()};
}

json C(const Curve& c) {
    json a = json::array();
    for (const glm::vec2& k : c.Keys) a.push_back(json::array({k.x, k.y}));
    return a;
}
Curve C(const json& j, const Curve& d) {
    if (!j.is_array()) return d;
    Curve c;
    for (const json& k : j)
        if (k.is_array() && k.size() >= 2) c.Keys.push_back({k[0].get<float>(), k[1].get<float>()});
    c.Sort();
    return c;
}

json G(const Gradient& g) {
    json out;
    json colors = json::array(), alphas = json::array();
    for (const auto& k : g.Colors) colors.push_back(json::array({k.T, k.Color.r, k.Color.g, k.Color.b}));
    for (const auto& k : g.Alphas) alphas.push_back(json::array({k.T, k.Alpha}));
    out["colors"] = colors;
    out["alphas"] = alphas;
    return out;
}
Gradient G(const json& j, const Gradient& d) {
    if (!j.is_object()) return d;
    Gradient g;
    if (j.contains("colors"))
        for (const json& k : j["colors"])
            if (k.is_array() && k.size() >= 4)
                g.Colors.push_back({k[0].get<float>(), {k[1].get<float>(), k[2].get<float>(), k[3].get<float>()}});
    if (j.contains("alphas"))
        for (const json& k : j["alphas"])
            if (k.is_array() && k.size() >= 2) g.Alphas.push_back({k[0].get<float>(), k[1].get<float>()});
    g.Sort();
    return g;
}

// Перечисления — СЛОВАМИ: номер в файле ломается от любой вставки в середину
// списка, а слово — нет.
template <typename E, size_t N>
const char* Name(E v, const char* const (&names)[N]) {
    const size_t i = (size_t)v;
    return i < N ? names[i] : names[0];
}
template <typename E, size_t N>
E Parse(const json& j, const char* key, E d, const char* const (&names)[N]) {
    if (!j.contains(key) || !j[key].is_string()) return d;
    const std::string s = j[key].get<std::string>();
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return (E)i;
    return d;
}

const char* const kShapes[] = {"point", "sphere", "hemisphere", "cone", "box", "circle", "edge"};
const char* const kSpaces[] = {"world", "local"};
const char* const kRenders[] = {"billboard", "stretched", "horizontal", "vertical", "mesh", "trail"};
const char* const kBlends[] = {"alpha", "additive", "premultiplied"};
const char* const kSprites[] = {"softCircle", "circle", "square"};
const char* const kFlipbooks[] = {"overLifetime", "speed", "random", "fixed"};
const char* const kCollisions[] = {"plane", "world"};
const char* const kMeshes[] = {"cube", "sphere", "tetrahedron", "quad"};
const char* const kTriggers[] = {"birth", "death", "collision"};

template <typename T>
void Get(const json& j, const char* key, T& v) {
    if (j.contains(key) && !j[key].is_null()) {
        try {
            v = j[key].get<T>();
        } catch (const std::exception&) {
            // Поле не того типа — остаётся умолчание (см. «чтение терпимо»).
        }
    }
}

json ToJson(const ParticleEffect& f) {
    json j;
    j["duration"] = f.Duration;
    j["loop"] = f.Loop;
    j["startDelay"] = f.StartDelay;
    j["prewarm"] = f.Prewarm;
    j["maxParticles"] = f.MaxParticles;
    j["space"] = Name(f.Space, kSpaces);
    j["simulationSpeed"] = f.SimulationSpeed;
    j["gravityScale"] = f.GravityScale;
    j["inheritVelocity"] = f.InheritVelocity;
    j["startLifetime"] = R(f.StartLifetime);
    j["startSpeed"] = R(f.StartSpeed);
    j["startSize"] = R(f.StartSize);
    j["startRotation"] = R(f.StartRotation);
    j["startColorA"] = V4(f.StartColorA);
    j["startColorB"] = V4(f.StartColorB);

    j["rateOverTime"] = f.RateOverTime;
    j["rateOverDistance"] = f.RateOverDistance;
    json bursts = json::array();
    for (const Burst& b : f.Bursts)
        bursts.push_back({{"time", b.Time}, {"countMin", b.CountMin}, {"countMax", b.CountMax},
                          {"cycles", b.Cycles}, {"interval", b.Interval}});
    j["bursts"] = bursts;

    j["shape"] = Name(f.Shape, kShapes);
    j["radius"] = f.Radius;
    j["coneAngle"] = f.ConeAngle;
    j["arc"] = f.Arc;
    j["boxSize"] = V3(f.BoxSize);
    j["edgeLength"] = f.EdgeLength;
    j["fromShell"] = f.FromShell;
    j["randomizeDirection"] = f.RandomizeDirection;
    j["shapeOffset"] = V3(f.ShapeOffset);
    j["shapeRotation"] = V3(f.ShapeRotation);

    j["useVelocity"] = f.UseVelocity;
    j["linearVelocity"] = V3(f.LinearVelocity);
    j["orbital"] = f.Orbital;
    j["radial"] = f.Radial;
    j["speedOverLifetime"] = C(f.SpeedOverLifetime);

    j["useForces"] = f.UseForces;
    j["force"] = V3(f.Force);
    j["wind"] = V3(f.Wind);
    j["windInfluence"] = f.WindInfluence;
    j["gustiness"] = f.Gustiness;
    j["drag"] = f.Drag;
    j["maxSpeed"] = f.MaxSpeed;
    j["attraction"] = f.Attraction;

    j["useNoise"] = f.UseNoise;
    j["noiseStrength"] = f.NoiseStrength;
    j["noiseFrequency"] = f.NoiseFrequency;
    j["noiseScroll"] = f.NoiseScroll;
    j["noiseOctaves"] = f.NoiseOctaves;

    j["useSizeOverLifetime"] = f.UseSizeOverLifetime;
    j["sizeOverLifetime"] = C(f.SizeOverLifetime);
    j["useSizeBySpeed"] = f.UseSizeBySpeed;
    j["sizeBySpeedRange"] = R(f.SizeBySpeedRange);
    j["sizeBySpeed"] = C(f.SizeBySpeed);
    j["useColorOverLifetime"] = f.UseColorOverLifetime;
    j["colorOverLifetime"] = G(f.ColorOverLifetime);
    j["useColorBySpeed"] = f.UseColorBySpeed;
    j["colorBySpeedRange"] = R(f.ColorBySpeedRange);
    j["colorBySpeed"] = G(f.ColorBySpeed);
    j["useRotation"] = f.UseRotation;
    j["angularVelocity"] = R(f.AngularVelocity);
    j["rotationOverLifetime"] = C(f.RotationOverLifetime);
    j["alignToVelocity"] = f.AlignToVelocity;

    j["useCollision"] = f.UseCollision;
    j["collision"] = Name(f.Collision, kCollisions);
    j["planeHeight"] = f.PlaneHeight;
    j["bounce"] = f.Bounce;
    j["friction"] = f.Friction;
    j["lifetimeLoss"] = f.LifetimeLoss;
    j["collisionRadius"] = f.CollisionRadius;

    j["texture"] = f.Texture;
    j["frames"] = f.Frames;
    j["tilesX"] = f.TilesX;
    j["tilesY"] = f.TilesY;
    j["flipbook"] = Name(f.Flipbook, kFlipbooks);
    j["frameRate"] = f.FrameRate;
    j["cycles"] = f.Cycles;
    j["startFrame"] = f.StartFrame;
    j["randomStartFrame"] = f.RandomStartFrame;
    j["pixelArt"] = f.PixelArt;

    j["render"] = Name(f.Render, kRenders);
    j["blend"] = Name(f.Blend, kBlends);
    j["sprite"] = Name(f.Sprite, kSprites);
    j["intensity"] = f.Intensity;
    j["sortByDistance"] = f.SortByDistance;
    j["stretchLength"] = f.StretchLength;
    j["stretchBySpeed"] = f.StretchBySpeed;
    j["mesh"] = Name(f.Mesh, kMeshes);
    j["meshScale"] = V3(f.MeshScale);
    j["trailLifetime"] = f.TrailLifetime;
    j["trailMinDistance"] = f.TrailMinDistance;
    j["trailMaxPoints"] = f.TrailMaxPoints;
    j["trailWidth"] = C(f.TrailWidth);
    j["trailColor"] = G(f.TrailColor);
    j["trailHead"] = f.TrailHead;

    json subs = json::array();
    for (const SubEmitter& s : f.SubEmitters)
        subs.push_back({{"when", Name(s.When, kTriggers)}, {"effect", s.Effect}, {"count", s.Count},
                        {"inheritColor", s.InheritColor}, {"inheritVelocity", s.InheritVelocity}});
    j["subEmitters"] = subs;
    return j;
}

ParticleEffect FromJson(const json& j) {
    ParticleEffect f;
    Get(j, "duration", f.Duration);
    Get(j, "loop", f.Loop);
    Get(j, "startDelay", f.StartDelay);
    Get(j, "prewarm", f.Prewarm);
    Get(j, "maxParticles", f.MaxParticles);
    f.Space = Parse(j, "space", f.Space, kSpaces);
    Get(j, "simulationSpeed", f.SimulationSpeed);
    Get(j, "gravityScale", f.GravityScale);
    Get(j, "inheritVelocity", f.InheritVelocity);
    if (j.contains("startLifetime")) f.StartLifetime = R(j["startLifetime"], f.StartLifetime);
    if (j.contains("startSpeed")) f.StartSpeed = R(j["startSpeed"], f.StartSpeed);
    if (j.contains("startSize")) f.StartSize = R(j["startSize"], f.StartSize);
    if (j.contains("startRotation")) f.StartRotation = R(j["startRotation"], f.StartRotation);
    if (j.contains("startColorA")) f.StartColorA = V4(j["startColorA"], f.StartColorA);
    if (j.contains("startColorB")) f.StartColorB = V4(j["startColorB"], f.StartColorB);

    Get(j, "rateOverTime", f.RateOverTime);
    Get(j, "rateOverDistance", f.RateOverDistance);
    if (j.contains("bursts") && j["bursts"].is_array()) {
        for (const json& b : j["bursts"]) {
            Burst x;
            Get(b, "time", x.Time);
            Get(b, "countMin", x.CountMin);
            Get(b, "countMax", x.CountMax);
            Get(b, "cycles", x.Cycles);
            Get(b, "interval", x.Interval);
            f.Bursts.push_back(x);
        }
    }

    f.Shape = Parse(j, "shape", f.Shape, kShapes);
    Get(j, "radius", f.Radius);
    Get(j, "coneAngle", f.ConeAngle);
    Get(j, "arc", f.Arc);
    if (j.contains("boxSize")) f.BoxSize = V3(j["boxSize"], f.BoxSize);
    Get(j, "edgeLength", f.EdgeLength);
    Get(j, "fromShell", f.FromShell);
    Get(j, "randomizeDirection", f.RandomizeDirection);
    if (j.contains("shapeOffset")) f.ShapeOffset = V3(j["shapeOffset"], f.ShapeOffset);
    if (j.contains("shapeRotation")) f.ShapeRotation = V3(j["shapeRotation"], f.ShapeRotation);

    Get(j, "useVelocity", f.UseVelocity);
    if (j.contains("linearVelocity")) f.LinearVelocity = V3(j["linearVelocity"], f.LinearVelocity);
    Get(j, "orbital", f.Orbital);
    Get(j, "radial", f.Radial);
    if (j.contains("speedOverLifetime")) f.SpeedOverLifetime = C(j["speedOverLifetime"], f.SpeedOverLifetime);

    Get(j, "useForces", f.UseForces);
    if (j.contains("force")) f.Force = V3(j["force"], f.Force);
    if (j.contains("wind")) f.Wind = V3(j["wind"], f.Wind);
    Get(j, "windInfluence", f.WindInfluence);
    Get(j, "gustiness", f.Gustiness);
    Get(j, "drag", f.Drag);
    Get(j, "maxSpeed", f.MaxSpeed);
    Get(j, "attraction", f.Attraction);

    Get(j, "useNoise", f.UseNoise);
    Get(j, "noiseStrength", f.NoiseStrength);
    Get(j, "noiseFrequency", f.NoiseFrequency);
    Get(j, "noiseScroll", f.NoiseScroll);
    Get(j, "noiseOctaves", f.NoiseOctaves);

    Get(j, "useSizeOverLifetime", f.UseSizeOverLifetime);
    if (j.contains("sizeOverLifetime")) f.SizeOverLifetime = C(j["sizeOverLifetime"], f.SizeOverLifetime);
    Get(j, "useSizeBySpeed", f.UseSizeBySpeed);
    if (j.contains("sizeBySpeedRange")) f.SizeBySpeedRange = R(j["sizeBySpeedRange"], f.SizeBySpeedRange);
    if (j.contains("sizeBySpeed")) f.SizeBySpeed = C(j["sizeBySpeed"], f.SizeBySpeed);
    Get(j, "useColorOverLifetime", f.UseColorOverLifetime);
    if (j.contains("colorOverLifetime")) f.ColorOverLifetime = G(j["colorOverLifetime"], f.ColorOverLifetime);
    Get(j, "useColorBySpeed", f.UseColorBySpeed);
    if (j.contains("colorBySpeedRange")) f.ColorBySpeedRange = R(j["colorBySpeedRange"], f.ColorBySpeedRange);
    if (j.contains("colorBySpeed")) f.ColorBySpeed = G(j["colorBySpeed"], f.ColorBySpeed);
    Get(j, "useRotation", f.UseRotation);
    if (j.contains("angularVelocity")) f.AngularVelocity = R(j["angularVelocity"], f.AngularVelocity);
    if (j.contains("rotationOverLifetime"))
        f.RotationOverLifetime = C(j["rotationOverLifetime"], f.RotationOverLifetime);
    Get(j, "alignToVelocity", f.AlignToVelocity);

    Get(j, "useCollision", f.UseCollision);
    f.Collision = Parse(j, "collision", f.Collision, kCollisions);
    Get(j, "planeHeight", f.PlaneHeight);
    Get(j, "bounce", f.Bounce);
    Get(j, "friction", f.Friction);
    Get(j, "lifetimeLoss", f.LifetimeLoss);
    Get(j, "collisionRadius", f.CollisionRadius);

    Get(j, "texture", f.Texture);
    Get(j, "frames", f.Frames);
    Get(j, "tilesX", f.TilesX);
    Get(j, "tilesY", f.TilesY);
    f.Flipbook = Parse(j, "flipbook", f.Flipbook, kFlipbooks);
    Get(j, "frameRate", f.FrameRate);
    Get(j, "cycles", f.Cycles);
    Get(j, "startFrame", f.StartFrame);
    Get(j, "randomStartFrame", f.RandomStartFrame);
    Get(j, "pixelArt", f.PixelArt);

    f.Render = Parse(j, "render", f.Render, kRenders);
    f.Blend = Parse(j, "blend", f.Blend, kBlends);
    f.Sprite = Parse(j, "sprite", f.Sprite, kSprites);
    Get(j, "intensity", f.Intensity);
    Get(j, "sortByDistance", f.SortByDistance);
    Get(j, "stretchLength", f.StretchLength);
    Get(j, "stretchBySpeed", f.StretchBySpeed);
    f.Mesh = Parse(j, "mesh", f.Mesh, kMeshes);
    if (j.contains("meshScale")) f.MeshScale = V3(j["meshScale"], f.MeshScale);
    Get(j, "trailLifetime", f.TrailLifetime);
    Get(j, "trailMinDistance", f.TrailMinDistance);
    Get(j, "trailMaxPoints", f.TrailMaxPoints);
    if (j.contains("trailWidth")) f.TrailWidth = C(j["trailWidth"], f.TrailWidth);
    if (j.contains("trailColor")) f.TrailColor = G(j["trailColor"], f.TrailColor);
    Get(j, "trailHead", f.TrailHead);

    if (j.contains("subEmitters") && j["subEmitters"].is_array()) {
        for (const json& s : j["subEmitters"]) {
            SubEmitter x;
            x.When = Parse(s, "when", x.When, kTriggers);
            Get(s, "effect", x.Effect);
            Get(s, "count", x.Count);
            Get(s, "inheritColor", x.InheritColor);
            Get(s, "inheritVelocity", x.InheritVelocity);
            f.SubEmitters.push_back(x);
        }
    }
    return f;
}

} // namespace

std::string EffectToJson(const ParticleEffect& fx) { return ToJson(fx).dump(2); }

bool EffectFromJson(const std::string& text, ParticleEffect& out, std::string* err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (err) *err = "не JSON-объект";
        return false;
    }
    out = FromJson(j);
    return true;
}

bool SaveEffectFile(const std::string& path, const ParticleEffect& fx, std::string* err) {
    std::ofstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (err) *err = "не открылся на запись: " + path;
        return false;
    }
    f << EffectToJson(fx) << "\n";
    return (bool)f;
}

bool LoadEffectFile(const std::string& path, ParticleEffect& out, std::string* err) {
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (err) *err = "не открылся: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return EffectFromJson(ss.str(), out, err);
}

} // namespace sage::fx
