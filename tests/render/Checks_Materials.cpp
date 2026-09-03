// ---------------------------------------------------------------------------
// МАТЕРИАЛ ДОХОДИТ ДО ПИКСЕЛЕЙ.
//
// Материалы были покрыты проверками только «изнутри»: что .sagemat читается,
// что кэш отдаёт тот же указатель, что поля переживают запись. Всё это может
// быть верно, а объект на экране — не меняться, и ровно это и произошло: путь
// от правки в редакторе до цвета на объекте не проверял никто.
//
// Здесь проверяется он целиком и по КАРТИНКЕ: назначили материал — объект
// перекрасился; подвинули ползунок — кадр изменился; сохранили и перечитали —
// выглядит так же. Числа берутся из области экрана, где стоит сам объект, а не
// из среднего по кадру: небо и пол дали бы одинаковый ответ на что угодно.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/RenderComponents.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;

namespace sage::rendertest {
namespace {

// Средний цвет прямоугольника кадра в долях 0..1. Прямоугольник задаётся
// долями ширины и высоты, чтобы проверки не зависели от размера кадра.
glm::vec3 PatchColor(const Image& img, float x0, float y0, float x1, float y1) {
    if (img.Empty()) return glm::vec3(0.0f);
    const int ix0 = (int)(x0 * img.Width), ix1 = (int)(x1 * img.Width);
    const int iy0 = (int)(y0 * img.Height), iy1 = (int)(y1 * img.Height);
    double r = 0, g = 0, b = 0;
    long n = 0;
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            if (i + 2 >= img.Pixels.size()) continue;
            r += img.Pixels[i]; g += img.Pixels[i + 1]; b += img.Pixels[i + 2];
            ++n;
        }
    }
    if (n == 0) return glm::vec3(0.0f);
    return glm::vec3((float)(r / n / 255.0), (float)(g / n / 255.0), (float)(b / n / 255.0));
}

// Сцена под проверку материала: один шар в центре кадра на тёмном полу.
// Шар, а не куб: на изогнутой поверхности видно и блик, и шероховатость, то
// есть ровно то, ради чего материал и настраивают.
std::unique_ptr<Scene> MakeMaterialScene(GameObject& outBall) {
    auto scene = std::make_unique<Scene>("MaterialTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
    scene->Lighting.Sun.Intensity = 1.6f;
    scene->Lighting.Sun.Color = {1.0f, 1.0f, 1.0f};
    scene->Lighting.SkyColor = {0.10f, 0.11f, 0.13f};
    scene->Lighting.GroundColor = {0.05f, 0.05f, 0.05f};
    scene->Lighting.AmbientStrength = 0.25f;
    scene->Lighting.Skybox.Enabled = false;

    GameObject ground = scene->CreateObject("Ground");
    ground.GetTransform().Scale = {14.0f, 1.0f, 14.0f};
    ground.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
    ground.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);
    ground.Renderer().Color = {0.06f, 0.06f, 0.07f};

    outBall = scene->CreateObject("Ball");
    outBall.GetTransform().Position = {0.0f, 1.0f, 0.0f};
    outBall.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    outBall.Renderer().Ref = MeshRef{MeshRef::Type::Sphere};
    outBall.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
    return scene;
}

// Пятно в середине кадра — там, где стоит шар.
glm::vec3 BallColor(const Image& img) { return PatchColor(img, 0.42f, 0.36f, 0.58f, 0.56f); }

// Самый яркий пиксель области — то есть БЛИК.
//
// Шероховатость видна именно в нём: она не красит поверхность, а размазывает
// отражение источника. В среднем по шару её почти не видно (энергия та же,
// просто распределена шире), и мерить её средним — значит написать проверку,
// которая пройдёт и при полностью потерянном параметре.
float BallHighlight(const Image& img) {
    if (img.Empty()) return 0.0f;
    const int ix0 = (int)(0.34f * img.Width), ix1 = (int)(0.66f * img.Width);
    const int iy0 = (int)(0.28f * img.Height), iy1 = (int)(0.64f * img.Height);
    int best = 0;
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            if (i + 2 >= img.Pixels.size()) continue;
            const int luma = (img.Pixels[i] * 30 + img.Pixels[i + 1] * 59 + img.Pixels[i + 2] * 11) / 100;
            if (luma > best) best = luma;
        }
    }
    return (float)best / 255.0f;
}

Image Shot(FrameRenderer& r, Scene& scene) {
    return RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH);
}

