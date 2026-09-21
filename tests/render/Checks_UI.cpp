// ---------------------------------------------------------------------------
// Эталонные кадры — ИНТЕРФЕЙС ИГРЫ.
//
// ЗАЧЕМ ИМЕННО КАРТИНКА. Правило новой системы частей — «часть рисует себя в
// прямоугольнике СВОЕГО элемента и не двигает соседей» — проверяемо данными
// только наполовину: тесты видят, что подпись переехала в ребёнка, а вот
// закрасила ли подложка галку целиком, наехал ли значок на текст и осталась ли
// дорожка ползунка тонкой полосой — видно только на пикселях. Ровно эти три
// поломки и были возможны при переходе, и ровно они не роняют ни один
// модульный тест.
//
// Меряются не «похожие картинки», а ЧИСЛА С СМЫСЛОМ: доля закрашенного,
// яркость по колонкам, разница между двумя кадрами. Эталон здесь был бы хуже:
// шрифт и сглаживание у разных драйверов свои, а «галка не закрасила ряд» —
// утверждение, которое от драйвера не зависит.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

#include <filesystem>
#include <utility>
#include <system_error>

namespace sage::rendertest {
namespace {

// Ставит элементу прямоугольник в левом верхнем углу кадра. Вызывается и ПОСЛЕ
// заготовки: заготовка приносит свой прямоугольник (кнопка 200x52 по центру), и
// мерить доли в нём значило бы гадать, где он оказался.
void PlaceTopLeft(Scene& scene, GameObject e, glm::vec2 size);

constexpr int kUiW = 480;
constexpr int kUiH = 270;

// Кадр с интерфейсом сцены на чёрном фоне: фон намеренно чёрный, чтобы
// «закрашено» и «не закрашено» отличались без порогов и подбора.
Image RenderUI(UIRenderer& ui, Scene& scene, int w = kUiW, int h = kUiH) {
    Framebuffer fbo(w, h);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);
    ui.Begin(w, h);
    sage::ui::DrawSceneUI(scene, ui, w, h);
    ui.End();
    Image img = Capture(w, h);
    dev.BindDefaultFramebuffer();
    return img;
}

// Средняя яркость прямоугольника кадра, 0..255.
double Luma(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            sum += 0.299 * img.Pixels[i] + 0.587 * img.Pixels[i + 1] + 0.114 * img.Pixels[i + 2];
        }
    }
    return sum / ((x1 - x0) * (y1 - y0));
}

// Доля заметно закрашенных пикселей прямоугольника.
double Covered(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    int lit = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            if (img.Pixels[i] > 12 || img.Pixels[i + 1] > 12 || img.Pixels[i + 2] > 12) ++lit;
        }
    }
    return (double)lit / ((x1 - x0) * (y1 - y0));
}

// Элемент в левом верхнем углу кадра, чтобы прямоугольник кадра и
// прямоугольник элемента совпадали: так измеренные доли читаются прямо, без
// пересчёта координат.
GameObject Screen(Scene& scene, const char* name, glm::vec2 size) {
    GameObject e = scene.CreateObject(name);
    scene.Registry().emplace<sage::ui::Element>(e.Entity(), sage::ui::Element{});
    PlaceTopLeft(scene, e, size);
    return e;
}

void PlaceTopLeft(Scene& scene, GameObject e, glm::vec2 size) {
    sage::ui::Element& t = scene.Registry().get<sage::ui::Element>(e.Entity());
    t.Anchor = UIAnchor::TopLeft;
    t.Mode = sage::ui::Element::Stretch::None;
    t.Position = {0.0f, 0.0f};
    t.Size = size;
}

