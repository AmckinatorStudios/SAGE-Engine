// ===========================================================================
//  ЭЛЕМЕНТЫ УПРАВЛЕНИЯ: выключатель (Toggle: Switch), вертикальный ползунок и
//  раскрытый выпадающий список ПОВЕРХ элементов, нарисованных после него.
// ===========================================================================
#include "Fixture.h"

#include <cstdio>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"

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

} // namespace

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