// --- 1. Материал красит объект, и правка видна СРАЗУ -----------------------
//
// «Сразу» — это без пересоздания сущности и без перезагрузки сцены: редактор
// правит поля в общем экземпляре Material, и следующий же кадр обязан выйти
// другим. Проверяется именно так, как это делает редактор.
void TestMaterialPaintsAndUpdatesLive(FrameRenderer& r) {
    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);

    auto material = std::make_shared<Material>();
    material->Albedo = {0.85f, 0.15f, 0.15f};   // красный
    material->Metallic = 0.0f;
    material->Roughness = 0.6f;
    ball.Renderer().MaterialPtr = material;

    const glm::vec3 red = BallColor(Shot(r, *scene));

    // Ползунок цвета в редакторе — это ровно такая же запись в поле.
    material->Albedo = {0.15f, 0.15f, 0.85f};   // синий
    const glm::vec3 blue = BallColor(Shot(r, *scene));

    std::printf("       материал: красный (%.2f, %.2f, %.2f) -> синий (%.2f, %.2f, %.2f)\n",
                red.r, red.g, red.b, blue.r, blue.g, blue.b);

    Check(red.r > red.b + 0.10f, "материал красит объект в свой цвет");
    Check(blue.b > blue.r + 0.10f, "правка материала видна в следующем же кадре");
}

// --- 2. Один файл — один материал, как бы путь ни написали -----------------
//
// ГЛАВНАЯ проверка этого набора. В редакторе сущность держит ссылку
// ОТНОСИТЕЛЬНО ПРОЕКТА ("assets/x.sagemat"), а панель ассетов — настоящий путь
// в файловой системе. Пока ключом кэша был путь «как дали», на один файл
// заводилось два объекта Material: редактор правил свой, рендер рисовал свой.
// Со стороны это и выглядело как «редактор материалов не работает».
//
// Здесь тот же путь пройден до пикселей: назначаем ОТНОСИТЕЛЬНЫМ, правим
// АБСОЛЮТНЫМ, смотрим на объект.
void TestEditingByAnotherSpellingReachesTheObject(FrameRenderer& r) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_matpath";
    fs::create_directories(dir / "assets", ec);
    const fs::path saved = fs::current_path(ec);
    fs::current_path(dir, ec);

    {
        std::ofstream f("assets/ball.sagemat");
        f << R"({"albedo":[0.85,0.15,0.15],"metallic":0.0,"roughness":0.6})";
    }

    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);

    // Так материал приезжает из сцены: ссылкой относительно проекта.
    ball.Renderer().MaterialPath = "assets/ball.sagemat";
    ball.Renderer().MaterialPtr = ResourceManager::Instance().GetMaterial("assets/ball.sagemat");
    const glm::vec3 before = BallColor(Shot(r, *scene));

    // А так его открывает панель ассетов — полным путём.
    const std::string full = (fs::current_path(ec) / "assets" / "ball.sagemat").string();
    std::shared_ptr<Material> edited = ResourceManager::Instance().GetMaterial(full);
    Check(edited == ball.Renderer().MaterialPtr, "один файл материала — один объект в кэше");
    if (edited) edited->Albedo = {0.15f, 0.15f, 0.85f};

    const glm::vec3 after = BallColor(Shot(r, *scene));
    std::printf("       правка другим написанием пути: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)\n",
                before.r, before.g, before.b, after.r, after.g, after.b);
    Check(before.r > before.b + 0.10f && after.b > after.r + 0.10f,
          "правка материала из панели ассетов доходит до объекта сцены");

    ResourceManager::Instance().Clear();
    fs::current_path(saved, ec);
    fs::remove_all(dir, ec);
}

