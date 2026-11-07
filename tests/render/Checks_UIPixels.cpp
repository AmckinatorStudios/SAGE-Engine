// ---------------------------------------------------------------------------
// Эталонные кадры — ИНТЕРФЕЙС ПО ПИКСЕЛЯМ.
//
// Пиксельные игры (наборы текстур 16×/32×, шрифт-картинка) первыми видят то,
// что в гладком интерфейсе незаметно: полупрозрачный край панели, буквы с
// разной толщиной штрихов, градиент, который не рисуется. Каждая проверка
// здесь падала на прежнем коде — см. CHANGELOG, «Интерфейс без артефактов».
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "stb_image_write.h"

#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/components/Visual.h"

namespace sage::rendertest {
namespace {

constexpr int kW2 = 160;
constexpr int kH2 = 90;

Image Draw(UIRenderer& ui, Scene& scene) {
    Framebuffer fbo(kW2, kH2);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);
    ui.Begin(kW2, kH2);
    sage::ui::DrawSceneUI(scene, ui, kW2, kH2);
    ui.End();
    Image img = Capture(kW2, kH2);
    dev.BindDefaultFramebuffer();
    return img;
}

GameObject Element(Scene& scene, const char* name, glm::vec2 pos, glm::vec2 size) {
    GameObject e = scene.CreateObject(name);
    sage::ui::Element el;
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    scene.Registry().emplace<sage::ui::Element>(e.Entity(), el);
    return e;
}

int Red(const Image& img, int x, int y) {
    return img.Pixels[((size_t)y * img.Width + x) * 3];   // Capture уже перевернул строки
}

// --- Край квадратной панели — сплошной ------------------------------------------
//
// Панель без скругления на полупикселе (по центру экрана нечётной ширины так
// стоит каждая вторая) рисовалась с полупрозрачными крайними столбцами, а на
// целом пикселе — с крайним рядом на 7% бледнее: SDF сглаживал прямой край.
// На стыке двух панелей это тёмная полоса, на тёмном фоне — светлая кайма.
void CheckSquarePanelEdgesAreSolid(UIRenderer& ui) {
    Scene scene("edge");
    GameObject e = Element(scene, "Panel", {10.5f, 10.0f}, {20.0f, 10.0f});
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1.0f, 0.0f, 0.0f, 1.0f};
    fill.Rounding = 0.0f;
    const Image img = Draw(ui, scene);
    int partial = 0, full = 0;
    for (int x = 0; x < 40; ++x) {
        const int v = Red(img, x, 15);
        if (v > 250) ++full;
        else if (v > 8) ++partial;
    }
    std::printf("    квадратная панель на полупикселе: сплошных столбцов %d, полупрозрачных %d\n", full, partial);
    Check(partial == 0, "край квадратной панели — без полупрозрачных столбцов");
    Check(full == 20, "ширина панели — ровно 20 пикселей");
}

// --- Градиент из прозрачного ----------------------------------------------------
//
// Тень под шапкой экрана, затемнение к низу — градиент, у которого верхний
// цвет прозрачный. Отрисовка выходила по нулевой альфе ВЕРХНЕГО цвета и не
// рисовала ничего.
void CheckGradientFromTransparentDraws(UIRenderer& ui) {
    Scene scene("gradient");
    GameObject e = Element(scene, "Shade", {0.0f, 0.0f}, {40.0f, 40.0f});
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1.0f, 1.0f, 1.0f, 0.0f};
    fill.Gradient = {1.0f, 1.0f, 1.0f, 1.0f};
    fill.Rounding = 0.0f;
    const Image img = Draw(ui, scene);
    const int top = Red(img, 20, 1), bottom = Red(img, 20, 38);
    std::printf("    градиент из прозрачного: верх %d, низ %d\n", top, bottom);
    Check(bottom > 200, "градиент из прозрачного рисуется (низ светлый)");
    Check(top < 40, "градиент из прозрачного: верх прозрачный");
}

// --- Шрифт-картинка держит свою сетку ---------------------------------------------
//
// Шрифт 32× (ячейка 32, пиксель буквы — 4 пикселя атласа), кегль 2 («строка
// 16»). Прежде масштаб был 16/36 атласа: квадрат буквы шириной 32 пикселя
// атласа выходил шириной 14 экранных, и ближайший сосед выбрасывал из букв
// разные строки. Теперь масштаб кратен пикселю буквы: ровно 2 экранных на
// пиксель, квадрат — 16×16, строка — 18.
void CheckImageFontKeepsItsPixelGrid(UIRenderer& ui) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_image_font_check";
    fs::create_directories(dir);
    const int size = 512, cell = 32;
    std::vector<unsigned char> px((size_t)size * size * 4, 0);
    const int cx = ('A' % 16) * cell, cy = ('A' / 16) * cell;
    for (int y = 0; y < cell; ++y)
        for (int x = 0; x < cell; ++x) {
            unsigned char* p = &px[((size_t)(cy + y) * size + cx + x) * 4];
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    const std::string file = (dir / "font.png").string();
    stbi_write_png(file.c_str(), size, size, 4, px.data(), size * 4);

    Scene scene("imagefont");
    GameObject e = Element(scene, "Text", {10.5f, 10.0f}, {60.0f, 30.0f});
    auto& label = scene.Registry().emplace<sage::ui::Label>(e.Entity());
    label.Text = "A";
    label.Font = file;
    label.Scale = 2.0f;
    label.FontFiltering = sage::ui::TextureFiltering::Nearest;
    label.Horizontal = sage::ui::Label::Align::Start;
    label.Vertical = sage::ui::Label::Align::Start;
    label.PadX = 0.0f;
    label.Color = {1.0f, 1.0f, 1.0f, 1.0f};
    const Image img = Draw(ui, scene);
    int x0 = -1, x1 = -1, y0 = -1, y1 = -1;
    for (int y = 0; y < kH2; ++y)
        for (int x = 0; x < kW2; ++x)
            if (Red(img, x, y) > 128) {
                if (x0 < 0 || x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y0 < 0 || y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    const int w = x0 < 0 ? 0 : x1 - x0 + 1, h = y0 < 0 ? 0 : y1 - y0 + 1;
    UITextStyle style;
    style.UseFont = ui.LoadFont(file, label.FontPixelHeight, true);
    const float line = ui.LineHeight(2.0f, style);
    std::printf("    шрифт-картинка: квадрат буквы %dx%d, строка %.1f\n", w, h, line);
    Check(w == 16 && h == 16, "шрифт-картинка: пиксель буквы — ровно 2 экранных");
    Check(std::abs(line - 18.0f) < 0.01f, "шрифт-картинка: строка 9 пикселей буквы × 2");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

void RunUIPixelChecks() {
    std::printf("\n--- Интерфейс по пикселям ---\n");
    UIRenderer ui;
    CheckSquarePanelEdgesAreSolid(ui);
    CheckGradientFromTransparentDraws(ui);
    CheckImageFontKeepsItsPixelGrid(ui);
}

} // namespace sage::rendertest
