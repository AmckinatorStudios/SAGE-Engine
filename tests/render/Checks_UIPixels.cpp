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
#include <algorithm>
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
#include "sage/ui/components/Layout.h"
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

// Картинка RGBA во временную папку (строки сверху вниз, как в файле).
std::string WritePng(const char* name, int w, int h, const std::vector<unsigned char>& px) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_ui_sprite_checks";
    fs::create_directories(dir);
    const std::string file = (dir / name).string();
    stbi_write_png(file.c_str(), w, h, 4, px.data(), w * 4);
    return file;
}

// Длины одноцветных отрезков строки y на [x0, x1): по красному каналу > 128.
std::vector<int> Runs(const Image& img, int y, int x0, int x1) {
    std::vector<int> runs;
    int prev = -1, n = 0;
    for (int x = x0; x < x1; ++x) {
        const int k = Red(img, x, y) > 128 ? 1 : 0;
        if (k == prev) { ++n; continue; }
        if (prev >= 0) runs.push_back(n);
        prev = k;
        n = 1;
    }
    if (prev >= 0) runs.push_back(n);
    return runs;
}

// Корень с холстом «подгонять под разрешение»: масштаб ровно k.
GameObject ScaledRoot(Scene& scene, float k) {
    GameObject root = Element(scene, "Root", {0.0f, 0.0f}, {kW2 / k, kH2 / k});
    auto& canvas = scene.Registry().emplace<sage::ui::Canvas>(root.Entity());
    canvas.Mode = sage::ui::Canvas::Scale::ScaleWithSize;
    canvas.Reference = {kW2 / k, kH2 / k};
    return root;
}

// --- Пиксельная картинка на дробном холсте ------------------------------------
//
// Полосы через пиксель (4×1, белый-чёрный-белый-чёрный) в элементе 16×4 на
// холсте с масштабом 1.37: прежде шаг пикселя был 5.48 экранных, и полосы
// выходили по 5 и 6 пикселей — на каждом разрешении своя «рваность». Теперь
// шаг целый, полосы одинаковые.
void CheckPixelArtOnScaledCanvasKeepsEvenLines(UIRenderer& ui) {
    std::vector<unsigned char> px(4 * 1 * 4, 0);
    for (int x = 0; x < 4; x += 2) px[x * 4] = px[x * 4 + 1] = px[x * 4 + 2] = 255;
    for (int x = 0; x < 4; ++x) px[x * 4 + 3] = 255;
    const std::string file = WritePng("stripes.png", 4, 1, px);
    Scene scene("canvas");
    GameObject root = ScaledRoot(scene, 1.37f);
    GameObject e = Element(scene, "Stripes", {4.0f, 4.0f}, {16.0f, 4.0f});
    scene.SetParent(e.Entity(), root.Entity());
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1, 1, 1, 1};
    fill.Rounding = 0.0f;
    fill.Texture = file;
    fill.Filtering = sage::ui::TextureFiltering::Nearest;
    const Image img = Draw(ui, scene);
    std::vector<int> runs = Runs(img, 8, 0, kW2);
    // Первый и последний отрезок — фон вокруг картинки.
    std::string text;
    for (int r : runs) text += std::to_string(r) + " ";
    std::printf("    полосы на холсте 1.37: отрезки %s\n", text.c_str());
    // фон, белая, чёрная, белая, (чёрная + фон): три внутренних полосы равны.
    const bool even = runs.size() >= 5 && runs[1] == runs[2] && runs[2] == runs[3];
    Check(even, "пиксельная картинка на дробном холсте: полосы одинаковой ширины");
}

// --- Девятина на дробном холсте -----------------------------------------------
//
// Рамка в пиксель картинки на холсте 1.5: левый край выходил в 2 экранных
// пикселя, правый — в 1. Теперь шаг целый — края равной толщины.
void CheckNineSliceBordersEqualOnScaledCanvas(UIRenderer& ui) {
    std::vector<unsigned char> px(6 * 6 * 4, 0);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
            unsigned char* d = &px[((size_t)y * 6 + x) * 4];
            const bool edge = x == 0 || y == 0 || x == 5 || y == 5;
            d[0] = edge ? 255 : 0;
            d[2] = edge ? 0 : 255;
            d[3] = 255;
        }
    const std::string file = WritePng("frame1.png", 6, 6, px);
    Scene scene("nine15");
    GameObject root = ScaledRoot(scene, 1.5f);
    GameObject e = Element(scene, "Frame", {3.0f, 3.0f}, {60.0f, 30.0f});
    scene.SetParent(e.Entity(), root.Entity());
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1, 1, 1, 1};
    fill.Rounding = 0.0f;
    fill.Texture = file;
    fill.Fit = (int)sage::ui::Image::Mode::NineSlice;
    fill.SliceBorder = {1, 1, 1, 1};
    fill.Filtering = sage::ui::TextureFiltering::Nearest;
    const Image img = Draw(ui, scene);
    const std::vector<int> runs = Runs(img, 20, 0, kW2);
    // фон, левый край, середина, правый край, фон
    std::printf("    девятина на холсте 1.5: отрезки строки %d", (int)runs.size());
    for (int r : runs) std::printf(" %d", r);
    std::printf("\n");
    Check(runs.size() >= 4 && runs[1] == runs[runs.size() - 2] && runs[1] == 2,
          "девятина на дробном холсте: левый и правый край по 2 пикселя");
}

