// Твины (sage/anim/Tween.h): кривые, время (задержка, скорость, повтор,
// туда-обратно, реверс), последовательность и параллель, свойства 3D и
// интерфейса через реестр, Lua API и прежние TweenMove — все через один
// проигрыватель сцены.
#include "TestFramework.h"

#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "sage/anim/Tween.h"
#include "sage/audio/AudioComponents.h"
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/Material.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"
#include "sage/ui/Element.h"

using namespace sage::anim;

namespace {

TweenTrack MakeTrack(const char* prop, glm::vec4 to, float dur, Ease e = Ease::Linear(), float start = 0.0f) {
    TweenTrack t;
    t.Property = prop;
    t.To = to;
    t.Duration = dur;
    t.Start = start;
    t.Curve = e;
    return t;
}

glm::vec3& Pos(Scene& s, GameObject o) { return s.Registry().get<Transform>(o.Entity()).Position; }

} // namespace

// --- Кривые ---------------------------------------------------------------------

TEST(Tween_every_ease_starts_at_0_and_ends_at_1) {
    for (int s = 0; s < (int)EaseShape::Curve; ++s) {
        for (int m = 0; m < (int)EaseMode::Count; ++m) {
            const Ease e = Ease::Make((EaseShape)s, (EaseMode)m);
            CHECK_NEAR(Evaluate(e, 0.0f), 0.0f, 1e-4f);
            CHECK_NEAR(Evaluate(e, 1.0f), 1.0f, 1e-4f);
        }
    }
    // Out быстрее в начале, In — медленнее; InOut — ровно середина в середине.
    CHECK_TRUE(Evaluate(Ease::Make(EaseShape::Quad, EaseMode::Out), 0.25f) > 0.25f);
    CHECK_TRUE(Evaluate(Ease::Make(EaseShape::Quad, EaseMode::In), 0.25f) < 0.25f);
    CHECK_NEAR(Evaluate(Ease::Make(EaseShape::Cubic, EaseMode::InOut), 0.5f), 0.5f, 1e-4f);
    // Back перелетает цель — это и есть «пружина».
    float peak = 0.0f;
    for (int i = 0; i <= 100; ++i) peak = std::max(peak, Evaluate(Ease::Make(EaseShape::Back, EaseMode::Out), i / 100.0f));
    CHECK_TRUE(peak > 1.05f);
}

TEST(Tween_bezier_curve_and_names_round_trip) {
    Ease c;
    c.Shape = EaseShape::Curve;
    c.Bezier = {0.0f, 0.0f, 1.0f, 1.0f};              // прямая
    CHECK_NEAR(Evaluate(c, 0.3f), 0.3f, 2e-3f);
    c.Bezier = {0.0f, 0.0f, 0.2f, 1.0f};              // резкий старт: к 20 % времени — половина пути
    CHECK_NEAR(Evaluate(c, 0.2f), 0.5f, 2e-3f);

    for (const char* name : {"linear", "quad-out", "back-inout", "elastic-in", "bounce-out"}) {
        Ease e;
        CHECK_TRUE(Parse(name, e));
        CHECK_EQ(ToString(e), std::string(name));
    }
    Ease e;
    CHECK_TRUE(Parse(ToString(c), e));
    CHECK_TRUE(e == c);
    // Прежние и «человеческие» написания.
    CHECK_TRUE(Parse("BackOut", e) && e == Ease::Make(EaseShape::Back, EaseMode::Out));
    CHECK_TRUE(Parse("EaseOutBack", e) && e == Ease::Make(EaseShape::Back, EaseMode::Out));
    CHECK_TRUE(Parse("out", e) && e == Ease::Make(EaseShape::Quad, EaseMode::Out));
    CHECK_FALSE(Parse("wobble", e));
}

// --- Время ------------------------------------------------------------------------

TEST(Tween_position_reaches_target_and_finishes_with_callback) {
    Scene s("t");
    GameObject o = s.CreateObject("Box");
    TweenClip clip;
    clip.Tracks.push_back(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));
    const TweenHandle h = s.Tweens.Play(s.Registry(), o.Entity(), clip);
    int done = 0;
    s.Tweens.SetOnComplete(h, [&] { ++done; });
    s.Tweens.Update(0.5f);
    CHECK_NEAR(Pos(s, o).x, 5.0f, 1e-4f);
    CHECK_TRUE(s.Tweens.IsPlaying(h));
    s.Tweens.Update(0.6f);
    CHECK_NEAR(Pos(s, o).x, 10.0f, 1e-4f);
    CHECK_FALSE(s.Tweens.IsActive(h));
    CHECK_EQ(done, 1);
}