// --- 3. Metallic и Roughness доходят до шейдера ----------------------------
//
// Это не «ещё два поля»: металл и диэлектрик считаются в шейдере разными
// формулами, и если фактор не доехал, материал выглядит правдоподобно —
// просто всегда одинаково. Проверяем по разнице кадров.
void TestPbrFactorsReachTheShader(FrameRenderer& r) {
    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);
    auto material = std::make_shared<Material>();
    material->Albedo = {0.9f, 0.9f, 0.9f};
    ball.Renderer().MaterialPtr = material;

    material->Metallic = 0.0f;
    material->Roughness = 0.15f;
    const glm::vec3 dielectric = BallColor(Shot(r, *scene));

    material->Metallic = 1.0f;
    const glm::vec3 metal = BallColor(Shot(r, *scene));

    const float metalDiff = std::abs(metal.r - dielectric.r);
    // Шероховатость меряется по БЛИКУ, а не по среднему цвету: она размазывает
    // отражение источника, почти не меняя суммарную яркость шара.
    material->Metallic = 0.0f;
    material->Roughness = 0.05f;
    const float sharp = BallHighlight(Shot(r, *scene));
    material->Roughness = 1.0f;
    const float matte = BallHighlight(Shot(r, *scene));

    std::printf("       диэлектрик %.3f, металл %.3f (разница %.3f); блик: гладкий %.3f, матовый %.3f\n",
                dielectric.r, metal.r, metalDiff, sharp, matte);
    // Металл теряет рассеянную составляющую и заметно темнеет.
    Check(metalDiff > 0.05f, "Metallic доходит до шейдера");
    Check(sharp > matte + 0.02f, "Roughness доходит до шейдера: гладкий бликует ярче матового");
}

// --- 4. Прозрачность действительно смешивает -------------------------------
//
// Полупрозрачный материал уходит в отдельный проход. Ошибка здесь выглядит
// одним из двух: объект рисуется как непрозрачный (прозрачность потерялась) или
// пропадает совсем (не попал ни в один проход). Оба случая ловятся сравнением
// с фоном за объектом.
void TestOpacityBlends(FrameRenderer& r) {
    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);
    auto material = std::make_shared<Material>();
    material->Albedo = {0.9f, 0.9f, 0.9f};
    ball.Renderer().MaterialPtr = material;

    const glm::vec3 opaque = BallColor(Shot(r, *scene));

    // Тот же кадр без шара — фон, сквозь который будем смотреть.
    ball.Renderer().MeshPtr = nullptr;
    const glm::vec3 empty = BallColor(Shot(r, *scene));
    ball.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);

    material->Opacity = 0.35f;
    const glm::vec3 glass = BallColor(Shot(r, *scene));

    std::printf("       непрозрачный %.3f, стекло %.3f, пусто %.3f\n", opaque.r, glass.r, empty.r);
    // Стекло обязано быть МЕЖДУ пустым кадром и непрозрачным шаром: ближе к
    // непрозрачному — прозрачность потерялась, совпало с пустым — объект
    // не нарисовался вовсе.
    Check(std::abs(glass.r - opaque.r) > 0.05f, "прозрачность не игнорируется");
    Check(std::abs(glass.r - empty.r) > 0.03f, "полупрозрачный объект всё-таки рисуется");
}

// --- 5. Круг «сохранить -> перечитать» не меняет вид -----------------------
//
// Редактор пишет .sagemat и потом читает его обратно. Если запись и чтение
// разойдутся хоть в одном поле, объект после перезапуска проекта будет
// выглядеть иначе, чем его настроили, — и виноватым окажется «рендер».
void TestSaveReloadKeepsTheLook(FrameRenderer& r) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_matroundtrip";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "gold.sagemat";

    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);

    Material authored;
    authored.Albedo = {0.90f, 0.70f, 0.25f};
    authored.Metallic = 0.85f;
    authored.Roughness = 0.25f;
    authored.Emissive = {0.10f, 0.05f, 0.0f};
    authored.EmissiveStrength = 1.5f;
    authored.Opacity = 1.0f;
    authored.Render.UVScaleX = 3.0f;

    auto inMemory = std::make_shared<Material>(authored);
    ball.Renderer().MaterialPtr = inMemory;
    const glm::vec3 before = BallColor(Shot(r, *scene));

    authored.SaveToFile(file.string());
    auto loaded = std::make_shared<Material>(Material::LoadFromFile(file.string()));
    ball.Renderer().MaterialPtr = loaded;
    const glm::vec3 after = BallColor(Shot(r, *scene));

    const float diff = std::abs(before.r - after.r) + std::abs(before.g - after.g) +
                       std::abs(before.b - after.b);
    std::printf("       до записи (%.3f, %.3f, %.3f), после чтения (%.3f, %.3f, %.3f), расхождение %.4f\n",
                before.r, before.g, before.b, after.r, after.g, after.b, diff);
    Check(diff < 0.01f, "материал переживает запись и чтение без изменения вида");
    Check(std::abs(loaded->Render.UVScaleX - 3.0f) < 1e-4f, "свойства рендера переживают файл");

    fs::remove_all(dir, ec);
}