// --- Гладкий спрайт из атласа — без каймы соседа --------------------------------
//
// Атлас 8×4: левая половина красная, правая зелёная. Кусок — левая половина,
// гладкая фильтрация, крупно. Выборка на правом краю куска брала зелёный
// пиксель соседа — по краю спрайта шла зелёная кайма.
void CheckSmoothAtlasSpriteHasNoNeighbourFringe(UIRenderer& ui) {
    std::vector<unsigned char> px(8 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
            unsigned char* d = &px[((size_t)y * 8 + x) * 4];
            d[x < 4 ? 0 : 1] = 255;
            d[3] = 255;
        }
    const std::string file = WritePng("atlas.png", 8, 4, px);
    Scene scene("atlas");
    GameObject e = Element(scene, "Sprite", {10.0f, 10.0f}, {40.0f, 40.0f});
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1, 1, 1, 1};
    fill.Rounding = 0.0f;
    fill.Texture = file;
    fill.Sprite = {0, 0, 4, 4};
    fill.Filtering = sage::ui::TextureFiltering::Smooth;
    const Image img = Draw(ui, scene);
    int worstGreen = 0;
    for (int y = 12; y < 48; ++y) {
        const size_t i = ((size_t)y * img.Width + 49) * 3;
        worstGreen = std::max(worstGreen, (int)img.Pixels[i + 1]);
    }
    std::printf("    гладкий спрайт из атласа: зелёного на краю %d\n", worstGreen);
    Check(worstGreen < 16, "гладкий спрайт из атласа: без каймы соседнего куска");
}

// --- Надпись по центру — по центру ---------------------------------------------
//
// Шрифт-картинка: буква «A» — квадрат 7×7 пикселей буквы (заглавная, без
// хвоста), шаг 8. В поле 40×40 по центру по обеим осям поля вокруг буквы
// обязаны быть равными. Прежде ширина считалась с промежутком после буквы
// (буква левее центра на пиксель), а высота — строкой 9 вместо тела 7 (буква
// выше центра на два).
void CheckCentredTextIsCentred(UIRenderer& ui) {
    const int size = 512, cell = 32;
    std::vector<unsigned char> px((size_t)size * size * 4, 0);
    const int cx = ('A' % 16) * cell, cy = ('A' / 16) * cell;
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 28; ++x) {
            unsigned char* p = &px[((size_t)(cy + y) * size + cx + x) * 4];
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    const std::string file = WritePng("centre_font.png", size, size, px);
    Scene scene("centre");
    GameObject e = Element(scene, "Text", {20.0f, 20.0f}, {40.0f, 40.0f});
    auto& label = scene.Registry().emplace<sage::ui::Label>(e.Entity());
    label.Text = "A";
    label.Font = file;
    label.Scale = 2.0f;
    label.FontFiltering = sage::ui::TextureFiltering::Nearest;
    label.PadX = 0.0f;
    const Image img = Draw(ui, scene);
    int x0 = -1, x1 = -1, y0 = -1, y1 = -1;
    for (int y = 0; y < kH2; ++y)
        for (int x = 0; x < kW2; ++x)
            if (Red(img, x, y) > 128) {
                if (x0 < 0 || x < x0) x0 = x;
                x1 = std::max(x1, x);
                if (y0 < 0) y0 = y;
                y1 = y;
            }
    const int left = x0 - 20, right = 59 - x1, top = y0 - 20, bottom = 59 - y1;
    std::printf("    надпись по центру: поля слева %d справа %d, сверху %d снизу %d\n", left, right, top, bottom);
    Check(left == right, "надпись по центру: поля слева и справа равны");
    Check(top == bottom, "надпись по центру: поля сверху и снизу равны");
}

