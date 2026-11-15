// ===========================================================================
//  ЭЛЕМЕНТЫ УПРАВЛЕНИЯ: выключатель (Toggle: Switch), вертикальный ползунок и
//  раскрытый выпадающий список ПОВЕРХ элементов, нарисованных после него.
// ===========================================================================
#include "Fixture.h"

#include <cstdio>
#include <filesystem>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"

#include "stb_image_write.h"

namespace sage::rendertest {
namespace {

constexpr int kCtlW = 200;
constexpr int kCtlH = 100;

Image RenderControls(UIRenderer& ui, Scene& scene) {
    Framebuffer fbo(kCtlW, kCtlH);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);
    ui.Begin(kCtlW, kCtlH);
    sage::ui::DrawSceneUI(scene, ui, kCtlW, kCtlH);
    ui.End();
    Image img = Capture(kCtlW, kCtlH);
    dev.BindDefaultFramebuffer();
    return img;
}

glm::vec3 Px(const Image& img, int x, int y) {
    const size_t i = ((size_t)y * img.Width + x) * 3;
    return {img.Pixels[i] / 255.0f, img.Pixels[i + 1] / 255.0f, img.Pixels[i + 2] / 255.0f};
}

entt::entity Put(Scene& scene, const char* preset, glm::vec2 pos, glm::vec2 size) {
    GameObject e = scene.CreateEmptyObject(preset);
    Check(sage::ui::ApplyPreset(scene, e.Entity(), preset), "заготовка применилась");
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(e.Entity());
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    for (entt::entity c : scene.ChildrenOf(e.Entity())) scene.RemoveObject(scene.Registry().get<IdComponent>(c).Id);
    return e.Entity();
}

// Progress без подложки (на чёрном видно только заполнение), белым.
sage::ui::Bar& Gauge(Scene& scene, glm::vec2 pos, glm::vec2 size) {
    const entt::entity e = Put(scene, "Progress", pos, size);
    scene.Registry().remove<sage::ui::Fill>(e);
    scene.Registry().get<sage::ui::Element>(e).Skin = sage::ui::Element::SkinMode::Custom;
    sage::ui::Bar& b = scene.Registry().get<sage::ui::Bar>(e);
    b.Padding = glm::vec4(0.0f);
    b.Filled.Color = b.Filled.Gradient = {1, 1, 1, 1};
    b.Filled.Rounding = 0.0f;
    return b;
}

} // namespace

void RunUIProgressChecks() {
    std::printf("[UI progress]\n");
    UIRenderer ui;
    {
        // Radial на четверть, по часовой от верха: закрашена правая верхняя
        // четверть круга, остальные пусты.
        Scene scene("radial");
        sage::ui::Bar& b = Gauge(scene, {50, 0}, {100, 100});
        b.Fill = sage::ui::Bar::Kind::Radial;
        b.Value = 0.25f;
        const Image img = RenderControls(ui, scene);
        const float tr = Px(img, 125, 25).r, tl = Px(img, 75, 25).r, br = Px(img, 125, 75).r;
        std::printf("    круг 25%%: справа сверху %.2f, слева сверху %.2f, справа снизу %.2f\n", tr, tl, br);
        Check(tr > 0.9f, "Radial: первая четверть закрашена");
        Check(tl < 0.05f && br < 0.05f, "Radial: остальные четверти пусты");
        Check(Px(img, 55, 5).r < 0.05f, "Radial: угол квадрата вне круга пуст");
    }
    {
        // Кольцо: середина пуста даже на 100 %.
        Scene scene("ring");
        sage::ui::Bar& b = Gauge(scene, {50, 0}, {100, 100});
        b.Fill = sage::ui::Bar::Kind::Radial;
        b.RingWidth = 15.0f;
        b.Value = 1.0f;
        const Image img = RenderControls(ui, scene);
        Check(Px(img, 100, 7).r > 0.9f, "Radial-кольцо: обод закрашен");
        Check(Px(img, 100, 50).r < 0.05f, "Radial-кольцо: середина пуста");
    }
    {
        // Path по горизонтали на половину: левая половина линии есть, правой нет.
        Scene scene("path");
        sage::ui::Bar& b = Gauge(scene, {0, 0}, {200, 100});
        b.Fill = sage::ui::Bar::Kind::Path;
        b.Path = "M 0 0.5 L 1 0.5";
        b.PathWidth = 20.0f;
        b.Value = 0.5f;
        const Image img = RenderControls(ui, scene);
        Check(Px(img, 50, 50).r > 0.9f, "Path: пройденная часть закрашена");
        Check(Px(img, 150, 50).r < 0.05f, "Path: оставшаяся часть пуста");
        Check(Px(img, 50, 20).r < 0.05f, "Path: вне толщины пути пусто");
    }
    {
        // Custom Mask: картинка порядка — тёмная слева, светлая справа; на
        // 30 % закрашена только левая часть.
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "sage_progress_check";
        fs::create_directories(dir);
        std::vector<unsigned char> px(64 * 8 * 4);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 64; ++x) {
                unsigned char* d = &px[((size_t)y * 64 + x) * 4];
                d[0] = d[1] = d[2] = (unsigned char)(x * 4);
                d[3] = 255;
            }
        const std::string file = (dir / "order.png").string();
        stbi_write_png(file.c_str(), 64, 8, 4, px.data(), 64 * 4);
        Scene scene("custom");
        sage::ui::Bar& b = Gauge(scene, {0, 0}, {200, 100});
        b.Fill = sage::ui::Bar::Kind::CustomMask;
        b.MaskImage = file;
        b.Value = 0.3f;
        const Image img = RenderControls(ui, scene);
        Check(Px(img, 20, 50).r > 0.9f, "Custom Mask: тёмная часть картинки закрашена");
        Check(Px(img, 160, 50).r < 0.05f, "Custom Mask: светлая часть пуста");
    }
    {
        // Диапазон Min..Max: 125 из 0..250 — ровно половина полосы.
        Scene scene("range");
        sage::ui::Bar& b = Gauge(scene, {0, 0}, {200, 100});
        b.Min = 0.0f;
        b.Max = 250.0f;
        b.Value = 125.0f;
        const Image img = RenderControls(ui, scene);
        Check(Px(img, 90, 50).r > 0.9f && Px(img, 110, 50).r < 0.05f, "Linear: Min..Max — половина");
    }
}

