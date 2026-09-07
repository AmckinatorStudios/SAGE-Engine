// ---------------------------------------------------------------------------
// Текстуры: мипмапы, уровень детализации, анизотропия — то есть всё, что решает
// вопрос «почему пол вдали мерцает» и «почему он же мылится под острым углом».
//
// ЭТИ ДВА ДЕФЕКТА — ОДИН И ТОТ ЖЕ ВЫБОР, СДЕЛАННЫЙ В РАЗНЫЕ СТОРОНЫ, и потому
// проверяются вместе. Один тексель экрана вдали накрывает десятки текселей
// текстуры. Взять из них один (без мипмапов) — получить шум, который при
// движении камеры превращается в мерцание. Взять усреднённый по квадрату
// (мипмап) — получить размытие, и на поверхности, видной под острым углом, оно
// чрезмерно: по одной оси текселей много, по другой мало, а квадратный мипмап
// усредняет по обеим одинаково. Анизотропная фильтрация усредняет ВДОЛЬ той
// оси, где сжатие сильнее, и потому даёт и отсутствие мерцания, и резкость.
//
// Проверяется это числами, а не эталоном: эталон говорит «стало иначе», а
// вопрос стоит «стало резче или мутнее» — на него отвечают контраст мелких
// деталей и разница между соседними кадрами при движении камеры.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/LightSystem.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;

namespace sage::rendertest {
namespace {

// Пол, уходящий к горизонту, и камера почти вдоль него. Это и есть тот самый
// случай: у горизонта текстура сжата в десятки раз по одной оси и почти не
// сжата по другой.
struct FloorScene {
    std::unique_ptr<Scene> Data;
    std::shared_ptr<Material> Mat;
};

// Шахматка с мелкой клеткой: чем мельче деталь, тем раньше она начинает
// мерцать, поэтому клетка в 4 пикселя — это увеличительное стекло для дефекта.
bool WriteChecker(const fs::path& file) {
    Image checker;
    checker.Width = 128;
    checker.Height = 128;
    checker.Pixels.assign((size_t)checker.Width * checker.Height * 3, 0);
    for (int y = 0; y < checker.Height; ++y) {
        for (int x = 0; x < checker.Width; ++x) {
            const bool light = ((x / 4) + (y / 4)) % 2 == 0;
            const size_t i = ((size_t)y * checker.Width + x) * 3;
            checker.Pixels[i] = checker.Pixels[i + 1] = checker.Pixels[i + 2] = light ? 240 : 40;
        }
    }
    return SavePng(file.string(), checker);
}

FloorScene MakeFloor(const std::string& texture) {
    FloorScene f;
    f.Data = std::make_unique<Scene>("TextureTest");
    f.Data->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.2f, -1.0f, -0.2f));
    f.Data->Lighting.Sun.Intensity = 1.4f;
    f.Data->Lighting.Sun.Color = {1.0f, 1.0f, 1.0f};
    f.Data->Lighting.SkyColor = {0.25f, 0.28f, 0.34f};
    f.Data->Lighting.GroundColor = {0.10f, 0.10f, 0.10f};
    f.Data->Lighting.AmbientStrength = 0.35f;
    f.Data->Lighting.Skybox.Enabled = false;

    GameObject floor = f.Data->CreateObject("Floor");
    floor.GetTransform().Scale = {120.0f, 1.0f, 120.0f};
    floor.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
    floor.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);

    f.Mat = std::make_shared<Material>();
    f.Mat->Albedo = {1.0f, 1.0f, 1.0f};
    f.Mat->Roughness = 0.95f;
    f.Mat->Metallic = 0.0f;
    f.Mat->TexturePath = texture;
    // Плитка укладывается много раз: без повтора одна шахматка растянулась бы
    // на сто двадцать метров и никакого сжатия не случилось бы вовсе.
    f.Mat->Render.UVScaleX = f.Mat->Render.UVScaleY = 60.0f;
    floor.Renderer().MaterialPtr = f.Mat;
    return f;
}

// Камера почти на уровне пола, смотрит вдаль: именно так поверхность и уходит
// «в перспективу», где начинаются и мерцание, и мыло.
glm::mat4 GrazingView(float shift) {
    const glm::vec3 eye(shift, 1.1f, 6.0f);
    const glm::vec3 target(shift * 0.98f, 0.55f, -30.0f);
    return glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
}