// --- 6. Свечение светится ---------------------------------------------------
void TestEmissiveShows(FrameRenderer& r) {
    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);
    auto material = std::make_shared<Material>();
    material->Albedo = {0.2f, 0.2f, 0.2f};
    ball.Renderer().MaterialPtr = material;

    const glm::vec3 dark = BallColor(Shot(r, *scene));
    material->Emissive = {0.0f, 0.8f, 0.0f};
    material->EmissiveStrength = 2.0f;
    const glm::vec3 glowing = BallColor(Shot(r, *scene));

    std::printf("       без свечения зелёный %.3f, со свечением %.3f\n", dark.g, glowing.g);
    Check(glowing.g > dark.g + 0.15f, "свечение материала видно на объекте");
}

// --- 7. Карта альбедо и её ПОВТОР ------------------------------------------
//
// Текстурный путь отрисовки — отдельный от плоского цвета (см. RenderBatch:
// материал с картами уходит в него), и сломать его можно, не задев цветной.
// А повтор (UVScale) — то, без чего текстурированный пол невозможен: развёртка
// примитивов движка 0..1 на грань, и одна плитка растягивается на сто метров.
//
// Проверяется по КРОМКАМ, а не по среднему цвету: повтор не меняет среднюю
// яркость шахматки, он меняет число переходов. Средним такую проверку не
// написать — она прошла бы и при полностью потерянном множителе.
void TestAlbedoMapAndTiling(FrameRenderer& r) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_mattex";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "checker.png";

    // Шахматка 64x64 клетками по 8 пикселей: она сама себе эталон — по числу
    // кромок видно, сколько раз её уложили.
    Image checker;
    checker.Width = 64;
    checker.Height = 64;
    checker.Pixels.assign((size_t)checker.Width * checker.Height * 3, 0);
    for (int y = 0; y < checker.Height; ++y) {
        for (int x = 0; x < checker.Width; ++x) {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            const size_t i = ((size_t)y * checker.Width + x) * 3;
            checker.Pixels[i] = checker.Pixels[i + 1] = checker.Pixels[i + 2] = light ? 235 : 60;
        }
    }
    if (!SavePng(file.string(), checker)) {
        std::printf("       не удалось записать шахматку — проверка пропущена\n");
        CountFail();
        return;
    }

    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);
    // Плоскость, а не шар: повтор смотрят на полу, и на плоскости кромки
    // считаются без искажений сферической развёртки.
    ball.GetTransform().Position = {0.0f, 0.02f, 0.0f};
    ball.GetTransform().Scale = {6.0f, 1.0f, 6.0f};
    ball.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
    ball.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);

    auto material = std::make_shared<Material>();
    material->Albedo = {1.0f, 1.0f, 1.0f};
    material->Roughness = 0.9f;
    material->TexturePath = file.string();
    ResourceManager::Instance().ResolveMaterialTextures(*material);
    if (!material->AlbedoTex) {
        std::printf("       карта альбедо не загрузилась\n");
        CountFail();
        fs::remove_all(dir, ec);
        return;
    }
    ball.Renderer().MaterialPtr = material;

    // Число кромок в кадре: сумма модулей разностей соседних пикселей.
    auto edges = [](const Image& img) {
        long long sum = 0;
        for (int y = 0; y < img.Height; ++y) {
            for (int x = 1; x < img.Width; ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                sum += std::abs((int)img.Pixels[i] - (int)img.Pixels[i - 3]);
            }
        }
        return (double)sum / (double)(img.Width * img.Height);
    };

    material->Render.UVScaleX = material->Render.UVScaleY = 1.0f;
    const Image once = Shot(r, *scene);
    const double e1 = edges(once);

    material->Render.UVScaleX = material->Render.UVScaleY = 6.0f;
    const Image many = Shot(r, *scene);
    const double e6 = edges(many);

    std::printf("       кромок: один повтор %.2f, шесть повторов %.2f\n", e1, e6);
    Check(e1 > 1.0, "карта альбедо видна на объекте");
    Check(e6 > e1 * 1.5, "повтор текстуры (UVScale) доходит до шейдера");

    ResourceManager::Instance().Clear();
    fs::remove_all(dir, ec);
}

} // namespace

void RunMaterialChecks(FrameRenderer& r) {
    TestMaterialPaintsAndUpdatesLive(r);
    TestEditingByAnotherSpellingReachesTheObject(r);
    TestPbrFactorsReachTheShader(r);
    TestOpacityBlends(r);
    TestSaveReloadKeepsTheLook(r);
    TestEmissiveShows(r);
    TestAlbedoMapAndTiling(r);
}

} // namespace sage::rendertest