void RunUIControlsChecks() {
    std::printf("[UI controls]\n");
    UIRenderer ui;
    {
        // Выключатель 72x40 включён: светлый бегунок справа, слева — дорожка.
        Scene scene("switch");
        const entt::entity t = Put(scene, "Toggle", {10, 10}, {72, 40});
        sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(t);
        r.ToggleVariant = sage::ui::Range::Variant::Switch;
        r.Value = 1.0f;
        const Image img = RenderControls(ui, scene);
        const glm::vec3 knob = Px(img, 62, 30), left = Px(img, 22, 30);
        std::printf("    выключатель: бегунок %.2f, слева %.2f\n", knob.r, left.r);
        Check(knob.r > 0.85f && knob.b > 0.85f, "выключатель: бегунок у правого края");
        Check(left.r < 0.8f, "выключатель: слева дорожка, а не бегунок");
        Check(Px(img, 5, 30).r < 0.05f, "выключатель: левее элемента пусто");
    }
    {
        // Вертикальный ползунок на 3/4: ручка в верхней четверти, а не справа.
        Scene scene("vslider");
        const entt::entity s = Put(scene, "Slider", {90, 0}, {20, 100});
        sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(s);
        r.Vertical = true;
        r.Value = 0.75f;
        const Image img = RenderControls(ui, scene);
        const glm::vec3 lo = Px(img, 100, 90), hi = Px(img, 100, 3);
        const float filledLow = lo.r + lo.g + lo.b, emptyTop = hi.r + hi.g + hi.b;
        std::printf("    вертикальный: заполнено внизу %.2f, вверху %.2f\n", filledLow, emptyTop);
        Check(filledLow > emptyTop + 0.1f, "вертикальный ползунок: заполнение снизу");
    }
    {
        // Раскрытый список — поверх белой формы, нарисованной ПОСЛЕ него.
        Scene scene("dropdown");
        const entt::entity d = Put(scene, "Dropdown", {10, 10}, {120, 20});
        auto& dd = scene.Registry().get<sage::ui::Dropdown>(d);
        dd.Options = "One\nTwo";
        dd.Open = true;
        const entt::entity shape = Put(scene, "Shape", {10, 45}, {120, 30});
        scene.Registry().get<sage::ui::Shape>(shape).Color = {1, 1, 1, 1};
        scene.Registry().get<sage::ui::Element>(shape).Order = 10;
        const Image img = RenderControls(ui, scene);
        const float over = Px(img, 120, 55).r, beside = Px(img, 120, 85).r;
        std::printf("    список: над формой %.2f\n", over);
        Check(over < 0.5f, "раскрытый список рисуется поверх элементов после него");
        Check(beside < 0.05f, "список: две строки, ниже пусто");
    }
}

} // namespace sage::rendertest