TEST(Tween_delay_speed_reverse) {
    Scene s("t");
    GameObject o = s.CreateObject("Box");
    TweenClip clip;
    clip.Delay = 0.5f;
    clip.Speed = 2.0f;
    clip.Tracks.push_back(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));
    s.Tweens.Play(s.Registry(), o.Entity(), clip);
    s.Tweens.Update(0.2f);                 // 0.4 с «времени твина» — ещё задержка
    CHECK_NEAR(Pos(s, o).x, 0.0f, 1e-5f);
    s.Tweens.Update(0.25f);                // 0.9 → 0.4 с проигрыша
    CHECK_NEAR(Pos(s, o).x, 4.0f, 1e-3f);

    GameObject r = s.CreateObject("Back");
    TweenClip rev;
    rev.Reverse = true;
    rev.Tracks.push_back(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));
    s.Tweens.Play(s.Registry(), r.Entity(), rev);
    s.Tweens.Update(0.25f);                // задом наперёд: из 10 к 0
    CHECK_NEAR(Pos(s, r).x, 7.5f, 1e-3f);
}

TEST(Tween_loop_and_pingpong) {
    Scene s("t");
    GameObject a = s.CreateObject("Loop");
    GameObject b = s.CreateObject("Ping");
    TweenClip loop;
    loop.Loop = TweenLoop::Loop;
    loop.Tracks.push_back(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));
    TweenClip ping = loop;
    ping.Loop = TweenLoop::PingPong;
    s.Tweens.Play(s.Registry(), a.Entity(), loop);
    s.Tweens.Play(s.Registry(), b.Entity(), ping);
    s.Tweens.Update(0.75f);
    s.Tweens.Update(0.5f);                 // 1.25 с
    CHECK_NEAR(Pos(s, a).x, 2.5f, 1e-3f);  // второй круг, снова от 0
    CHECK_NEAR(Pos(s, b).x, 7.5f, 1e-3f);  // обратный ход, от 10
    CHECK_EQ(s.Tweens.Count(), 2);         // повторы сами не кончаются
}

// Последовательность и параллель — время начала дорожек на одной шкале.
TEST(Tween_sequence_parallel_and_gap) {
    Scene s("t");
    GameObject o = s.CreateObject("Box");
    TweenClip clip;
    clip.AppendAfter(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));      // 0..1
    clip.AppendAfter(MakeTrack("object.scale", {2, 2, 2, 0}, 1.0f), 0.5f);    // 1.5..2.5
    clip.AppendWith(MakeTrack("object.rotation", {0, 90, 0, 0}, 1.0f));       // 1.5..2.5
    CHECK_NEAR(clip.Length(), 2.5f, 1e-5f);
    s.Tweens.Play(s.Registry(), o.Entity(), clip);
    const Transform& t = s.Registry().get<Transform>(o.Entity());
    s.Tweens.Update(1.2f);
    CHECK_NEAR(t.Position.x, 10.0f, 1e-4f);
    CHECK_NEAR(t.Scale.x, 1.0f, 1e-5f);     // «затем» ещё не началось — масштаб не тронут
    s.Tweens.Update(0.8f);                  // 2.0 — середина второго шага
    CHECK_NEAR(t.Scale.x, 1.5f, 1e-3f);
    CHECK_NEAR(t.Rotation.y, 45.0f, 1e-3f); // идёт вместе с ним

    // Растянуть весь твин по времени — дорожки сохраняют пропорции.
    clip.Stretch(5.0f);
    CHECK_NEAR(clip.Tracks[1].Start, 3.0f, 1e-4f);
    CHECK_NEAR(clip.Tracks[1].Duration, 2.0f, 1e-4f);
    clip.ArrangeParallel();
    CHECK_NEAR(clip.Length(), 2.0f, 1e-4f);
    clip.ArrangeSequence();
    CHECK_NEAR(clip.Tracks[2].Start, 4.0f, 1e-4f);
}

TEST(Tween_pause_stop_and_newest_wins) {
    Scene s("t");
    GameObject o = s.CreateObject("Box");
    TweenClip a;
    a.Tracks.push_back(MakeTrack("object.position", {10, 0, 0, 0}, 1.0f));
    const TweenHandle h1 = s.Tweens.Play(s.Registry(), o.Entity(), a);
    s.Tweens.Update(0.5f);
    CHECK_TRUE(s.Tweens.Pause(h1));
    s.Tweens.Update(0.5f);
    CHECK_NEAR(Pos(s, o).x, 5.0f, 1e-4f);
    s.Tweens.Resume(h1);
    // Новый твин того же свойства забирает его: старый больше не пишет.
    TweenClip b;
    b.Tracks.push_back(MakeTrack("object.position", {-10, 0, 0, 0}, 1.0f));
    s.Tweens.Play(s.Registry(), o.Entity(), b);
    s.Tweens.Update(0.5f);
    CHECK_NEAR(Pos(s, o).x, -2.5f, 1e-3f);  // из 5 к -10 наполовину
    // Первый доиграл своё время (он просто больше не пишет позицию) — снят сам.
    CHECK_EQ(s.Tweens.StopFor(o.Entity()), 1);
    s.Tweens.Update(0.1f);
    CHECK_EQ(s.Tweens.Count(), 0);
}

