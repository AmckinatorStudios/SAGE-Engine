// ===========================================================================
//  ЭТАП 8 ПРОСТРАНСТВА «ИНТЕРФЕЙС»: Render Texture (режим обновления,
//  пропорции), анимация внутри .sageui, наследование и переопределения
//  стилей, лист спрайтов (pivot, последовательности, атлас).
// ===========================================================================
#include "TestFramework.h"

#include <filesystem>
#include <fstream>

#include "sage/anim/PropertyAnimator.h"
#include "sage/anim/Tween.h"
#include "sage/render/ScenePasses.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/ui/Interface.h"
#include "sage/ui/SpriteSheet.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIStyle.h"

#include <stb_image_write.h>

namespace fs = std::filesystem;
using sage::ui::SpriteSheet;

TEST(SpriteSheet_pivot_and_sequences_survive_the_file) {
    SpriteSheet sheet;
    for (const char* n : {"run_10", "run_2", "run_1", "idle"}) {
        sage::ui::SpriteRegion r;
        r.Name = n;
        r.Rect = {0, 0, 8, 8};
        sheet.Set(r);
    }
    sheet.Find("run_1")->Pivot = {0.25f, 1.0f};
    CHECK_EQ(sheet.SequenceFromPrefix("run", "run_", 10.0f), 3);
    // Порядок — по числу в конце имени, а не по алфавиту (run_10 после run_2).
    const auto* q = sheet.FindSequence("run");
    CHECK_TRUE(q && q->Frames[0] == "run_1" && q->Frames[1] == "run_2" && q->Frames[2] == "run_10");
    SpriteSheet back;
    std::string err;
    CHECK_TRUE(SpriteSheet::FromJsonString(sheet.ToJsonString(), back, err));
    CHECK_NEAR(back.Find("run_1")->Pivot.x, 0.25f, 1e-5f);
    CHECK_NEAR(back.Find("idle")->Pivot.y, 0.5f, 1e-5f);
    const auto* bq = back.FindSequence("run");
    CHECK_TRUE(bq && bq->Frames.size() == 3);
    CHECK_NEAR(bq->Fps, 10.0f, 1e-5f);
    // Кадр по времени: по кругу у петли, на последнем — без неё.
    CHECK_TRUE(bq->FrameAt(0.0f) == "run_1");
    CHECK_TRUE(bq->FrameAt(0.15f) == "run_2");
    CHECK_TRUE(bq->FrameAt(0.35f) == "run_1");
    sage::ui::SpriteSequence once = *bq;
    once.Loop = false;
    CHECK_TRUE(once.FrameAt(5.0f) == "run_10");
}

TEST(SpriteSheet_atlas_packs_without_overlap) {
    std::vector<std::vector<unsigned char>> px;
    std::vector<sage::ui::AtlasInput> in;
    const int sizes[][2] = {{40, 30}, {20, 50}, {64, 10}, {10, 10}};
    px.reserve(4);
    for (int i = 0; i < 4; ++i) {
        px.emplace_back((size_t)sizes[i][0] * sizes[i][1] * 4, (unsigned char)(50 * (i + 1)));
        in.push_back({"img" + std::to_string(i), px.back().data(), sizes[i][0], sizes[i][1]});
    }
    std::vector<unsigned char> atlas;
    int w = 0, h = 0;
    SpriteSheet sheet;
    std::string err;
    CHECK_TRUE(SpriteSheet::PackAtlas(in, 2, 1024, atlas, w, h, sheet, err));
    CHECK_EQ((int)sheet.Regions.size(), 4);
    for (size_t a = 0; a < sheet.Regions.size(); ++a)
        for (size_t b = a + 1; b < sheet.Regions.size(); ++b) {
            const glm::ivec4 ra = sheet.Regions[a].Rect, rb = sheet.Regions[b].Rect;
            const bool apart = ra.x + ra.z <= rb.x || rb.x + rb.z <= ra.x || ra.y + ra.w <= rb.y || rb.y + rb.w <= ra.y;
            CHECK_TRUE(apart);
        }
    // Пиксели картинки лежат в своём куске.
    const glm::ivec4 r = sheet.Find("img2")->Rect;
    CHECK_EQ((int)atlas[((size_t)r.y * w + r.x) * 4], 150);
    // Не помещается — честный отказ, а не обрезанный атлас.
    CHECK_FALSE(SpriteSheet::PackAtlas(in, 2, 32, atlas, w, h, sheet, err));
}

