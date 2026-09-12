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

// --- Карта, назначенная ПОСЛЕ загрузки материала ---------------------------
//
// Из отчёта человека: он назначает текстуру в слот материала и получает под
// слотом красное «Не удалось загрузить» — при том, что обложка ЭТОЙ ЖЕ
// текстуры в том же слоте прекрасно видна. Противоречие объясняется просто:
// обложка грузится по ПУТИ, а материал рисуется по УКАЗАТЕЛЮ, и указатель
// собирался ровно один раз — при первой загрузке материала. Дальше путь мог
// меняться сколько угодно: перетаскиванием, кнопкой «Обзор…», очисткой слота,
// скриптом, импортом материалов модели, — а указатель оставался прежним.
//
// Правило «поменял путь — перерезолвь» тут и не сработало: путь через
// перетаскивание его помнил, путь через «Обзор…» — нет.
void TestTextureAssignedAfterLoadReachesTheMaterial() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_mat_latetex";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Две ЗАМЕТНО разные картинки: по цвету видно, какая из них доехала.
    Image red, blue;
    red.Width = red.Height = 4;
    blue.Width = blue.Height = 4;
    red.Pixels.assign(4 * 4 * 3, 0);
    blue.Pixels.assign(4 * 4 * 3, 0);
    for (int i = 0; i < 4 * 4; ++i) {
        red.Pixels[(size_t)i * 3 + 0] = 220; red.Pixels[(size_t)i * 3 + 1] = 20;
        red.Pixels[(size_t)i * 3 + 2] = 20;
        blue.Pixels[(size_t)i * 3 + 0] = 20; blue.Pixels[(size_t)i * 3 + 1] = 20;
        blue.Pixels[(size_t)i * 3 + 2] = 220;
    }
    const fs::path redFile = dir / "red.png";
    const fs::path blueFile = dir / "blue.png";
    if (!SavePng(redFile.string(), red) || !SavePng(blueFile.string(), blue)) {
        std::printf("       не удалось записать картинки — проверка пропущена\n");
        CountFail();
        return;
    }

    // Материал на диске БЕЗ карт — как только что созданный «New Material».
    const fs::path matFile = dir / "late.sagemat";
    { std::ofstream f(matFile); f << R"({"albedo":[1,1,1],"metallic":0,"roughness":0.8})"; }

    ResourceManager& rm = ResourceManager::Instance();
    std::shared_ptr<Material> mat = rm.GetMaterial(matFile.string());
    Check(mat != nullptr, "материал прочитался");
    if (!mat) { fs::remove_all(dir, ec); return; }
    Check(mat->AlbedoTex == nullptr, "карт у нового материала нет");

    // Ровно то, что делает кнопка «Обзор…»: путь записан в поле материала, и
    // больше ничего.
    mat->TexturePath = redFile.string();
    std::shared_ptr<Material> again = rm.GetMaterial(matFile.string());
    Check(again == mat, "материал тот же самый");
    Check(again->AlbedoTex != nullptr, "назначенная карта доехала до материала");

    // И смена карты на другую — тоже: устаревшим считается любой разошедшийся
    // путь, а не только «был пустым».
    const std::shared_ptr<Texture> first = again->AlbedoTex;
    again->TexturePath = blueFile.string();
    std::shared_ptr<Material> third = rm.GetMaterial(matFile.string());
    Check(third->AlbedoTex != nullptr && third->AlbedoTex != first,
          "смена карты доезжает тоже");

    // Очистка слота — обратная сторона того же: пустой путь обязан снять карту,
    // иначе «Очистить» ничего не делает.
    third->TexturePath.clear();
    Check(rm.GetMaterial(matFile.string())->AlbedoTex == nullptr, "очистка слота снимает карту");

    rm.Clear();
    fs::remove_all(dir, ec);
}