// Контейнер с маской и белым ребёнком во весь контейнер.
GameObject Masked(Scene& scene, glm::vec2 pos, glm::vec2 size, const sage::ui::Mask& m) {
    GameObject box = Element(scene, "Box", pos, size);
    scene.Registry().emplace<sage::ui::Mask>(box.Entity(), m);
    GameObject kid = Element(scene, "Kid", {0.0f, 0.0f}, size);
    scene.SetParent(kid.Entity(), box.Entity());
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(kid.Entity());
    fill.Color = {1, 1, 1, 1};
    fill.Rounding = 0.0f;
    return box;
}

// --- Маски по форме -------------------------------------------------------------
//
// Скруглённая маска была в данных, но резала прямоугольником (её даже прятали
// из инспектора). Теперь: скругление, картинка, инверсия, мягкий край.
void CheckShapeMasks(UIRenderer& ui) {
    {
        Scene scene("rounded");
        sage::ui::Mask m;
        m.Form = sage::ui::Mask::Shape::RoundedRect;
        m.Rounding = 14.0f;
        Masked(scene, {10.0f, 10.0f}, {40.0f, 40.0f}, m);
        const Image img = Draw(ui, scene);
        const int corner = Red(img, 11, 11), middle = Red(img, 30, 30), edge = Red(img, 30, 10);
        std::printf("    маска со скруглением: угол %d, середина %d, край посередине %d\n", corner, middle, edge);
        Check(corner < 20 && middle > 240 && edge > 200, "маска со скруглением срезает углы");
    }
    {
        // Картинка-маска 2×1: левая половина непрозрачна, правая прозрачна.
        std::vector<unsigned char> px = {255, 255, 255, 255, 255, 255, 255, 0};
        const std::string file = WritePng("mask.png", 2, 1, px);
        Scene scene("imagemask");
        sage::ui::Mask m;
        m.Form = sage::ui::Mask::Shape::Image;
        m.Image = file;
        m.Filtering = sage::ui::TextureFiltering::Nearest;
        Masked(scene, {10.0f, 10.0f}, {40.0f, 20.0f}, m);
        const Image img = Draw(ui, scene);
        const int left = Red(img, 15, 20), right = Red(img, 45, 20);
        std::printf("    маска-картинка: слева %d, справа %d\n", left, right);
        Check(left > 240 && right < 20, "маска-картинка: видно только там, где она непрозрачна");
    }
    {
        Scene scene("invert");
        sage::ui::Mask m;
        m.Invert = true;
        m.Padding = {10.0f, 10.0f, 10.0f, 10.0f};
        Masked(scene, {10.0f, 10.0f}, {40.0f, 40.0f}, m);
        const Image img = Draw(ui, scene);
        const int ring = Red(img, 13, 30), hole = Red(img, 30, 30);
        std::printf("    маска наоборот: у края %d, в окне %d\n", ring, hole);
        Check(ring > 240 && hole < 20, "маска наоборот: окно прозрачное, вокруг видно");
    }
    {
        Scene scene("feather");
        sage::ui::Mask m;
        m.Feather = 10.0f;
        Masked(scene, {10.0f, 10.0f}, {60.0f, 40.0f}, m);
        const Image img = Draw(ui, scene);
        const int edge = Red(img, 12, 30), inner = Red(img, 40, 30);
        std::printf("    мягкий край: у края %d, в середине %d\n", edge, inner);
        Check(edge > 20 && edge < 200 && inner > 240, "мягкий край маски: плавное затухание к краю");
    }
}

// --- Слой поверх всего интерфейса -------------------------------------------------
//
// Красный — ребёнок первого окна, синее второе окно лежит поверх. Слой 1 у
// красного поднимает его над синим, хотя по дереву он ниже.
void CheckDrawLayerLiftsAboveTheTree(UIRenderer& ui) {
    Scene scene("layers");
    GameObject win1 = Element(scene, "Window1", {0.0f, 0.0f}, {80.0f, 80.0f});
    GameObject tip = Element(scene, "Tip", {10.0f, 10.0f}, {30.0f, 30.0f});
    scene.SetParent(tip.Entity(), win1.Entity());
    auto& red = scene.Registry().emplace<sage::ui::Fill>(tip.Entity());
    red.Color = {1, 0, 0, 1};
    red.Rounding = 0.0f;
    GameObject win2 = Element(scene, "Window2", {0.0f, 0.0f}, {80.0f, 80.0f});
    auto& blue = scene.Registry().emplace<sage::ui::Fill>(win2.Entity());
    blue.Color = {0, 0, 1, 1};
    blue.Rounding = 0.0f;
    scene.Registry().get<sage::ui::Element>(win2.Entity()).Order = 1;
    const int before = Red(Draw(ui, scene), 20, 20);
    scene.Registry().get<sage::ui::Element>(tip.Entity()).DrawLayer = 1;
    const int after = Red(Draw(ui, scene), 20, 20);
    std::printf("    слой: красный под окном %d, со слоем 1 — %d\n", before, after);
    Check(before < 20 && after > 240, "слой отрисовки поднимает элемент над соседним окном");
}