// --- Свойства: 3D, свет, камера, звук, материал, интерфейс ----------------------------

TEST(Tween_works_on_light_camera_audio_material_and_ui) {
    Scene s("t");
    entt::registry& reg = s.Registry();
    GameObject lamp = s.CreateObject("Lamp");
    reg.emplace<LightComponent>(lamp.Entity()).Intensity = 1.0f;
    GameObject cam = s.CreateObject("Cam");
    reg.emplace<CameraComponent>(cam.Entity()).Fov = 60.0f;
    GameObject speaker = s.CreateObject("Speaker");
    reg.emplace<AudioSourceComponent>(speaker.Entity()).Volume = 1.0f;

    // Два объекта с ОДНИМ материалом: твин цвета одного не красит второй.
    auto shared = std::make_shared<Material>();
    GameObject doorA = s.CreateObject("DoorA");
    GameObject doorB = s.CreateObject("DoorB");
    reg.emplace<MeshRendererComponent>(doorA.Entity()).MaterialPtr = shared;
    reg.emplace<MeshRendererComponent>(doorB.Entity()).MaterialPtr = shared;

    GameObject button = s.CreateObject("Button");
    reg.emplace<sage::ui::Element>(button.Entity());

    auto one = [&](GameObject o, const char* prop, glm::vec4 to) {
        TweenClip c;
        c.Tracks.push_back(MakeTrack(prop, to, 1.0f));
        CHECK_TRUE(s.Tweens.Play(reg, o.Entity(), c) != kNoTween);
    };
    one(lamp, "light.intensity", {3, 0, 0, 0});
    one(cam, "camera.fov", {90, 0, 0, 0});
    one(speaker, "audio.volume", {0, 0, 0, 0});
    one(doorA, "material.color", {1, 0, 0, 1});
    one(doorA, "material.opacity", {0, 0, 0, 0});
    one(button, "element.position", {100, 50, 0, 0});
    one(button, "element.opacity", {0, 0, 0, 0});
    one(button, "element.scale", {2, 0, 0, 0});
    s.Tweens.Update(0.5f);

    CHECK_NEAR(reg.get<LightComponent>(lamp.Entity()).Intensity, 2.0f, 1e-4f);
    CHECK_NEAR(reg.get<CameraComponent>(cam.Entity()).Fov, 75.0f, 1e-3f);
    CHECK_NEAR(reg.get<AudioSourceComponent>(speaker.Entity()).Volume, 0.5f, 1e-4f);
    const auto& mrA = reg.get<MeshRendererComponent>(doorA.Entity());
    CHECK_TRUE(mrA.MaterialPtr.get() != shared.get());          // своя копия
    CHECK_NEAR(mrA.MaterialPtr->Albedo.g, 0.5f, 1e-4f);
    CHECK_NEAR(mrA.MaterialPtr->Opacity, 0.5f, 1e-4f);
    CHECK_NEAR(shared->Albedo.g, 1.0f, 1e-6f);                  // общий не тронут
    CHECK_TRUE(reg.get<MeshRendererComponent>(doorB.Entity()).MaterialPtr.get() == shared.get());
    const auto& el = reg.get<sage::ui::Element>(button.Entity());
    CHECK_NEAR(el.Opacity, 0.5f, 1e-4f);
    CHECK_NEAR(el.Scale, 1.5f, 1e-4f);
    CHECK_NEAR(el.Position.y, 16.0f + (50.0f - 16.0f) * 0.5f, 1e-3f);
}

// Свойство, которого у объекта нет, — пропуск, а не падение и не порча чужого.
TEST(Tween_missing_property_is_skipped) {
    Scene s("t");
    GameObject o = s.CreateObject("Plain");
    TweenClip c;
    c.Tracks.push_back(MakeTrack("light.intensity", {3, 0, 0, 0}, 1.0f));
    c.Tracks.push_back(MakeTrack("no.such", {3, 0, 0, 0}, 1.0f));
    c.Tracks.push_back(MakeTrack("object.position", {1, 0, 0, 0}, 1.0f));
    s.Tweens.Play(s.Registry(), o.Entity(), c);
    s.Tweens.Update(1.0f);
    CHECK_NEAR(Pos(s, o).x, 1.0f, 1e-5f);
}

