#include "sage/scene/EnvironmentSchema.h"

#include <algorithm>
#include <array>

#include "sage/core/Paths.h"

namespace sage::env {

namespace {

// Короткие конструкторы свойств: схема читается как таблица, а не как стена
// инициализаторов.
#define FIELD(expr) [](Env& e) -> void* { return &(e.expr); }
#define WHEN(cond) [](const Env& e) { return (cond); }

Prop P(PropKind kind, std::string key, std::string label, FieldFn field, std::string hint = {},
       CondFn visible = {}) {
    Prop p;
    p.Kind = kind;
    p.Key = std::move(key);
    p.Label = std::move(label);
    p.Field = std::move(field);
    p.Hint = std::move(hint);
    p.Visible = std::move(visible);
    return p;
}

Prop Num(PropKind kind, std::string key, std::string label, FieldFn field, float min, float max,
         float step, std::string format, std::string hint = {}, CondFn visible = {}) {
    Prop p = P(kind, std::move(key), std::move(label), std::move(field), std::move(hint),
               std::move(visible));
    p.Min = min;
    p.Max = max;
    p.Step = step;
    p.Format = std::move(format);
    return p;
}

Prop Float(std::string key, std::string label, FieldFn field, float min, float max, float step,
           std::string format, std::string hint = {}, CondFn visible = {}) {
    return Num(PropKind::Float, std::move(key), std::move(label), std::move(field), min, max, step,
               std::move(format), std::move(hint), std::move(visible));
}

Prop Slider(std::string key, std::string label, FieldFn field, float min, float max,
            std::string format, std::string hint = {}, CondFn visible = {}) {
    return Num(PropKind::Slider, std::move(key), std::move(label), std::move(field), min, max, 0.0f,
               std::move(format), std::move(hint), std::move(visible));
}

Prop Enum(std::string key, std::string label, FieldFn field, std::vector<std::string> options,
          std::string hint = {}, CondFn visible = {}) {
    Prop p = P(PropKind::Enum, std::move(key), std::move(label), std::move(field), std::move(hint),
               std::move(visible));
    p.Options = std::move(options);
    return p;
}

Prop Custom(std::string key, CondFn visible = {}) {
    Prop p;
    p.Kind = PropKind::Custom;
    p.Key = std::move(key);
    p.Visible = std::move(visible);
    return p;
}

Group G(std::string id, std::string label, std::vector<Prop> props, bool open = true,
        FieldFn toggle = {}, std::string toggleHint = {}, CondFn visible = {}) {
    Group g;
    g.Id = std::move(id);
    g.Label = std::move(label);
    g.Props = std::move(props);
    g.Open = open;
    g.Toggle = std::move(toggle);
    g.ToggleHint = std::move(toggleHint);
    g.Visible = std::move(visible);
    return g;
}

using Src = SkyboxSettings::Source;

// Поворот и яркость — у ВСЕХ текстурных небес одни и те же, и только у них:
// процедурному и одноцветному небу рендер их не применяет (SkyDraw.cpp).
std::vector<Prop> TexturedCommon() {
    return {
        Float("sky.rotation", "Rotation", FIELD(Skybox.RotationDeg), -360.0f, 360.0f, 0.5f, "%.0f°",
              "Turns the sky around the vertical axis"),
        Float("sky.exposure", "Exposure", FIELD(Skybox.Intensity), 0.0f, 4.0f, 0.01f, "%.2f",
              "Brightness of the sky image; ambient light and reflections follow it"),
        Custom("sky.status"),
    };
}

System MakeSky() {
    System s;
    s.Id = "sky";
    s.Label = "Sky";
    s.Icon = "sun";
    s.Hint = "What is drawn behind the scene and lights it from above";
    s.EnabledLabel = "Sky";
    s.EnabledHint = "Sky and background of THIS scene. Game Settings → Sky only switches its display in Game View and in the built game";
    s.Enabled = FIELD(Skybox.Enabled);
    s.GetType = [](const Env& e) { return (int)e.Skybox.Kind; };
    s.SetType = [](Env& e, int v) { e.Skybox.Kind = (Src)v; };

    // --- Процедурное ------------------------------------------------------
    Variant proc;
    proc.Value = (int)Src::Procedural;
    proc.Key = "procedural";
    proc.Label = "Procedural";
    proc.Hint = "A gradient with sun, moon, stars and clouds — no files";
    proc.Groups = {
        G("state", "", {Custom("sky.preset"), Custom("sky.sunlink")}),
        G("colors", "Colors",
          {
              P(PropKind::Color, "sky.zenith", "Zenith", FIELD(Skybox.TopColor)),
              P(PropKind::Color, "sky.horizon", "Horizon", FIELD(Skybox.HorizonColor)),
              P(PropKind::Bool, "sky.dayNight", "Night palette", FIELD(Skybox.DayNight),
                "Sky colours and the scene's light follow the sun's height: below the\n"
                "horizon it gets dark and the moon takes over. This is NOT the time-of-day\n"
                "cycle: the cycle moves the sun, the palette only recolours the sky."),
              P(PropKind::Color, "sky.nightZenith", "Zenith (night)", FIELD(Skybox.NightTopColor), {},
                WHEN(e.Skybox.DayNight)),
              P(PropKind::Color, "sky.nightHorizon", "Horizon (night)",
                FIELD(Skybox.NightHorizonColor), {}, WHEN(e.Skybox.DayNight)),
              P(PropKind::Color, "sky.dusk", "Sunset glow", FIELD(Skybox.DuskColor), {},
                WHEN(e.Skybox.DayNight)),
          }),
        G("sun", "Sun",
          {
              P(PropKind::Color, "sky.sun.color", "Disc colour", FIELD(Skybox.SunColor)),
              Float("sky.sun.size", "Size", FIELD(Skybox.SunSize), 0.005f, 0.6f, 0.002f, "%.3f"),
              Float("sky.sun.brightness", "Brightness", FIELD(Skybox.SunBrightness), 0.0f, 20.0f, 0.05f,
                    "%.2f"),
              Float("sky.sun.glow", "Glow", FIELD(Skybox.SunGlow), 0.0f, 4.0f, 0.02f, "%.2f"),
              P(PropKind::Texture, "sky.sun.texture", "Picture", FIELD(Skybox.SunTexture),
                "Empty — a round disc"),
              Enum("sky.discFiltering", "Picture filtering", FIELD(Skybox.Filtering),
                   {"Smooth", "Nearest pixel"},
                   "How the sun and moon pictures are filtered: nearest pixel keeps them crisp."),
          },
          false, FIELD(Skybox.Celestials), "Discs of the sun and moon and the stars"),
        G("moon", "Moon",
          {
              P(PropKind::Color, "sky.moon.color", "Disc colour", FIELD(Skybox.MoonColor), {},
                WHEN(e.Skybox.Celestials)),
              Float("sky.moon.size", "Size", FIELD(Skybox.MoonSize), 0.005f, 0.6f, 0.002f, "%.3f", {},
                    WHEN(e.Skybox.Celestials)),
              P(PropKind::Bool, "sky.moon.phase", "Phase", FIELD(Skybox.MoonPhase), {},
                WHEN(e.Skybox.Celestials)),
              P(PropKind::Texture, "sky.moon.texture", "Picture", FIELD(Skybox.MoonTexture),
                "Empty — a round disc", WHEN(e.Skybox.Celestials)),
              P(PropKind::Color, "sky.moonlight.color", "Moonlight", FIELD(Skybox.MoonlightColor), {},
                WHEN(e.Skybox.DayNight)),
              Float("sky.moonlight.strength", "Moonlight strength", FIELD(Skybox.MoonlightIntensity),
                    0.0f, 1.0f, 0.005f, "%.3f",
                    "At night the moon becomes the scene's light: it casts the shadows\n"
                    "and sets how dark the night is.",
                    WHEN(e.Skybox.DayNight)),
          },
          false, FIELD(Skybox.Moon), "The moon disc and, with the night palette, moonlight at night",
          WHEN(e.Skybox.Celestials || e.Skybox.DayNight)),
        G("stars", "Stars",
          {
              Float("sky.stars.brightness", "Brightness", FIELD(Skybox.StarIntensity), 0.0f, 3.0f, 0.02f,
                    "%.2f", "0 — no stars"),
              Float("sky.stars.density", "Density", FIELD(Skybox.StarDensity), 0.0f, 20.0f, 0.02f, "%.2f"),
              Float("sky.stars.size", "Size", FIELD(Skybox.StarSize), 0.1f, 10.0f, 0.02f, "%.2f"),
          },
          false, {}, {}, WHEN(e.Skybox.Celestials)),
        G("horizon", "Horizon",
          {
              Float("sky.gradient", "Gradient curve", FIELD(Skybox.GradientExponent), 0.05f, 8.0f, 0.01f,
                    "%.2f",
                    "How fast the zenith colour takes over going up.\n"
                    "0.5 — a narrow bright band at the horizon; 1 — even; above 1 — hazy."),
              Float("sky.horizonSoftness", "Horizon softness", FIELD(Skybox.HorizonSoftness), 0.0f, 1.0f,
                    0.005f, "%.3f"),
              Float("sky.horizonOffset", "Horizon offset", FIELD(Skybox.HorizonOffset), -0.5f, 0.5f,
                    0.002f, "%.3f"),
              P(PropKind::Bool, "sky.ground", "Own colour below the horizon", FIELD(Skybox.Ground),
                "Off — the horizon colour goes down to the nadir."),
              P(PropKind::Color, "sky.ground.color", "Below horizon", FIELD(Skybox.GroundColor), {},
                WHEN(e.Skybox.Ground)),
              P(PropKind::Color, "sky.ground.nightColor", "Below horizon (night)",
                FIELD(Skybox.NightGroundColor), {}, WHEN(e.Skybox.Ground && e.Skybox.DayNight)),
              Float("sky.ground.blend", "Edge width", FIELD(Skybox.GroundBlend), 0.0f, 0.5f, 0.002f, "%.3f",
                    {}, WHEN(e.Skybox.Ground)),
          },
          false),
        G("clouds", "Clouds",
          {
              P(PropKind::Color, "sky.clouds.color", "Colour", FIELD(Skybox.CloudColor)),
              P(PropKind::Color, "sky.clouds.nightColor", "Colour (night)", FIELD(Skybox.NightCloudColor),
                {}, WHEN(e.Skybox.DayNight)),
              Float("sky.clouds.height", "Height", FIELD(Skybox.CloudHeight), -2000.0f, 5000.0f, 1.0f,
                    "%.0f m"),
              Float("sky.clouds.scale", "Size", FIELD(Skybox.CloudScale), 0.1f, 1000.0f, 0.1f, "%.1f m"),
              Slider("sky.clouds.coverage", "Coverage", FIELD(Skybox.CloudCoverage), 0.0f, 1.0f, "%.2f"),
              Slider("sky.clouds.opacity", "Opacity", FIELD(Skybox.CloudOpacity), 0.0f, 1.0f, "%.2f"),
              Float("sky.clouds.wind", "Wind", FIELD(Skybox.CloudWind), -100.0f, 100.0f, 0.05f,
                    "%.2f m/s"),
              Float("sky.clouds.fade", "Cloud fade distance", FIELD(Skybox.CloudFade), 10.0f, 50000.0f, 5.0f,
                    "%.0f m",
                    "Distance at which clouds melt into the horizon. Not related to fog or shadow distance"),
          },
          false, FIELD(Skybox.Clouds), "A cloud layer at a height above the camera"),
    };
    // Ветер — две координаты: тип поля уточняется здесь, чтобы таблица выше
    // читалась одинаково.
    for (Group& g : proc.Groups)
        for (Prop& p : g.Props)
            if (p.Key == "sky.clouds.wind") p.Kind = PropKind::Vec2;

    // --- Один цвет --------------------------------------------------------
    Variant solid;
    solid.Value = (int)Src::Solid;
    solid.Key = "solid";
    solid.Label = "One colour";
    solid.Hint = "Flat fill: no gradient, no sun, no time of day. Ambient light takes this colour";
    solid.Groups = {G("main", "", {P(PropKind::Color, "sky.color", "Color", FIELD(Skybox.TopColor))})};

    // --- Одна картинка ----------------------------------------------------
    Variant image;
    image.Value = (int)Src::Image;
    image.Key = "image";
    image.Label = "One image (cross or panorama)";
    image.Hint = "A cross, strip or 2:1 panorama in one file";
    {
        std::vector<Prop> props = {
            P(PropKind::Texture, "sky.image", "Texture", FIELD(Skybox.ImagePath)),
            Enum("sky.imageLayout", "Layout", FIELD(Skybox.ImageLayout),
                 {"Detect automatically", "Cross 4:3", "Cross 3:4", "Row 6:1", "Column 1:6",
                  "Panorama 2:1"},
                 "Usually the aspect ratio is enough to tell. Set it by hand\n"
                 "if the sky came out scrambled: 4:3 is not always a cross."),
        };
        for (Prop& p : TexturedCommon()) props.push_back(p);
        image.Groups = {G("main", "", props)};
    }

    // --- Папка с кубической картой — прежний тип -------------------------
    // Из выбора убран: он требовал угадать имена px/nx/py/ny/pz/nz, а «шесть
    // файлов» делает то же самое без правил об именах и с подписью у каждой
    // грани. Сцена, где он уже стоит, открывается с тем же небом и кнопкой
    // перевода на шесть файлов (sky.folder.convert).
    Variant cube;
    cube.Value = (int)Src::Cubemap;
    cube.Key = "cubemap";
    cube.Label = "Cubemap folder (old)";
    cube.Hint = "An older sky type: a folder with faces px/nx/py/ny/pz/nz. Convert it to six files";
    cube.Legacy = true;
    {
        std::vector<Prop> props = {P(PropKind::Folder, "sky.folder", "Folder", FIELD(Skybox.CubemapDir)),
                                   Custom("sky.folder.convert")};
        for (Prop& p : TexturedCommon()) props.push_back(p);
        cube.Groups = {G("main", "", props)};
    }

    // --- Шесть файлов -----------------------------------------------------
    Variant faces;
    faces.Value = (int)Src::Faces;
    faces.Key = "faces";
    faces.Label = "Six separate files";
    faces.Hint = "Six pictures picked one by one, any file names";
    faces.Groups = {
        // Развёртка куба сверху (sky.faces.map) показывает, какая картинка
        // где окажется, — подписи «+X» сами этого не объясняют. У каждой
        // грани — своя подсказка: что видно, если смотреть в её сторону.
        G("faces", "Faces",
          {
              Custom("sky.faces.map"),
              P(PropKind::Texture, "sky.face.px", "Right (+X)", FIELD(Skybox.FacePaths[0]),
                "Right side: what is seen looking along +X"),
              P(PropKind::Texture, "sky.face.nx", "Left (-X)", FIELD(Skybox.FacePaths[1]),
                "Left side: what is seen looking along -X"),
              P(PropKind::Texture, "sky.face.py", "Up (+Y)", FIELD(Skybox.FacePaths[2]),
                "Top: the sky straight overhead"),
              P(PropKind::Texture, "sky.face.ny", "Down (-Y)", FIELD(Skybox.FacePaths[3]),
                "Bottom: what is under the feet, below the horizon"),
              P(PropKind::Texture, "sky.face.pz", "Front (+Z)", FIELD(Skybox.FacePaths[4]),
                "Front: what is seen looking along +Z"),
              P(PropKind::Texture, "sky.face.nz", "Back (-Z)", FIELD(Skybox.FacePaths[5]),
                "Back: what is seen looking along -Z, behind the front"),
          }),
        G("main", "", TexturedCommon()),
    };

    // Порядок в списке — по частоте, а не по значению перечисления: значение
    // лежит в файле сцены и переставлять его нельзя, а список — можно.
    s.Variants = {proc, solid, image, cube, faces};
    return s;
}

System MakeCycle() {
    System s;
    s.Id = "cycle";
    s.Label = "Day / Night";
    s.Icon = "clock";
    s.Hint = "Time of day turns the sun object; in the game time runs by itself. Works with any sky.\nThe sky's \"Night palette\" is separate: it only recolours the sky by the sun's height";
    s.Enabled = FIELD(Cycle.Enabled);
    s.EnabledLabel = "Day / Night Cycle";
    s.EnabledHint = "Turns on the time-of-day cycle: Time of Day moves the sun object";
    Variant v;
    v.Key = "cycle";
    v.Groups = {
        G("time", "",
          {
              Slider("cycle.time", "Time of Day", FIELD(Cycle.Time), 0.0f, 24.0f, "%.2f h",
                     "6 — sunrise, 12 — noon, 18 — sunset, 0 — midnight"),
              Float("cycle.speed", "Time Speed", FIELD(Cycle.Speed), -100.0f, 100.0f, 0.05f, "x%.2f",
                    "1 — normal; 0 — time stands still; below 0 — backwards"),
              Float("cycle.dayLength", "Day Length", FIELD(Cycle.DayLengthMinutes), 0.1f, 1440.0f, 0.1f,
                    "%.1f min", "Real minutes in one game day at speed 1"),
              P(PropKind::Bool, "cycle.runInPlay", "Run in Play", FIELD(Cycle.RunInPlay),
                "Off — the sun stays at the set time; scripts can still move it"),
              Custom("cycle.sunlink"),
          }),
        G("path", "Sun Path (advanced)",
          {
              Float("cycle.azimuth", "Sunrise direction", FIELD(Cycle.SunAzimuth), -360.0f, 360.0f, 0.5f,
                    "%.0f°", "Where the sun rises: 0 — +Z, 90 — +X"),
              Float("cycle.noon", "Noon height", FIELD(Cycle.NoonElevation), 1.0f, 90.0f, 0.5f, "%.0f°",
                    "How high the sun stands at noon"),
          },
          false),
    };
    s.Variants = {v};
    return s;
}

System MakeAmbient() {
    System s;
    s.Id = "ambient";
    s.Label = "Ambient Light";
    s.Icon = "light";
    s.Hint = "Soft light from all around: from the sky or your own colours";
    s.TypeLabel = "Source";
    s.GetType = [](const Env& e) { return (int)e.AmbientMode; };
    s.SetType = [](Env& e, int v) { e.AmbientMode = (Env::AmbientSource)v; };
    Variant sky;
    sky.Value = (int)Env::AmbientSource::FromSky;
    sky.Key = "sky";
    sky.Label = "From Sky";
    sky.Hint = "Colours are taken from the sky and darken with it";
    sky.Groups = {G("main", "",
                    {Float("ambient.intensity", "Intensity", FIELD(AmbientStrength), 0.0f, 2.0f, 0.01f, "%.2f"),
                     Custom("ambient.computed")})};
    Variant custom;
    custom.Value = (int)Env::AmbientSource::Custom;
    custom.Key = "custom";
    custom.Label = "Custom";
    custom.Hint = "Exactly the colours below, whatever the sky";
    custom.Groups = {G("main", "",
                       {P(PropKind::Color, "ambient.sky", "Sky Color", FIELD(SkyColor), "Tints upward faces"),
                        P(PropKind::Color, "ambient.ground", "Ground Color", FIELD(GroundColor),
                          "Tints downward faces"),
                        Float("ambient.intensity.custom", "Intensity", FIELD(AmbientStrength), 0.0f, 2.0f,
                              0.01f, "%.2f")})};
    s.Variants = {sky, custom};
    return s;
}

System MakeShadows() {
    System s;
    s.Id = "shadows";
    s.Label = "Scene Shadow Settings";
    s.Icon = "cube";
    s.Hint = "Overrides for THIS scene. Shadow quality, resolution and cascades stay in Game Settings";
    s.Open = false;
    Variant v;
    v.Key = "shadows";
    v.Groups = {G("main", "",
                  {Float("shadows.distance", "Shadow Distance Override", FIELD(Shadows.Distance), 0.0f, 2000.0f,
                         0.5f, "%.0f m",
                         "How far from the camera this scene's shadows reach, in metres.\n"
                         "0 — use the global shadow distance from Game Settings"),
                   Float("shadows.sunAngle", "Sun Angular Size", FIELD(Shadows.SunAngle), 0.0f, 5.0f, 0.05f,
                         "%.2f°",
                         "Angular size of the sun: sharp shadow at the base, softer farther away.\n0 — hard shadows")})};
    s.Variants = {v};
    return s;
}

System MakeFog() {
    System s;
    s.Id = "fog";
    s.Label = "Atmosphere";
    s.Icon = "cone";
    s.Hint = "Fog of the scene: linear distance fog or exponential height fog";
    s.EnabledHint = "Fog of THIS scene. Game Settings → Fog only switches its display in Game View and in the built game";
    s.EnabledLabel = "Fog";
    s.Enabled = FIELD(Fog.Enabled);
    s.GetType = [](const Env& e) { return (int)e.Fog.Kind; };
    s.SetType = [](Env& e, int v) { e.Fog.Kind = (FogSettings::Mode)v; };
    Variant lin;
    lin.Value = (int)FogSettings::Mode::Linear;
    lin.Key = "linear";
    lin.Label = "Linear";
    lin.Hint = "From Start to End objects fade into the fog colour";
    lin.Groups = {G("main", "",
                    {P(PropKind::Color, "fog.color", "Color", FIELD(Fog.Color)),
                     Float("fog.start", "Start Distance", FIELD(Fog.Start), 0.0f, 500.0f, 0.2f, "%.1f m",
                           "Distance from the camera where fog begins, in metres"),
                     Float("fog.end", "End Distance", FIELD(Fog.End), 0.0f, 1000.0f, 0.2f, "%.1f m",
                         "Distance from the camera where objects are fully fogged, in metres")})};
    Variant height;
    height.Value = (int)FogSettings::Mode::ExponentialHeight;
    height.Key = "height";
    height.Label = "Exponential height";
    height.Hint = "Denser near the ground, thinner up high; glows toward the sun";
    height.Groups = {
        G("main", "",
          {P(PropKind::Color, "fog.color.height", "Color", FIELD(Fog.Color)),
           Float("fog.density", "Density", FIELD(Fog.Density), 0.0f, 1.0f, 0.001f, "%.4f"),
           Float("fog.falloff", "Height Falloff", FIELD(Fog.HeightFalloff), 0.0f, 5.0f, 0.005f, "%.3f"),
           Float("fog.baseHeight", "Base Height", FIELD(Fog.BaseHeight), -1000.0f, 1000.0f, 0.1f, "%.1f m",
                 "World height (Y) where the fog is densest, in metres"),
           Float("fog.startDistance", "Start Distance", FIELD(Fog.Start), 0.0f, 1000.0f, 0.2f, "%.1f m",
                 "Distance from the camera where fog begins, in metres"),
           Slider("fog.maxOpacity", "Max Opacity", FIELD(Fog.MaxOpacity), 0.0f, 1.0f, "%.2f")}),
        G("glow", "Sun Glow",
          {Float("fog.sunGlow", "Strength", FIELD(Fog.SunScatter), 0.0f, 4.0f, 0.01f, "%.2f"),
           Float("fog.sunGlowSize", "Tightness", FIELD(Fog.SunExponent), 1.0f, 64.0f, 0.1f, "%.1f",
                 "Larger — a narrower glow around the sun")},
          false),
    };
    s.Variants = {lin, height};
    s.Normalize = [](Env& e) {
        if (e.Fog.Kind == FogSettings::Mode::Linear && e.Fog.End < e.Fog.Start) e.Fog.End = e.Fog.Start;
    };
    return s;
}

#undef FIELD
#undef WHEN

bool GroupActive(const Group& g, const Env& env) {
    if (g.Visible && !g.Visible(env)) return false;
    if (g.Toggle && !*static_cast<bool*>(g.Toggle(const_cast<Env&>(env)))) return false;
    return true;
}

} // namespace

const std::vector<System>& Systems() {
    static const std::vector<System> systems = {MakeSky(), MakeFog(), MakeCycle(), MakeAmbient(), MakeShadows()};
    return systems;
}

const Variant* CurrentVariant(const System& system, const Env& env) {
    if (system.Variants.empty()) return nullptr;
    if (!system.GetType) return &system.Variants.front();
    const int v = system.GetType(env);
    for (const Variant& var : system.Variants)
        if (var.Value == v) return &var;
    return nullptr;
}

const System* FindSystem(const std::string& id) {
    for (const System& s : Systems())
        if (s.Id == id) return &s;
    return nullptr;
}

const Prop* FindProp(const std::string& key) {
    for (const System& s : Systems()) {
        auto scan = [&](const std::vector<Group>& groups) -> const Prop* {
            for (const Group& g : groups)
                for (const Prop& p : g.Props)
                    if (p.Key == key) return &p;
            return nullptr;
        };
        for (const Variant& v : s.Variants)
            if (const Prop* p = scan(v.Groups)) return p;
        if (const Prop* p = scan(s.Common)) return p;
    }
    return nullptr;
}

bool IsPropActive(const std::string& key, const Env& env) {
    for (const System& s : Systems()) {
        if (s.Enabled && !*static_cast<bool*>(s.Enabled(const_cast<Env&>(env)))) continue;
        auto scan = [&](const std::vector<Group>& groups) {
            for (const Group& g : groups) {
                if (!GroupActive(g, env)) continue;
                for (const Prop& p : g.Props)
                    if (p.Key == key && (!p.Visible || p.Visible(env))) return true;
            }
            return false;
        };
        const Variant* v = CurrentVariant(s, env);
        if (v && scan(v->Groups)) return true;
        if (scan(s.Common)) return true;
    }
    return false;
}

bool CubemapFolderToFaces(SkyboxSettings& sky, const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    if (sky.CubemapDir.empty()) return false;
    // Имена и порядок — те же, что читает Skybox::LoadFromDirectory: +X, -X,
    // +Y, -Y, +Z, -Z. Грань найдена не вся — небо не трогаем: половина граней
    // из папки и половина пустых слотов хуже, чем прежнее рабочее небо.
    static const char* kNames[6] = {"px", "nx", "py", "ny", "pz", "nz"};
    static const char* kExt[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    const fs::path dir = sage::PathFromUtf8(sky.CubemapDir);
    const fs::path full = dir.is_absolute() ? dir : base / dir;
    std::array<std::string, 6> found;
    for (int i = 0; i < 6; ++i) {
        for (const char* ext : kExt) {
            std::error_code ec;
            const std::string file = std::string(kNames[i]) + ext;
            if (fs::is_regular_file(full / sage::PathFromUtf8(file), ec)) {
                found[i] = sage::PathToUtf8(dir / sage::PathFromUtf8(file));
                std::replace(found[i].begin(), found[i].end(), '\\', '/');
                break;
            }
        }
        if (found[i].empty()) return false;
    }
    for (int i = 0; i < 6; ++i) sky.FacePaths[i] = found[i];
    sky.Kind = Src::Faces;
    return true;
}

const char* SkySourceKey(SkyboxSettings::Source source) {
    switch (source) {
        case Src::Procedural: return "procedural";
        case Src::Cubemap: return "cubemap";
        case Src::Faces: return "faces";
        case Src::Image: return "image";
        case Src::Solid: return "solid";
    }
    return "procedural";
}

bool SkySourceFromKey(const std::string& key, SkyboxSettings::Source& out) {
    for (Src s : {Src::Procedural, Src::Cubemap, Src::Faces, Src::Image, Src::Solid}) {
        if (key == SkySourceKey(s)) {
            out = s;
            return true;
        }
    }
    return false;
}

} // namespace sage::env