// --- Альфа в прозрачной цели --------------------------------------------------------
//
// Полупрозрачная панель (альфа 0.5) в цели, очищенной в прозрачный: альфа цели
// обязана стать 0.5. Прежнее смешивание (a, 1 - a) умножало её саму на себя —
// 0.25: интерфейс, снятый в текстуру или слой, выходил вдвое прозрачнее.
void CheckAlphaIntoTransparentTarget(UIRenderer& ui) {
    Scene scene("alpha");
    GameObject e = Element(scene, "Half", {0.0f, 0.0f}, {40.0f, 40.0f});
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1.0f, 0.0f, 0.0f, 0.5f};
    fill.Rounding = 0.0f;
    Framebuffer fbo(kW2, kH2);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    dev.Clear(true, true);
    ui.Begin(kW2, kH2);
    sage::ui::DrawSceneUI(scene, ui, kW2, kH2);
    ui.End();
    std::vector<unsigned char> px((size_t)kW2 * kH2 * 4);
    dev.ReadPixelsRGBA(0, 0, kW2, kH2, px.data());
    dev.BindDefaultFramebuffer();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    const size_t i = ((size_t)(kH2 - 1 - 20) * kW2 + 20) * 4;   // строки снизу вверх
    std::printf("    альфа в прозрачной цели: цвет %d, альфа %d\n", px[i], px[i + 3]);
    Check(std::abs((int)px[i + 3] - 128) <= 3, "полупрозрачная панель оставляет в цели альфу 0.5");
    Check(std::abs((int)px[i] - 128) <= 3, "цвет в цели — с домноженной альфой (0.5 красного)");
}

// --- Гладкая картинка с прозрачностью — без каймы -----------------------------------
//
// Красный квадрат на прозрачном БЕЛОМ поле (так сохраняют многие редакторы).
// Гладкий фильтр на краю подмешивал невидимый белый: вокруг шла светлая кайма.
void CheckSmoothImageHasNoTransparentHalo(UIRenderer& ui) {
    std::vector<unsigned char> px(8 * 8 * 4, 255);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            unsigned char* d = &px[((size_t)y * 8 + x) * 4];
            const bool in = x >= 2 && x < 6 && y >= 2 && y < 6;
            d[1] = d[2] = in ? 0 : 255;
            d[3] = in ? 255 : 0;
        }
    const std::string file = WritePng("halo.png", 8, 8, px);
    Scene scene("halo");
    GameObject e = Element(scene, "Pic", {0.0f, 0.0f}, {80.0f, 80.0f});
    auto& fill = scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    fill.Color = {1, 1, 1, 1};
    fill.Rounding = 0.0f;
    fill.Texture = file;
    fill.Filtering = sage::ui::TextureFiltering::Smooth;
    const Image img = Draw(ui, scene);
    int worst = 0;
    for (int x = 0; x < 80; ++x) worst = std::max(worst, (int)img.Pixels[((size_t)40 * img.Width + x) * 3 + 1]);
    std::printf("    кайма прозрачности: зелёного (белой каймы) не больше %d\n", worst);
    Check(worst < 16, "гладкая картинка с прозрачностью — без светлой каймы");
}

} // namespace

void RunUIPixelChecks() {
    std::printf("\n--- Интерфейс по пикселям ---\n");
    UIRenderer ui;
    CheckSquarePanelEdgesAreSolid(ui);
    CheckGradientFromTransparentDraws(ui);
    CheckImageFontKeepsItsPixelGrid(ui);
    CheckPixelArtOnScaledCanvasKeepsEvenLines(ui);
    CheckNineSliceBordersEqualOnScaledCanvas(ui);
    CheckSmoothAtlasSpriteHasNoNeighbourFringe(ui);
    CheckCentredTextIsCentred(ui);
    CheckShapeMasks(ui);
    CheckDrawLayerLiftsAboveTheTree(ui);
    CheckAlphaIntoTransparentTarget(ui);
    CheckSmoothImageHasNoTransparentHalo(ui);
}

} // namespace sage::rendertest