// --- КАЖДАЯ КАРТА МАТЕРИАЛА ДОХОДИТ ДО КАДРА ---------------------------------
//
// Жалоба: «в материалах загружается только albedo, остальные не работают».
// Проверить это одним взглядом нельзя: карта, которая не доехала, не оставляет
// ни ошибки, ни пустого слота — кадр просто выглядит так, будто её и не
// назначали. Поэтому здесь каждая карта проверяется ОТДЕЛЬНО и по своему
// признаку: назначили — кадр обязан измениться именно так, как эта карта
// работает.
//
// Проверок пять, а не одна общая, чтобы по упавшей было видно, КАКАЯ карта
// потерялась: «материал сломался» — это не диагноз.
void TestEveryMapReachesTheFrame(FrameRenderer& r) {
    std::printf("=== Материал: карты, а не только albedo ===\n");
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "sage_mat_maps";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // Однотонная картинка заданного цвета: карты движка читаются из R-канала
    // (metallic/roughness/AO), а albedo и emissive — целиком.
    auto solid = [&](const char* name, int rr, int gg, int bb) {
        Image img;
        img.Width = img.Height = 8;
        img.Pixels.assign((size_t)img.Width * img.Height * 3, 0);
        for (int i = 0; i < img.Width * img.Height; ++i) {
            img.Pixels[(size_t)i * 3 + 0] = (unsigned char)rr;
            img.Pixels[(size_t)i * 3 + 1] = (unsigned char)gg;
            img.Pixels[(size_t)i * 3 + 2] = (unsigned char)bb;
        }
        const std::filesystem::path p = dir / name;
        return SavePng(p.string(), img) ? p.string() : std::string();
    };

    const std::string white = solid("white.png", 255, 255, 255);
    const std::string black = solid("black.png", 8, 8, 8);
    const std::string green = solid("green.png", 20, 220, 20);
    if (white.empty() || black.empty() || green.empty()) {
        std::printf("       картинки не записаны — проверка пропущена\n");
        CountFail();
        return;
    }

    GameObject ball;
    std::unique_ptr<Scene> scene = MakeMaterialScene(ball);
    auto material = std::make_shared<Material>();
    material->Albedo = {1.0f, 1.0f, 1.0f};
    material->Metallic = 1.0f;   // фактор на максимум: карта его УМНОЖАЕТ
    material->Roughness = 1.0f;
    ball.Renderer().MaterialPtr = material;

    ResourceManager& rm = ResourceManager::Instance();
    auto shoot = [&]() {
        rm.ResolveMaterialTextures(*material);
        return BallColor(Shot(r, *scene));
    };
    auto luma = [](const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };

    // --- 1. Albedo: цвет шара становится цветом карты ------------------------
    const glm::vec3 plain = shoot();
    material->TexturePath = green;
    const glm::vec3 withAlbedo = shoot();
    Check(withAlbedo.g > withAlbedo.r + 0.05f && withAlbedo.g > plain.g - 1.0f,
          "albedo-карта красит шар");
    material->TexturePath.clear();

    // --- 2. Metallic: металл при том же свете темнее диэлектрика -------------
    //
    // У металла нет диффузного отражения — он виден только бликом и отражением
    // окружения. Карта с чёрным R гасит металличность до нуля, то есть
    // возвращает диэлектрик; белая оставляет металл.
    material->MetallicMapPath = black;
    const float dielectric = luma(shoot());
    material->MetallicMapPath = white;
    const float metal = luma(shoot());
    std::printf("       metallic: карта чёрная %.3f, белая %.3f\n", dielectric, metal);
    Check(std::abs(dielectric - metal) > 0.02f, "metallic-карта доходит до шейдера");
    material->MetallicMapPath.clear();

    // --- 3. Roughness: гладкое даёт узкий яркий блик, шероховатое — размытый --
    //
    // Мерится ПО САМОМУ ЯРКОМУ пикселю шара, а не по среднему пятну: шероховатость
    // двигает блик, а блик — это десяток пикселей, который в среднем по пятну
    // тонет. Среднее здесь давало разницу в три тысячных и объявляло рабочую
    // карту сломанной.
    //
    // И на МЕТАЛЛЕ: у диэлектрика зеркальная составляющая мала, у металла она
    // единственная, и шероховатость на нём видна во всю силу.
    auto brightest = [&]() {
        const Image img = Shot(r, *scene);
        float best = 0.0f;
        for (int y = (int)(img.Height * 0.30f); y < (int)(img.Height * 0.62f); ++y)
            for (int x = (int)(img.Width * 0.36f); x < (int)(img.Width * 0.64f); ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                const float l = (0.2126f * img.Pixels[i] + 0.7152f * img.Pixels[i + 1] +
                                 0.0722f * img.Pixels[i + 2]) / 255.0f;
                best = std::max(best, l);
            }
        return best;
    };
    material->Metallic = 1.0f;
    material->RoughnessMapPath = black;
    rm.ResolveMaterialTextures(*material);
    const float smooth = brightest();
    material->RoughnessMapPath = white;
    rm.ResolveMaterialTextures(*material);
    const float rough = brightest();
    std::printf("       roughness: карта чёрная %.3f, белая %.3f (по яркому пикселю)\n", smooth,
                rough);
    Check(std::abs(smooth - rough) > 0.02f, "roughness-карта доходит до шейдера");
    material->RoughnessMapPath.clear();
    material->Metallic = 0.0f;

    // --- 4. AO: затенение умножает непрямой свет ------------------------------
    material->AOMapPath = white;
    const float lit = luma(shoot());
    material->AOMapPath = black;
    const float occluded = luma(shoot());
    std::printf("       AO: карта белая %.3f, чёрная %.3f\n", lit, occluded);
    Check(occluded < lit - 0.002f, "AO-карта затемняет непрямой свет");
    material->AOMapPath.clear();

    // --- 5. Emissive: свечение добавляется поверх освещения -------------------
    const float unlit = luma(shoot());
    material->Emissive = {1.0f, 1.0f, 1.0f};
    material->EmissiveMap = green;
    const glm::vec3 glow = shoot();
    std::printf("       emissive: без карты %.3f, с картой %.3f\n", unlit, luma(glow));
    Check(luma(glow) > unlit + 0.02f && glow.g > glow.r + 0.05f,
          "emissive-карта светится своим цветом");
    material->EmissiveMap.clear();
    material->Emissive = {0.0f, 0.0f, 0.0f};

    // --- 6. Normal: карта разворачивает нормали и меняет затенение ------------
    //
    // Ровная карта (128,128,255) — это «нормаль как есть», и кадр с ней обязан
    // совпасть с кадром без карты. Наклонная — обязан отличаться. Так проверка
    // отличает РАБОТАЮЩУЮ карту от карты, которая просто игнорируется: второе
    // тоже даёт «кадр не изменился».
    const glm::vec3 noNormal = shoot();
    material->NormalMapPath = solid("flat_n.png", 128, 128, 255);
    const glm::vec3 flatNormal = shoot();
    material->NormalMapPath = solid("tilt_n.png", 235, 128, 140);
    const glm::vec3 tiltNormal = shoot();
    std::printf("       normal: без карты %.3f, ровная %.3f, наклонная %.3f\n", luma(noNormal),
                luma(flatNormal), luma(tiltNormal));
    Check(std::abs(luma(flatNormal) - luma(noNormal)) < 0.05f, "ровная normal-карта ничего не меняет");
    Check(std::abs(luma(tiltNormal) - luma(flatNormal)) > 0.01f,
          "наклонная normal-карта меняет затенение");

    rm.Clear();
    std::filesystem::remove_all(dir, ec);
}

void RunMaterialChecks(FrameRenderer& r) {
    TestMaterialPaintsAndUpdatesLive(r);
    TestEditingByAnotherSpellingReachesTheObject(r);
    TestPbrFactorsReachTheShader(r);
    TestOpacityBlends(r);
    TestSaveReloadKeepsTheLook(r);
    TestEmissiveShows(r);
    TestAlbedoMapAndTiling(r);
    TestTextureAssignedAfterLoadReachesTheMaterial();
    TestEveryMapReachesTheFrame(r);
}

} // namespace sage::rendertest