// --- Галка не закрашивает свой ряд -----------------------------------------
//
// Старая отрисовка при виде «галка» подменяла заливку элемента квадратиком, и
// перенос этого правила в независимые части грозил ровно обратным: подложка
// красит элемент целиком, а квадратик ложится сверху. Ряд 200x36 превратился
// бы в сплошную плашку, на которой подпись не видна.
void CheckToggleDoesNotPaintTheRow(UIRenderer& ui) {
    Scene scene("toggle");
    GameObject e = Screen(scene, "Check", {(float)kUiW, 36.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Checkbox"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 36.0f});
    sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(e.Entity());
    r.Value = 1.0f;   // включена: галочка внутри квадратика
    const Image img = RenderUI(ui, scene);

    // Квадратик — слева, стороной в высоту элемента; подпись-ребёнок стоит за
    // ним (заготовка сдвигает её на 44), остальной ряд обязан быть пуст.
    const double box = Covered(img, 2, 2, 34, 34);
    const double caption = Covered(img, 40, 2, 200, 34);
    const double rest = Covered(img, 220, 2, kUiW - 4, 34);
    std::printf("    галка: квадратик %.2f, подпись %.3f, хвост ряда %.3f\n", box, caption, rest);
    Check(box > 0.5, "галка: квадратик закрашен");
    // Подпись — тонкие штрихи букв, никак не плашка во всю высоту.
    Check(caption > 0.01 && caption < 0.35, "галка: подпись справа от квадратика видна");
    // А дальше подписи — чистый фон: подложка ряд не закрашивает.
    Check(rest < 0.005, "галка: ряд НЕ закрашен подложкой");
}

// --- Значок не наезжает на текст -------------------------------------------
//
// Значок рисуется по центру СВОЕГО элемента. Если бы он остался «квадратиком у
// левого края родителя», подпись-ребёнок, у которой своё поле слева, легла бы
// прямо на него.
void CheckIconAndTextDoNotOverlap(UIRenderer& ui) {
    Scene scene("icon");
    GameObject row = Screen(scene, "Row", {(float)kUiW, 40.0f});

    GameObject iconObj = scene.CreateObject("Icon");
    sage::ui::Element it;
    it.Anchor = UIAnchor::CenterLeft;
    it.Position = {4.0f, 0.0f};
    it.Size = {32.0f, 32.0f};
    scene.Registry().emplace<sage::ui::Element>(iconObj.Entity(), it);
    sage::ui::Icon icon;
    icon.Name = "heart";
    icon.Color = {1.0f, 0.3f, 0.3f, 1.0f};
    scene.Registry().emplace<sage::ui::Icon>(iconObj.Entity(), icon);
    scene.SetParent(iconObj.Entity(), row.Entity());

    GameObject textObj = scene.CreateObject("Text");
    sage::ui::Element tt;
    tt.Anchor = UIAnchor::TopLeft;
    tt.Mode = sage::ui::Element::Stretch::Both;
    tt.Margin = {44.0f, 0.0f, 0.0f, 0.0f};
    scene.Registry().emplace<sage::ui::Element>(textObj.Entity(), tt);
    sage::ui::Label label;
    label.Text = "100 / 100";
    label.Scale = 1.6f;
    label.Horizontal = sage::ui::Label::Align::Start;
    scene.Registry().emplace<sage::ui::Label>(textObj.Entity(), label);
    scene.SetParent(textObj.Entity(), row.Entity());

    const Image img = RenderUI(ui, scene);
    // Значок красный, текст белый: канал зелёного отделяет одно от другого без
    // догадок о форме букв.
    // Закрашенностью, а не средней яркостью: штрихи букв занимают малую долю
    // строки, и средняя яркость подписи заведомо низкая при любом шрифте.
    const double iconCover = Covered(img, 6, 6, 36, 34);
    const double gapCover = Covered(img, 38, 4, 43, 36);
    const double textCover = Covered(img, 46, 4, 200, 36);
    std::printf("    значок+текст: значок %.2f, зазор %.2f, текст %.2f\n", iconCover, gapCover,
                textCover);
    Check(iconCover > 0.15, "значок нарисован");
    Check(textCover > 0.02, "подпись нарисована");
    // Полоса между ними пуста: значит, ничего никуда не наехало.
    Check(gapCover < 0.02, "между значком и подписью пусто");
}

// --- Ползунок остаётся полосой, а не плашкой --------------------------------
void CheckSliderStaysThin(UIRenderer& ui) {
    Scene scene("slider");
    GameObject e = Screen(scene, "Volume", {(float)kUiW, 40.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Slider"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 40.0f});
    const Image img = RenderUI(ui, scene);

    const double middle = Covered(img, 4, 16, kUiW - 4, 24);
    const double top = Covered(img, 60, 1, kUiW - 60, 8);
    std::printf("    ползунок: дорожка %.2f, верх элемента %.2f\n", middle, top);
    Check(middle > 0.8, "ползунок: дорожка по центру закрашена");
    Check(top < 0.1, "ползунок: верх элемента НЕ закрашен (это не плашка)");
}

// --- Цвет живёт у самой части ------------------------------------------------
//
// Перекрасить ползунок теперь можно его собственным полем. Раньше для этого
// на элемент вешали пустую полосу — правило, которое ниоткуда не следует.
void CheckRangeColourIsItsOwn(UIRenderer& ui) {
    Scene scene("colour");
    GameObject e = Screen(scene, "Volume", {(float)kUiW, 40.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Slider"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 40.0f});
    sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(e.Entity());
    r.Value = 1.0f;                              // заполнено целиком
    r.AccentColor = {0.1f, 0.1f, 0.1f, 1.0f};    // тёмный акцент
    const Image dark = RenderUI(ui, scene);
    r.AccentColor = {0.2f, 1.0f, 0.4f, 1.0f};    // яркий акцент
    const Image bright = RenderUI(ui, scene);

    const double a = Luma(dark, 4, 16, kUiW - 4, 24);
    const double b = Luma(bright, 4, 16, kUiW - 4, 24);
    std::printf("    ползунок: тёмный акцент %.1f, яркий %.1f\n", a, b);
    Check(b > a + 20.0, "цвет ползунка меняется его собственным полем");
}

// --- Демо-худ рисуется и не сваливается в кучу -------------------------------
void CheckHudDemoDraws(UIRenderer& ui) {
    Scene scene("hud");
    Check(sage::ui::BuildDemo(scene, "hud") >= 0, "демо-худ собрался");
    // Кадр в ОПОРНОМ разрешении холста: иначе координаты пришлось бы
    // пересчитывать масштабом холста, то есть повторять в тесте ту самую
    // формулу, которую тест и проверяет.
    const Image img = RenderUI(ui, scene, 1920, 1080);

    // Полоса здоровья — в левом верхнем углу (отступ 24, размер 320x34);
    // значок и шкала лежат в РАЗНЫХ прямоугольниках, поэтому закрашено и там,
    // и там.
    const double icon = Covered(img, 30, 30, 56, 52);
    const double bar = Covered(img, 70, 32, 330, 50);
    std::printf("    худ: значок %.2f, шкала %.2f\n", icon, bar);
    Check(icon > 0.15, "худ: значок здоровья нарисован");
    Check(bar > 0.5, "худ: шкала здоровья нарисована рядом, а не под значком");
}

// --- Начертание и свой шрифт надписи ----------------------------------------
//
// Жирный и курсив рисует сам движок (второй оттиск со сдвигом и наклон
// глифов), поэтому проверяется то, что от этого и ждут: жирный кладёт БОЛЬШЕ
// краски, чем обычный, а курсив — столько же, но не в тех же местах. Числами,
// а не эталонным кадром: форма букв зависит от шрифта системы, а «жирнее»
// и «наклонено» — нет.
void CheckFaceMakesTextBolderAndSlanted(UIRenderer& ui) {
    auto draw = [&](sage::ui::Label::Style face) {
        Scene scene("face");
        GameObject e = Screen(scene, "Caption", {(float)kUiW, 40.0f});
        sage::ui::Label label;
        label.Text = "Bold and slanted";
        label.Scale = 3.0f;
        label.Face = face;
        label.Horizontal = sage::ui::Label::Align::Start;
        label.Vertical = sage::ui::Label::Align::Center;
        scene.Registry().emplace<sage::ui::Label>(e.Entity(), label);
        return RenderUI(ui, scene);
    };

    const Image plain = draw(sage::ui::Label::Style::Regular);
    const Image bold = draw(sage::ui::Label::Style::Bold);
    const Image italic = draw(sage::ui::Label::Style::Italic);

    const double inkPlain = Covered(plain, 0, 0, kUiW, 40);
    const double inkBold = Covered(bold, 0, 0, kUiW, 40);
    const double inkItalic = Covered(italic, 0, 0, kUiW, 40);
    std::printf("    начертание: обычный %.4f, жирный %.4f, курсив %.4f\n", inkPlain, inkBold,
                inkItalic);
    Check(inkPlain > 0.002, "надпись нарисована");
    // Жирный — это второй оттиск со сдвигом: краски заметно больше.
    Check(inkBold > inkPlain * 1.15, "жирный кладёт больше краски, чем обычный");
    // Курсив краски добавляет немного (наклон), но САМА КАРТИНКА другая:
    // сравниваем верхнюю полосу строки, куда уезжают наклонённые верхушки.
    const double topPlain = Covered(plain, 0, 8, kUiW, 16);
    const double topItalic = Covered(italic, 0, 8, kUiW, 16);
    std::printf("    курсив: верх строки обычный %.4f, курсив %.4f\n", topPlain, topItalic);
    Check(std::fabs(topItalic - topPlain) > 0.0005, "курсив наклоняет глифы");
}

// Свой шрифт у надписи: берётся ИМЕННО он, а не шрифт интерфейса. Проверяем
// тем, что доступно без второго файла в репозитории, — размером букв: тот же
// текст, набранный своим шрифтом с ВДВОЕ меньшей высотой запекания, обязан
// остаться таким же по размеру на экране (высота запекания — про чёткость
// атласа, а не про кегль), и при этом надпись обязана нарисоваться.
void CheckLabelUsesItsOwnFontFile(UIRenderer& ui) {
    // Шрифт берётся ИЗ РЕПОЗИТОРИЯ (тесты запускаются из его корня — оттуда же
    // им передают tests/render/references): рядом с этим бинарником движковых
    // ассетов нет, а тест обязан работать на настоящем файле, а не на том, что
    // найдётся случайно.
    const std::string path = "engine/assets/fonts/sage-default.ttf";
    auto draw = [&](const std::string& font, float bake) {
        Scene scene("font");
        GameObject e = Screen(scene, "Caption", {(float)kUiW, 40.0f});
        sage::ui::Label label;
        label.Text = "Font file";
        label.Scale = 3.0f;
        label.Font = font;
        label.FontPixelHeight = bake;
        label.Horizontal = sage::ui::Label::Align::Start;
        label.Vertical = sage::ui::Label::Align::Center;
        scene.Registry().emplace<sage::ui::Label>(e.Entity(), label);
        return RenderUI(ui, scene);
    };

    // СНАЧАЛА убеждаемся, что файл вообще открылся. Без этой проверки тест
    // был бы пустым: не открывшийся шрифт молча заменяется шрифтом интерфейса,
    // и «надпись нарисована» оказалось бы правдой при полностью сломанной
    // загрузке своих шрифтов.
    const Font* loaded = ui.LoadFont(path);
    Check(loaded != nullptr, "свой шрифт открылся из файла");
    // А несуществующий файл даёт отказ, а не пустой шрифт с нулевыми метриками.
    Check(ui.LoadFont("engine/assets/fonts/no-such-font.ttf") == nullptr,
          "отсутствующий файл шрифта не притворяется загруженным");

    const Image own = draw(path, 48.0f);
    const double ink = Covered(own, 0, 0, kUiW, 40);
    std::printf("    свой шрифт: краска %.4f\n", ink);
    Check(ink > 0.002, "надпись со своим шрифтом нарисована");

    // Та же надпись, запечённая вдвое мельче: на экране тот же кегль (разница
    // только в чёткости), то есть доля краски рядом.
    const Image coarse = draw(path, 24.0f);
    const double inkCoarse = Covered(coarse, 0, 0, kUiW, 40);
    std::printf("    свой шрифт: краска при мелком запекании %.4f\n", inkCoarse);
    Check(inkCoarse > ink * 0.6 && inkCoarse < ink * 1.6,
          "высота запекания меняет чёткость, а не кегль");
}

// --- Фильтрация картинки: меняет РЕЗКОСТЬ, а не размер ----------------------
//
// Жалоба: «фильтрация не работает — ничего не меняется, только картинка на
// пиксель меньше становится». Так и было: резкость съедал общий кэш текстур
// (см. кадровые проверки текстур), а единственным видимым следствием
// переключателя оставалось округление масштаба до целого, которое включалось
// заодно. Проверяем обе стороны развязки: резкость меняется, размер — нет.
void CheckFilteringChangesSharpnessNotSize(UIRenderer& ui) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_ui_filter";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "tiny.png";
    // Шахматка 4x4 из очень крупных клеток: увеличенная до элемента, она
    // показывает разницу фильтраций в чистом виде — сглаживание размывает
    // границы клеток, ближайший сосед оставляет их ступенькой.
    {
        Image tiny;
        tiny.Width = tiny.Height = 4;
        tiny.Pixels.assign((size_t)4 * 4 * 3, 0);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const size_t i = ((size_t)y * 4 + x) * 3;
                const unsigned char v = ((x + y) % 2) ? 245 : 20;
                tiny.Pixels[i] = tiny.Pixels[i + 1] = tiny.Pixels[i + 2] = v;
            }
        }
        if (!SavePng(file.string(), tiny)) {
            std::printf("    не удалось записать картинку — проверка пропущена\n");
            CountFail();
            return;
        }
    }

    auto shot = [&](sage::ui::TextureFiltering filtering) {
        Scene scene("filter");
        GameObject e = Screen(scene, "Pic", {160.0f, 160.0f});
        sage::ui::Image img;
        img.Path = file.string();
        img.Filtering = filtering;
        scene.Registry().emplace<sage::ui::Image>(e.Entity(), img);
        return RenderUI(ui, scene);
    };

    const Image smooth = shot(sage::ui::TextureFiltering::Smooth);
    const Image sharp = shot(sage::ui::TextureFiltering::Nearest);

    // Полутона: пиксели, которые не чёрные и не белые, — это и есть размытые
    // границы клеток. У ближайшего соседа их почти нет.
    auto midtones = [](const Image& img) {
        int mid = 0, total = 0;
        for (int y = 4; y < 156; ++y) {
            for (int x = 4; x < 156; ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                const int v = img.Pixels[i];
                ++total;
                if (v > 60 && v < 200) ++mid;
            }
        }
        return total ? (double)mid / total : 0.0;
    };
    const double midSmooth = midtones(smooth);
    const double midSharp = midtones(sharp);
    std::printf("    фильтрация: полутонов при сглаживании %.3f, при ближайшем соседе %.3f\n",
                midSmooth, midSharp);
    Check(midSmooth > midSharp * 2.0 + 0.01, "фильтрация меняет резкость картинки");

    // А РАЗМЕР — НЕ МЕНЯЕТ. Закрашенная площадь у обоих кадров одна и та же:
    // округление масштаба переехало в свою настройку («Кратный масштаб»).
    const double coverSmooth = Covered(smooth, 0, 0, 160, 160);
    const double coverSharp = Covered(sharp, 0, 0, 160, 160);
    std::printf("    фильтрация: закрашено %.3f и %.3f\n", coverSmooth, coverSharp);
    Check(std::fabs(coverSmooth - coverSharp) < 0.02, "фильтрация не меняет размер картинки");

    // И ТО ЖЕ САМОЕ — У ФАЙЛА С ПРОБЕЛАМИ И КИРИЛЛИЦЕЙ В ИМЕНИ. Наборы
    // спрайтов приезжают именно такими («Basic Charakter Spritesheet.png»), и
    // проверять картинку только на «tiny.png» значит не проверять тот случай,
    // с которым к движку и приходят.
    const fs::path spaced = dir / "Набор спрайтов 16x16.png";
    {
        std::error_code copyEc;
        fs::copy_file(file, spaced, fs::copy_options::overwrite_existing, copyEc);
        if (!copyEc) {
            Scene scene("spaced");
            GameObject e = Screen(scene, "Pic", {160.0f, 160.0f});
            sage::ui::Image img;
            img.Path = spaced.string();
            img.Filtering = sage::ui::TextureFiltering::Nearest;
            scene.Registry().emplace<sage::ui::Image>(e.Entity(), img);
            const double cover = Covered(RenderUI(ui, scene), 0, 0, 160, 160);
            std::printf("    имя с пробелами и кириллицей: закрашено %.3f\n", cover);
            Check(cover > 0.9, "картинка из файла с пробелами в имени рисуется");
        }
    }

    fs::remove_all(dir, ec);
}

// --- Кегль значит то, что написано -------------------------------------------
//
// Жалоба: «размер шрифта становится вдвое больше текущего значения, а когда
// пытаюсь увеличивать — сначала текст подымается, потом скачком становится
// большим». Обе половины — следствия одного: номинал вёрстки (8·кегль) и
// реальная высота строки шрифта расходились, а у резкого шрифта масштаб ещё и
// округлялся до целого, то есть у мелких кеглей не менялся вовсе, а потом
// удваивался.
void CheckFontSizeMatchesTheNumber(UIRenderer& ui) {
    // Высота закрашенного: у строки из заглавных без выносных элементов это
    // высота самих букв, и она обязана расти РОВНО пропорционально кеглю.
    auto textRows = [](const Image& img) {
        int top = -1, bottom = -1;
        for (int y = 0; y < img.Height; ++y) {
            bool lit = false;
            for (int x = 0; x < img.Width && !lit; ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                lit = img.Pixels[i] > 40;
            }
            if (lit) {
                if (top < 0) top = y;
                bottom = y;
            }
        }
        return std::pair<int, int>(top, bottom);
    };

    auto shot = [&](float size) {
        Scene scene("size");
        GameObject e = Screen(scene, "Caption", {(float)kUiW, (float)kUiH});
        sage::ui::Label label;
        label.Text = "HHHH";
        label.Scale = size;
        label.Horizontal = sage::ui::Label::Align::Center;
        label.Vertical = sage::ui::Label::Align::Center;
        scene.Registry().emplace<sage::ui::Label>(e.Entity(), label);
        return RenderUI(ui, scene);
    };

    const Image small = shot(3.0f);
    const Image big = shot(6.0f);
    const auto rowsSmall = textRows(small);
    const auto rowsBig = textRows(big);
    const double hSmall = rowsSmall.second - rowsSmall.first + 1;
    const double hBig = rowsBig.second - rowsBig.first + 1;
    std::printf("    кегль: высота букв %.0f (размер 3) и %.0f (размер 6)\n", hSmall, hBig);
    Check(hSmall > 4.0 && hBig > 8.0, "текст нарисован при обоих кеглях");
    // Вдвое больший кегль — вдвое более высокие буквы. Ступеней («не менялось,
    // потом удвоилось») здесь быть не должно.
    Check(std::fabs(hBig / std::max(hSmall, 1.0) - 2.0) < 0.25, "кегль меняет размер линейно");
    // И буквы НЕ ВЫШЕ заявленной строки: 8 пикселей на единицу кегля — то, по
    // чему считает вёрстка, и заглавная обязана в неё помещаться.
    Check(hSmall <= 3.0 * 8.0 + 1.0 && hBig <= 6.0 * 8.0 + 1.0,
          "буквы помещаются в заявленную высоту строки");

    // ЦЕНТРИРОВАНИЕ. Оно и «подымало» текст: блок мерили номиналом, а рисовали
    // шрифтом, и промах рос вместе с кеглем.
    const double centerSmall = (rowsSmall.first + rowsSmall.second) * 0.5;
    const double centerBig = (rowsBig.first + rowsBig.second) * 0.5;
    std::printf("    кегль: центр строки %.1f и %.1f (центр элемента %.1f)\n", centerSmall,
                centerBig, kUiH * 0.5);
    // Допуск — пара пикселей, а не «на глаз»: промах центрирования РОС вместе
    // с кеглем, и мягкий допуск пропустил бы ровно то, на что жалуются.
    Check(std::fabs(centerSmall - kUiH * 0.5) < 3.0, "мелкий текст стоит по центру");
    Check(std::fabs(centerBig - kUiH * 0.5) < 3.0, "крупный текст стоит по центру");

    // И ГЛАВНОЕ: заявленная высота строки совпадает с настоящей. Пока они
    // расходились, вёрстка мерила блок одним числом, а рисовала другим —
    // отсюда и «текст подымается», и «размер не тот, что в поле».
    for (float size : {2.0f, 3.5f, 6.0f}) {
        const float nominal = ui.TextHeight(size);
        const float real = ui.LineHeight(size);
        std::printf("    кегль %.1f: заявлено %.2f, у шрифта %.2f\n", size, nominal, real);
        Check(std::fabs(real - nominal) < nominal * 0.02f,
              "высота строки совпадает с заявленной");
    }
}

// --- Резкий шрифт тоже слушается кегля ---------------------------------------
//
// Ровно то, на что жалуются: «размер шрифта становится вдвое больше значения, а
// увеличиваю — сначала текст подымается, потом скачком большим становится». Так
// и было у шрифта с резкой фильтрацией: масштаб глифа округлялся до целого САМ,
// поэтому у мелких кеглей упирался в единицу (то есть стоял на месте и крупнее
// заданного), а на каком-то шаге удваивался. Теперь округление — отдельная
// настройка («Кратный масштаб шрифта»), и по умолчанию его нет.
void CheckSharpFontStillFollowsTheSize(UIRenderer& ui) {
    auto height = [&](float size, bool snap) {
        Scene scene("sharp");
        GameObject e = Screen(scene, "Caption", {(float)kUiW, (float)kUiH});
        sage::ui::Label label;
        label.Text = "HHHH";
        label.Scale = size;
        label.Font = "engine/assets/fonts/sage-default.ttf";
        label.FontPixelHeight = 32.0f;
        label.FontFiltering = sage::ui::TextureFiltering::Nearest;
        label.FontSnapPixels = snap;
        label.Horizontal = sage::ui::Label::Align::Center;
        label.Vertical = sage::ui::Label::Align::Center;
        scene.Registry().emplace<sage::ui::Label>(e.Entity(), label);
        const Image img = RenderUI(ui, scene);
        int top = -1, bottom = -1;
        for (int y = 0; y < img.Height; ++y) {
            bool lit = false;
            for (int x = 0; x < img.Width && !lit; ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                lit = img.Pixels[i] > 40;
            }
            if (lit) { if (top < 0) top = y; bottom = y; }
        }
        return (double)(bottom - top + 1);
    };

    const double h2 = height(2.0f, /*snap=*/false);
    const double h4 = height(4.0f, /*snap=*/false);
    std::printf("    резкий шрифт: высота букв %.0f (размер 2) и %.0f (размер 4)\n", h2, h4);
    Check(h2 > 2.0 && h4 > 4.0, "резкий шрифт нарисован");
    Check(std::fabs(h4 / std::max(h2, 1.0) - 2.0) < 0.3,
          "резкая фильтрация не мешает кеглю меняться");

    // А включённый кратный масштаб — это по-прежнему ступени, и теперь так и
    // задумано: он для шрифтов, нарисованных по пикселям.
    const double snapped2 = height(2.0f, /*snap=*/true);
    const double snapped4 = height(4.0f, /*snap=*/true);
    std::printf("    кратный масштаб: %.0f и %.0f\n", snapped2, snapped4);
    Check(snapped2 > 0.0 && snapped4 > 0.0, "с кратным масштабом текст тоже рисуется");
}

// --- Поворот элемента поворачивает и КАРТИНКУ --------------------------------
//
// Из отчёта: «изображение, именно загруженная картинка, не поворачивается
// вообще». Так и было: поворот применялся к подложке и к буквам, а квад
// картинки складывался из неповёрнутых углов — единственное место, где это
// пропускалось.
void CheckImageRotates(UIRenderer& ui) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_ui_rotate";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "bar.png";
    {
        // Узкая горизонтальная полоса: повернув её на 90°, ни с чем не
        // перепутаешь — она станет вертикальной.
        Image bar;
        bar.Width = 32;
        bar.Height = 8;
        bar.Pixels.assign((size_t)bar.Width * bar.Height * 3, 0);
        for (size_t i = 0; i < bar.Pixels.size(); i += 3) {
            bar.Pixels[i] = 250;
            bar.Pixels[i + 1] = 250;
            bar.Pixels[i + 2] = 250;
        }
        if (!SavePng(file.string(), bar)) {
            std::printf("    не удалось записать картинку — проверка пропущена\n");
            CountFail();
            return;
        }
    }

    auto shot = [&](float degrees) {
        Scene scene("rotate");
        GameObject e = Screen(scene, "Pic", {120.0f, 30.0f});
        sage::ui::Element& box = scene.Registry().get<sage::ui::Element>(e.Entity());
        box.Rotation = degrees;
        sage::ui::Image img;
        img.Path = file.string();
        scene.Registry().emplace<sage::ui::Image>(e.Entity(), img);
        return RenderUI(ui, scene);
    };

    const Image flat = shot(0.0f);
    const Image turned = shot(90.0f);
    // Полоса 120x30 в левом верхнем углу: повёрнутая на 90° она уходит вниз за
    // пределы своей горизонтальной полосы и освобождает её правый конец.
    const double rightFlat = Covered(flat, 90, 2, 118, 28);
    const double rightTurned = Covered(turned, 90, 2, 118, 28);
    const double belowFlat = Covered(flat, 40, 40, 80, 90);
    const double belowTurned = Covered(turned, 40, 40, 80, 90);
    std::printf("    поворот картинки: правый конец %.2f -> %.2f, ниже элемента %.2f -> %.2f\n",
                rightFlat, rightTurned, belowFlat, belowTurned);
    Check(rightFlat > 0.8, "картинка нарисована");
    Check(rightTurned < 0.2, "повёрнутая картинка ушла с прежнего места");
    Check(belowTurned > belowFlat + 0.2, "повёрнутая картинка встала вертикально");

    fs::remove_all(dir, ec);
}

} // namespace

void RunUIChecks() {
    std::printf("\n--- Интерфейс игры ---\n");
    UIRenderer ui;
    CheckToggleDoesNotPaintTheRow(ui);
    CheckIconAndTextDoNotOverlap(ui);
    CheckSliderStaysThin(ui);
    CheckRangeColourIsItsOwn(ui);
    CheckHudDemoDraws(ui);
    CheckFaceMakesTextBolderAndSlanted(ui);
    CheckLabelUsesItsOwnFontFile(ui);
    CheckFilteringChangesSharpnessNotSize(ui);
    CheckFontSizeMatchesTheNumber(ui);
    CheckSharpFontStillFollowsTheSize(ui);
    CheckImageRotates(ui);
}

} // namespace sage::rendertest