// --- Твины редактора: TweenComponent, PlayOnStart, сохранение ------------------------

TEST(Tween_component_plays_on_start_and_survives_save) {
    Scene s("t");
    GameObject panel = s.CreateObject("Panel");
    GameObject button = s.CreateObject("Button");
    TweenComponent& tc = s.Registry().emplace<TweenComponent>(button.Entity());
    TweenClip open;
    open.Name = "Open";
    open.Target = panel.Id();                       // твин кнопки двигает панель
    open.PlayOnStart = true;
    open.Loop = TweenLoop::PingPong;
    open.Delay = 0.25f;
    open.Speed = 1.5f;
    TweenTrack t = MakeTrack("object.position", {0, 4, 0, 0}, 2.0f, Ease::Make(EaseShape::Back, EaseMode::Out));
    open.Tracks.push_back(t);
    TweenTrack wait;                                // пауза — пустая дорожка
    wait.Start = 2.0f;
    wait.Duration = 0.5f;
    open.Tracks.push_back(wait);
    tc.Tweens.push_back(open);

    const std::string text = SceneSerializer::SaveToString(s);
    std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    GameObject b = back->FindByName("Button");
    const TweenComponent* rc = back->Registry().try_get<TweenComponent>(b.Entity());
    CHECK_TRUE(rc != nullptr && rc->Tweens.size() == 1);
    if (!rc || rc->Tweens.empty()) return;
    const TweenClip& c = rc->Tweens[0];
    CHECK_EQ(c.Name, std::string("Open"));
    CHECK_EQ(c.Target, panel.Id());
    CHECK_TRUE(c.PlayOnStart && c.Loop == TweenLoop::PingPong);
    CHECK_NEAR(c.Delay, 0.25f, 1e-6f);
    CHECK_NEAR(c.Speed, 1.5f, 1e-6f);
    CHECK_EQ((int)c.Tracks.size(), 2);
    CHECK_TRUE(c.Tracks[0].Curve == Ease::Make(EaseShape::Back, EaseMode::Out));
    CHECK_NEAR(c.Tracks[0].To.y, 4.0f, 1e-6f);
    CHECK_TRUE(c.Tracks[1].Property.empty());

    // Запуск: PlayOnStart ставит твин, и он двигает ПАНЕЛЬ.
    UpdateTweens(*back, 0.25f + 2.0f / 1.5f);        // задержка + весь ход
    GameObject p = back->FindByName("Panel");
    CHECK_NEAR(back->Registry().get<Transform>(p.Entity()).Position.y, 4.0f, 1e-3f);
    CHECK_NEAR(back->Registry().get<Transform>(b.Entity()).Position.y, 0.0f, 1e-6f);
}

// --- Lua ----------------------------------------------------------------------------

namespace {
struct LuaWorld {
    Scene scene{"lua"};
    ScriptEngine engine;
    std::unique_ptr<sage::scripting::ScriptingSystem> sys;
    LuaWorld() {
        engine.BindScene(scene);
        sys = std::make_unique<sage::scripting::ScriptingSystem>();
        sys->AddBackend(sage::scripting::MakeLuaBackend({&engine}));
        sage::scripting::ScriptServices services;
        services.ScenePtr = &scene;
        sys->Bind(services);
    }
    ~LuaWorld() { sys->Shutdown(); }
    sol::state& L() { return engine.Lua(); }
    void Step(float dt) {
        sys->Update(dt);
        UpdateTweens(scene, dt);
    }
};
} // namespace