TEST(UIImage_sequence_plays_frames) {
    const fs::path dir = fs::temp_directory_path() / "sage_seq_test";
    fs::create_directories(dir);
    std::vector<unsigned char> px(32 * 8 * 4, 255);
    const std::string png = (dir / "coin.png").string();
    stbi_write_png(png.c_str(), 32, 8, 4, px.data(), 32 * 4);
    SpriteSheet sheet;
    sage::ui::SpriteGrid grid;
    grid.CellW = grid.CellH = 8;
    grid.Prefix = "coin";
    grid.SkipEmpty = false;
    sheet.SliceGrid(32, 8, grid);
    sheet.SequenceFromPrefix("spin", "coin_", 10.0f);
    std::string err;
    CHECK_TRUE(sheet.SaveFile(SpriteSheet::SidecarPath(png), err));

    Scene scene("seq");
    GameObject o = scene.CreateEmptyObject("Coin");
    sage::ui::ApplyPreset(scene, o.Entity(), "Image");
    auto& el = scene.Registry().get<sage::ui::Element>(o.Entity());
    el.Anchor = UIAnchor::TopLeft;
    auto& im = scene.Registry().get<sage::ui::Image>(o.Entity());
    im.Path = png;
    im.Sequence = "spin";
    sage::ui::UIInputState step;
    step.DeltaTime = 0.05f;
    sage::ui::UpdateSceneUI(scene, step, 800, 600);
    CHECK_NEAR(im.Sprite.x, 0.0f, 1e-4f);
    step.DeltaTime = 0.2f;   // ещё два кадра
    sage::ui::UpdateSceneUI(scene, step, 800, 600);
    CHECK_NEAR(im.Sprite.x, 16.0f, 1e-4f);
    CHECK_NEAR(im.Sprite.z, 8.0f, 1e-4f);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(UIStyle_inherits_from_base_and_keeps_overrides) {
    const fs::path dir = fs::temp_directory_path() / "sage_style_base";
    fs::create_directories(dir);
    const std::string base = (dir / "base.sageuistyle").string();
    const std::string child = (dir / "child.sageuistyle").string();
    {
        std::ofstream(base) << R"({"sage_ui_style":1,"parts":{"fill":{"color":{"x":1,"y":0,"z":0,"w":1},"rounding":12}}})";
        std::ofstream(child) << R"({"sage_ui_style":1,"base":")" << base
                             << R"(","parts":{"fill":{"rounding":3}}})";
    }
    Scene scene("style");
    entt::registry& reg = scene.Registry();
    GameObject a = scene.CreateEmptyObject("A");
    GameObject b = scene.CreateEmptyObject("B");
    for (GameObject o : {a, b}) {
        reg.emplace<sage::ui::Element>(o.Entity()).Style = child;
        reg.emplace<sage::ui::Fill>(o.Entity());
    }
    // У B цвет задан сам — стиль его не трогает.
    reg.get<sage::ui::Fill>(b.Entity()).Color = {0, 1, 0, 1};
    reg.get<sage::ui::Element>(b.Entity()).StyleOverrides.push_back("fill.color");
    sage::ui::ClearStyleCache();
    sage::ui::ApplyStyles(scene);
    const auto& fa = reg.get<sage::ui::Fill>(a.Entity());
    CHECK_NEAR(fa.Color.r, 1.0f, 1e-5f);     // от родителя
    CHECK_NEAR(fa.Rounding, 3.0f, 1e-5f);   // своё поле наследника поверх
    const auto& fb = reg.get<sage::ui::Fill>(b.Entity());
    CHECK_NEAR(fb.Color.g, 1.0f, 1e-5f);    // переопределение уцелело
    CHECK_NEAR(fb.Rounding, 3.0f, 1e-5f);   // остальное — из стиля
    std::error_code ec;
    fs::remove_all(dir, ec);
    sage::ui::ClearStyleCache();
}

