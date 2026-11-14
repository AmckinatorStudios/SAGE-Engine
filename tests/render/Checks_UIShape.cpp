// ===========================================================================
//  ФОРМА (sage::ui::Shape): заливка, градиент и обводка по контуру.
//
//  Форма рисуется веером по контуру, а не SDF-прямоугольником, — поэтому
//  проверяется то, чего у прямоугольника не было: круг пуст в углах своего
//  квадрата, эллипс тянется по большей стороне, градиент меняет цвет вдоль
//  оси, обводка «внутри» не вылезает за край.
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
#include "sage/ui/components/Visual.h"

namespace sage::rendertest {
namespace {

constexpr int kShapeW = 200;
constexpr int kShapeH = 100;

Image Render(UIRenderer& ui, Scene& scene) {
    Framebuffer fbo(kShapeW, kShapeH);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);
    ui.Begin(kShapeW, kShapeH);
    sage::ui::DrawSceneUI(scene, ui, kShapeW, kShapeH);
    ui.End();
    Image img = Capture(kShapeW, kShapeH);
    dev.BindDefaultFramebuffer();
    return img;
}

// Цвет пикселя (x, y) от верхнего левого угла кадра.
glm::vec3 At(const Image& img, int x, int y) {
    const size_t i = ((size_t)y * img.Width + x) * 3;   // Capture уже перевернул строки
    return {img.Pixels[i] / 255.0f, img.Pixels[i + 1] / 255.0f, img.Pixels[i + 2] / 255.0f};
}

sage::ui::Shape& Place(Scene& scene, glm::vec2 pos, glm::vec2 size) {
    GameObject e = scene.CreateEmptyObject("Shape");
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Shape"), "заготовка формы применилась");
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(e.Entity());
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    sage::ui::Shape& s = scene.Registry().get<sage::ui::Shape>(e.Entity());
    s.Color = s.Color2 = {1.0f, 1.0f, 1.0f, 1.0f};
    return s;
}

// Есть ли светлые пиксели правее x (в строках y0..y1).
bool LitRightOf(const Image& img, int x, int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int xx = x; xx < img.Width; ++xx)
            if (At(img, xx, y).r > 0.2f) return true;
    return false;
}

// Длинная надпись в узком элементе 60x24 у левого края.
sage::ui::Label& LongText(Scene& scene) {
    GameObject e = scene.CreateEmptyObject("Text");
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Text"), "заготовка текста применилась");
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(e.Entity());
    el.Anchor = UIAnchor::TopLeft;
    el.Position = {10.0f, 10.0f};
    el.Size = {60.0f, 24.0f};
    sage::ui::Label& l = scene.Registry().get<sage::ui::Label>(e.Entity());
    l.Text = "WWWWWWWWWWWWWWWW";
    l.AutoWidth = false;
    l.PadX = 0.0f;
    l.Horizontal = sage::ui::Label::Align::Start;
    l.Color = {1.0f, 1.0f, 1.0f, 1.0f};
    return l;
}

} // namespace

void RunUIShapeChecks() {
    std::printf("[UI shape]\n");
    UIRenderer ui;

    {
        // Круг в квадрате 80x80: центр закрашен, углы квадрата — пусто.
        Scene scene("circle");
        Place(scene, {10, 10}, {80, 80}).Form = sage::ui::Shape::Geometry::Circle;
        const Image img = Render(ui, scene);
        const float centre = At(img, 50, 50).r, corner = At(img, 13, 13).r, edgeMid = At(img, 12, 50).r;
        std::printf("    круг: центр %.2f, угол %.2f, край %.2f\n", centre, corner, edgeMid);
        Check(centre > 0.95f, "круг: центр закрашен");
        Check(corner < 0.05f, "круг: угол квадрата пуст (это не прямоугольник)");
        Check(edgeMid > 0.5f, "круг: середина стороны закрашена");
    }
    {
        // Эллипс 180x60: тянется по ширине, у верхних углов пусто.
        Scene scene("ellipse");
        Place(scene, {10, 20}, {180, 60}).Form = sage::ui::Shape::Geometry::Ellipse;
        const Image img = Render(ui, scene);
        Check(At(img, 15, 50).r > 0.5f, "эллипс: левый край по ширине закрашен");
        Check(At(img, 20, 24).r < 0.05f, "эллипс: верхний левый угол пуст");
    }
    {
        // Линейный градиент слева направо: красный слева, синий справа.
        Scene scene("gradient");
        sage::ui::Shape& s = Place(scene, {0, 0}, {200, 100});
        s.Form = sage::ui::Shape::Geometry::Rectangle;
        s.Fill = sage::ui::Shape::FillKind::LinearGradient;
        s.GradientAngle = 0.0f;
        s.Color = {1.0f, 0.0f, 0.0f, 1.0f};
        s.Color2 = {0.0f, 0.0f, 1.0f, 1.0f};
        const Image img = Render(ui, scene);
        const glm::vec3 l = At(img, 5, 50), r = At(img, 195, 50);
        std::printf("    градиент: слева (%.2f, %.2f), справа (%.2f, %.2f)\n", l.r, l.b, r.r, r.b);
        Check(l.r > 0.85f && l.b < 0.15f, "градиент: слева — начальный цвет");
        Check(r.b > 0.85f && r.r < 0.15f, "градиент: справа — конечный цвет");
    }
    {
        // Обводка 6 внутри, без заливки: кольцо у края, середина пуста, снаружи пусто.
        Scene scene("stroke");
        sage::ui::Shape& s = Place(scene, {20, 20}, {160, 60});
        s.Form = sage::ui::Shape::Geometry::Rectangle;
        s.Fill = sage::ui::Shape::FillKind::None;
        s.StrokeWidth = 6.0f;
        s.StrokeAlignment = sage::ui::Shape::StrokeAlign::Inside;
        s.StrokeColor = {0.0f, 1.0f, 0.0f, 1.0f};
        const Image img = Render(ui, scene);
        Check(At(img, 100, 22).g > 0.9f, "обводка: у края внутри — линия");
        Check(At(img, 100, 50).g < 0.05f, "обводка: середина без заливки пуста");
        Check(At(img, 100, 17).g < 0.05f, "обводка «внутри» не вылезает наружу");
    }

    // --- Текст: что делать с тем, что не помещается -------------------------
    {
        Scene scene("overflow");
        LongText(scene);
        Check(LitRightOf(Render(ui, scene), 74, 10, 34), "текст как есть вылезает за элемент (иначе проверка ниже пуста)");
    }
    {
        Scene scene("autosize");
        LongText(scene).AutoSize = true;
        Check(!LitRightOf(Render(ui, scene), 72, 0, kShapeH), "Auto Size: кегль уменьшился, текст в пределах элемента");
    }
    {
        Scene scene("ellipsis");
        LongText(scene).OverflowMode = sage::ui::Label::Overflow::Ellipsis;
        Check(!LitRightOf(Render(ui, scene), 72, 0, kShapeH), "Ellipsis: текст оборван многоточием в пределах элемента");
    }
    {
        Scene scene("clip");
        LongText(scene).OverflowMode = sage::ui::Label::Overflow::Clip;
        Check(!LitRightOf(Render(ui, scene), 71, 0, kShapeH), "Clip: текст обрезан по элементу");
    }
}

} // namespace sage::rendertest