TEST(Tween_lua_to_from_table_and_callbacks) {
    LuaWorld w;
    GameObject box = w.scene.CreateObject("Box");
    GameObject btn = w.scene.CreateObject("Btn");
    w.scene.Registry().emplace<sage::ui::Element>(btn.Entity());
    w.L().script(R"(
        local box = Scene.find("Box")
        local btn = Scene.find("Btn")
        done = false
        Tween.to(box, "position", Vector3.new(5, 2, 0), 1.0, Ease.Out)
            :on_complete(function() done = true end)
        Tween.to(btn, "opacity", 0, 1.0, Ease.Linear)
        Tween.to(box, {scale = 2, rotation = Vector3.new(0, 90, 0)}, 1.0, Ease.Linear)
        pulse = Tween.from(btn, "scale", 0, 1.0, Ease.Linear)
    )");
    w.Step(0.5f);
    const entt::registry& reg = w.scene.Registry();
    const Transform& t = reg.get<Transform>(box.Entity());
    CHECK_TRUE(t.Position.x > 2.5f && t.Position.x < 5.0f);    // Out — больше половины к середине
    CHECK_NEAR(t.Scale.y, 1.5f, 1e-3f);                        // число — на все три оси
    CHECK_NEAR(t.Rotation.y, 45.0f, 1e-3f);
    const auto& el = reg.get<sage::ui::Element>(btn.Entity());
    CHECK_NEAR(el.Opacity, 0.5f, 1e-3f);
    CHECK_NEAR(el.Scale, 0.5f, 1e-3f);                         // from: из 0 к бывшему 1
    CHECK_FALSE(w.L().script("return done").get<bool>());
    w.Step(0.6f);
    CHECK_NEAR(t.Position.x, 5.0f, 1e-4f);
    CHECK_TRUE(w.L().script("return done").get<bool>());       // on_complete позвался
    CHECK_FALSE(w.L().script("return pulse:is_playing()").get<bool>());
}

TEST(Tween_lua_chain_sequence_with_wait_loop_and_control) {
    LuaWorld w;
    GameObject box = w.scene.CreateObject("Box");
    w.L().script(R"(
        local box = Scene.find("Box")
        seq = Tween.to(box, "position", Vector3.new(10, 0, 0), 1.0, Ease.Linear)
            :wait(0.5)
            :then_to("scale", 3, 1.0, Ease.Linear)
            :with("rotation", Vector3.new(0, 180, 0), 1.0, Ease.Linear)
        total = seq:duration()
    )");
    CHECK_NEAR(w.L().script("return total").get<float>(), 2.5f, 1e-5f);
    const Transform& t = w.scene.Registry().get<Transform>(box.Entity());
    w.Step(1.25f);
    CHECK_NEAR(t.Position.x, 10.0f, 1e-4f);
    CHECK_NEAR(t.Scale.x, 1.0f, 1e-5f);                      // пауза: масштаб ещё не пошёл
    w.Step(0.75f);                                           // 2.0 — середина второго шага
    CHECK_NEAR(t.Scale.x, 2.0f, 1e-3f);
    CHECK_NEAR(t.Rotation.y, 90.0f, 1e-3f);

    // Пауза, продолжение, отмена — у твина и у объекта.
    w.L().script(R"(
        local box = Scene.find("Box")
        bob = Tween.to(box, "position", Vector3.new(0, 5, 0), 1.0, Ease.Linear):ping_pong()
        Tween.pause(box)
    )");
    const float y0 = t.Position.y;
    w.Step(0.5f);
    CHECK_NEAR(t.Position.y, y0, 1e-6f);
    CHECK_TRUE(w.L().script("return Tween.is_playing(Scene.find('Box')) == true").get<bool>());   // есть, хоть и на паузе
    w.L().script("Tween.resume(Scene.find('Box'))");
    w.Step(0.5f);
    CHECK_TRUE(t.Position.y > y0);
    w.L().script("Tween.cancel(Scene.find('Box'))");
    w.Step(0.1f);
    CHECK_FALSE(w.L().script("return Tween.is_playing(Scene.find('Box'))").get<bool>());
    CHECK_EQ(w.L().script("return Tween.count()").get<int>(), 0);
}

TEST(Tween_lua_errors_name_the_problem) {
    LuaWorld w;
    w.scene.CreateObject("Box");
    auto err = [&](const char* code) {
        sol::protected_function_result r = w.L().safe_script(code, sol::script_pass_on_error);
        if (r.valid()) return std::string();
        sol::error e = r;
        return std::string(e.what());
    };
    CHECK_TRUE(err("Tween.to(Scene.find('Box'), 'postion', 1, 1)").find("postion") != std::string::npos);
    CHECK_TRUE(err("Tween.to(nil, 'position', 1, 1)").find("объект") != std::string::npos);
}

// Прежние TweenMove/Ease.QuadOut — тем же проигрывателем.
TEST(Tween_legacy_functions_use_the_scene_player) {
    ScriptEngine se;
    Scene scene("T");
    se.BindScene(scene);
    GameObject o = scene.CreateObject("Mover");
    se.Lua()["e"] = o;
    se.Lua().script("TweenScale(e, Vec3.new(2, 2, 2), 1.0, Ease.BackOut)");
    CHECK_EQ(scene.Tweens.Count(), 1);
    scene.Tweens.Update(1.0f);
    CHECK_NEAR(scene.Registry().get<Transform>(o.Entity()).Scale.x, 2.0f, 1e-4f);
}