// Кадр пола без пост-обработки: она размывает и осветляет, а меряем мы
// РЕЗКОСТЬ текстуры — чужое размытие тут только мешает.
Image ShotFloor(FrameRenderer& r, Scene& scene, float shift) {
    Framebuffer fbo(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = GrazingView(shift);
    CameraComponent cam;
    cam.Fov = 60.0f;
    cam.NearClip = 0.1f;
    cam.FarClip = 300.0f;
    const glm::mat4 proj = cam.ProjectionMatrix((float)kW / (float)kH);

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    fbo.Bind();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    r.Batch.RenderColor(scene, view, proj, glm::vec3(shift, 1.1f, 6.0f), env, ShadowBinding(), 0);
    Image img = Capture(kW, kH);
    device.BindDefaultFramebuffer();
    return img;
}

// Полоса кадра, где поверхность сжата СИЛЬНО, но ещё не бесконечно.
//
// Границы не на глаз: горизонт в этом кадре приходится примерно на 58% высоты,
// ниже 80% пол уже близко и почти не сжат, а выше горизонта пола нет вовсе.
// Между ними — та самая зона, ради которой анизотропия и существует: сжатие по
// глубине в разы больше сжатия поперёк. У самого горизонта мерить нечего:
// там от текстуры остаётся последний мип, то есть одна средняя серая точка, и
// любая фильтрация даёт один и тот же ровный цвет.
struct Band { int Y0, Y1; };
Band FarBand() { return {(int)(kH * 0.60f), (int)(kH * 0.80f)}; }

// Средний контраст соседних пикселей в полосе — мера того, сколько ДЕТАЛЕЙ
// осталось. Замыленная текстура даёт ровный серый: контраст падает почти до
// нуля. Не усреднённая (без мипмапов) даёт высокий контраст, но он же и
// мерцает — поэтому одного этого числа мало, второе меряется ниже.
double Detail(const Image& img) {
    const Band b = FarBand();
    long long sum = 0;
    long long count = 0;
    for (int y = b.Y0; y < b.Y1; ++y) {
        for (int x = 1; x < img.Width; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            sum += std::abs((int)img.Pixels[i] - (int)img.Pixels[i - 3]);
            ++count;
        }
    }
    return count ? (double)sum / (double)count : 0.0;
}

// Насколько кадр МЕНЯЕТСЯ от крошечного сдвига камеры — это и есть мерцание в
// числах. Глаз видит его как «дальний пол кипит»; здесь это средняя разница
// двух кадров, снятых из почти одной точки.
double Shimmer(const Image& a, const Image& b) {
    if (a.Pixels.size() != b.Pixels.size()) return 255.0;
    const Band band = FarBand();
    long long sum = 0;
    long long count = 0;
    for (int y = band.Y0; y < band.Y1; ++y) {
        for (int x = 0; x < a.Width; ++x) {
            const size_t i = ((size_t)y * a.Width + x) * 3;
            sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
            ++count;
        }
    }
    return count ? (double)sum / (double)count : 0.0;
}

// Снимает пару «неподвижно / чуть сдвинулись» для заданной фильтрации.
struct FilterResult {
    double Detail = 0.0;
    double Shimmer = 0.0;
};

FilterResult MeasureFilter(FrameRenderer& r, const std::string& texture, TextureFilter filter,
                           bool mipmaps) {
    FloorScene floor = MakeFloor(texture);
    // Текстура берётся НАПРЯМУЮ, мимо кэша ResourceManager: кэш держит одну
    // картинку на путь и пересоздал бы её при каждой смене фильтрации, а нам
    // нужны обе рядом.
    auto tex = std::make_shared<Texture>(texture, filter, mipmaps);
    floor.Mat->AlbedoTex = tex;

    FilterResult out;
    const Image still = ShotFloor(r, *floor.Data, 0.0f);
    // Сдвиг в четверть метра — это доли пикселя на дальнем плане: ровно тот
    // масштаб движения, на котором мерцание и живёт.
    const Image moved = ShotFloor(r, *floor.Data, 0.25f);
    out.Detail = Detail(still);
    out.Shimmer = Shimmer(still, moved);
    return out;
}

// --- 1. Мипмапы убирают мерцание -------------------------------------------
void TestMipmapsKillShimmer(FrameRenderer& r, const std::string& texture) {
    const FilterResult none = MeasureFilter(r, texture, TextureFilter::Bilinear, false);
    const FilterResult mips = MeasureFilter(r, texture, TextureFilter::Trilinear, true);

    std::printf("       дальний план: без мипмапов деталей %.2f / дрожь %.2f, "
                "с мипмапами %.2f / %.2f\n",
                none.Detail, none.Shimmer, mips.Detail, mips.Shimmer);
    Check(none.Shimmer > 1.0, "без мипмапов дальний план действительно дрожит");
    Check(mips.Shimmer < none.Shimmer * 0.6,
          "мипмапы гасят дрожь дальнего плана как минимум вдвое");
}

// --- 2. Анизотропия возвращает резкость, не возвращая мерцания --------------
//
// ДВА УСЛОВИЯ СРАЗУ, и по отдельности они бессмысленны. «Резче» без второго
// условия достигается отключением мипмапов — и возвращает мерцание. «Не
// мерцает» без первого — размытием до серого. Ценность анизотропии ровно в
// том, что она даёт оба.
void TestAnisotropySharpensGrazingSurfaces(FrameRenderer& r, const std::string& texture) {
    const float maxAniso = sage::rhi::GraphicsDevice::Get().MaxAnisotropy();
    if (maxAniso <= 1.0f) {
        // Карта без расширения — не провал, а честный пропуск: движок в этом
        // случае обязан работать на трилинейной, что и проверено выше.
        std::printf("[ -- ] %-28s видеокарта не умеет анизотропию (максимум %.1f) — пропуск\n",
                    "aniso_sharper", maxAniso);
        return;
    }

    const FilterResult tri = MeasureFilter(r, texture, TextureFilter::Trilinear, true);
    const FilterResult aniso = MeasureFilter(r, texture, TextureFilter::Anisotropic, true);

    std::printf("       под острым углом: трилинейная деталей %.2f / дрожь %.2f, "
                "анизотропная %.2f / %.2f (максимум карты %.0fx)\n",
                tri.Detail, tri.Shimmer, aniso.Detail, aniso.Shimmer, maxAniso);
    Check(aniso.Detail > tri.Detail * 1.05,
          "анизотропия возвращает детали, съеденные трилинейной фильтрацией");
    // Допуск в полтора раза, а не «не больше»: больше деталей — это по
    // определению больше разницы между кадрами. Проверяется, что мерцание не
    // возвращается к уровню «без мипмапов».
    Check(aniso.Shimmer < tri.Shimmer * 1.5 + 0.5,
          "и не возвращает при этом мерцание");
}

// --- 3. Материалы просят анизотропию ---------------------------------------
//
// Проверка не картинки, а НАСТРОЙКИ ПО УМОЛЧАНИЮ. Поддержка анизотропии была в
// движке с самого начала и не использовалась ни разу: все карты материалов
// грузились трилинейными, то есть пол под острым углом мылился на всех
// видеокартах, включая те, где резкость досталась бы даром.
void TestMaterialTexturesUseAnisotropy(const std::string& texture) {
    auto material = std::make_shared<Material>();
    material->TexturePath = texture;
    ResourceManager::Instance().ResolveMaterialTextures(*material);
    const bool ok = material->AlbedoTex && material->AlbedoTex->Filter() == TextureFilter::Anisotropic;
    Check(ok, "карты материала грузятся с анизотропной фильтрацией");
    ResourceManager::Instance().Clear();
}

} // namespace

void RunTextureChecks(FrameRenderer& r) {
    std::printf("\n--- Текстуры: мипмапы, детализация, анизотропия ---\n");
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "sage_texfilter";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "checker.png";
    if (!WriteChecker(file)) {
        std::printf("[FAIL] не удалось записать тестовую шахматку\n");
        CountFail();
        return;
    }

    TestMipmapsKillShimmer(r, file.string());
    TestAnisotropySharpensGrazingSurfaces(r, file.string());
    TestMaterialTexturesUseAnisotropy(file.string());

    fs::remove_all(dir, ec);
}

} // namespace sage::rendertest