TEST(Interface_keeps_animation_of_its_elements) {
    Scene scene("anim");
    sage::ui::Interface ui;
    const entt::entity iface = sage::ui::Instantiate(scene, ui);
    const entt::entity root = sage::ui::MainContainer(scene, iface);
    GameObject e = scene.CreateEmptyObject("Pulse");
    scene.SetParent(e.Entity(), root);
    sage::ui::ApplyPreset(scene, e.Entity(), "Button");
    PropertyAnimatorComponent pa;
    pa.ClipPath = "anim/pulse.sageclip";
    pa.Loop = false;
    pa.Speed = 2.0f;
    scene.Registry().emplace<PropertyAnimatorComponent>(e.Entity(), pa);
    sage::anim::TweenComponent tc;
    tc.Tweens.push_back(sage::anim::TweenClip{});
    tc.Tweens.back().Name = "Grow";
    scene.Registry().emplace<sage::anim::TweenComponent>(e.Entity(), tc);

    sage::ui::Interface saved;
    std::string err;
    CHECK_TRUE(sage::ui::Interface::FromJsonString(sage::ui::Capture(scene, iface).ToJsonString(), saved, err));
    Scene other("other");
    const entt::entity copy = sage::ui::Instantiate(other, saved);
    const GameObject pulse = other.FindByName("Pulse");
    CHECK_TRUE(copy != entt::null && pulse.Valid());
    const auto* back = other.Registry().try_get<PropertyAnimatorComponent>(pulse.Entity());
    CHECK_TRUE(back && back->ClipPath == "anim/pulse.sageclip" && !back->Loop);
    CHECK_NEAR(back ? back->Speed : 0.0f, 2.0f, 1e-5f);
    const auto* tw = other.Registry().try_get<sage::anim::TweenComponent>(pulse.Entity());
    CHECK_TRUE(tw && tw->Tweens.size() == 1 && tw->Tweens[0].Name == "Grow");
}

TEST(RenderTexture_update_mode_aspect_and_change_signature) {
    RenderTextureComponent rt;
    rt.Width = 300;
    rt.Aspect = 0.75f;
    CHECK_EQ(rt.TargetHeight(), 400);
    Scene scene("rt");
    GameObject cam = scene.CreateEmptyObject("Cam");
    rt.Target = "portrait";
    rt.Mode = RenderTextureComponent::Update::OnChange;
    scene.Registry().emplace<RenderTextureComponent>(cam.Entity(), rt);
    GameObject box = scene.CreateObject("Box");   // объект с мешем — в кадре
    const auto& c = scene.Registry().get<RenderTextureComponent>(cam.Entity());
    const size_t a = sage::render::ChangeSignature(scene, cam.Entity(), c);
    CHECK_EQ(sage::render::ChangeSignature(scene, cam.Entity(), c), a);   // ничего не сдвинулось
    box.GetTransform().Position.x += 1.0f;
    CHECK_TRUE(sage::render::ChangeSignature(scene, cam.Entity(), c) != a);

    // Режим и пропорции переживают сохранение сцены.
    auto loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;
    const GameObject lc = loaded->FindByName("Cam");
    const auto* lrt = loaded->Registry().try_get<RenderTextureComponent>(lc.Entity());
    CHECK_TRUE(lrt && lrt->Mode == RenderTextureComponent::Update::OnChange);
    CHECK_NEAR(lrt ? lrt->Aspect : 0.0f, 0.75f, 1e-5f);
}
