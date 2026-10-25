#include "sage/render/PostFXDetail.h"
#include "sage/render/PostFX.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/render/LensFlare.h"

using namespace sage::rhi;

// Встроенные виды эффектов и готовые цепочки (см. PostFXDetail.h).
namespace sage::render {

using namespace postfx;

void RegisterBuiltinPostEffects(PostEffectCatalog& catalog) {
    // ПОРЯДОК РЕГИСТРАЦИИ — ПОРЯДОК ОБРАБОТКИ. Список «что можно добавить» в
    // инспекторе идёт отсюда, и он обязан читаться как путь кадра: экспозиция,
    // глубина, свечение, цвет, тон-маппинг, оптика, плёнка, вывод.
    {
        PostEffectKind k;
        k.Id = "exposure";
        k.Label = "Exposure";
        k.Hint = "Сколько света собрал кадр. В ступенях: +1 — вдвое светлее";
        k.Stage = PostStage::Exposure;
        k.Params = {Param("exposure", "Exposure", 0.0f, -6.0f, 6.0f, "Ступени экспозиции (EV)")};
        k.Run = RunExposure;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "autoexposure";
        k.Label = "Auto Exposure";
        k.Hint = "Адаптация глаза: кадр сам подстраивается под яркость сцены —\n"
                 "в пещере светлеет, на солнце темнеет, плавно";
        k.Stage = PostStage::Exposure;
        k.Params = {Param("compensation", "Compensation", 1.0f, -4.0f, 4.0f,
                          "Поправка в ступенях: +1 — вдвое светлее"),
                    Param("minEV", "Min Brightness", -8.0f, -16.0f, 16.0f,
                          "Темнее этого (log2 яркости) сцену не вытягивать"),
                    Param("maxEV", "Max Brightness", 10.0f, -16.0f, 16.0f,
                          "Ярче этого (log2 яркости) сцену не притушивать"),
                    Param("lowPercent", "Low Percent", 10.0f, 0.0f, 99.0f,
                          "Сколько процентов самых тёмных точек кадра не учитывать"),
                    Param("highPercent", "High Percent", 90.0f, 1.0f, 100.0f,
                          "Выше какого процента яркости точки не учитывать (блики, лампы)"),
                    Param("blackLevel", "Black Level", -10.0f, -16.0f, 0.0f,
                          "Точки темнее этого (log2 яркости) — пустота: не участвуют в замере.\n"
                          "Иначе чёрное небо заставляет глаз вытягивать кадр в белое"),
                    Param("speedUp", "Speed Up", 3.0f, 0.0f, 20.0f,
                          "Как быстро глаз привыкает к свету (1/с)"),
                    Param("speedDown", "Speed Down", 1.0f, 0.0f, 20.0f,
                          "Как быстро глаз привыкает к темноте (1/с)")};
        k.Run = RunAutoExposure;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "ssgi";
        k.Label = "Global Illumination";
        k.Hint = "Отражённый свет: освещённые поверхности подсвечивают соседей своим цветом. "
                 "Считается по кадру, без запекания";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {Param("intensity", "Intensity", 1.0f, 0.0f, 4.0f, "Сила отражённого света"),
                    Param("radius", "Radius", 3.0f, 0.25f, 12.0f,
                          "Как далеко ищется отражающая поверхность, м"),
                    Param("thickness", "Thickness", 0.5f, 0.05f, 4.0f,
                          "Толщина поверхностей для лучей: больше — меньше просветов, но больше ложных попаданий"),
                    Param("saturation", "Color Bleeding", 1.0f, 0.0f, 2.0f,
                          "Насколько отражённый свет несёт цвет источника: 0 — серый, 1 — как есть"),
                    EnumParam("quality", "Quality", 1, {"Low", "Medium", "High"},
                              "Лучей на пиксель: 4, 8 или 12. Больше — чище и дороже")};
        k.Run = RunScreenSpaceGI;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "ao";
        k.Label = "Ambient Occlusion";
        k.Hint = "Затемняет щели и места контакта предметов; считается по глубине кадра";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.ProducesAux = postaux::kAO;
        k.Params = {Param("radius", "Radius", 0.5f, 0.05f, 2.0f, "Радиус выборки в метрах"),
                    Param("strength", "Strength", 1.0f, 0.0f, 4.0f, "Во сколько раз усилить затемнение")};
        k.Run = RunAmbientOcclusion;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "dof";
        k.Label = "Depth of Field";
        k.Hint = "Размывает то, что дальше или ближе плоскости фокуса";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {
            EnumParam("focusMode", "Focus", 0, {"Manual", "Screen centre"},
                      "Вручную — на дистанции ниже; по центру — на предмет в центре кадра"),
            Param("focus", "Focus Distance", 10.0f, 0.05f, 1000.0f, "Расстояние до плоскости фокуса, м"),
            Param("focusRange", "Focus Range", 0.0f, 0.0f, 100.0f,
                  "Толщина резкого слоя вокруг фокуса, м: всё внутри — резкое"),
            Param("aperture", "Aperture", 2.8f, 0.7f, 22.0f, "f-число: меньше — сильнее размытие"),
            Param("focalLength", "Focal Length", 85.0f, 10.0f, 300.0f,
                  "Фокусное расстояние объектива, мм: длиннее — сильнее размытие фона"),
            Param("nearBlur", "Near Blur", 1.0f, 0.0f, 4.0f, "Сила размытия ближе фокуса (0 — нет)"),
            Param("farBlur", "Far Blur", 1.0f, 0.0f, 4.0f, "Сила размытия дальше фокуса (0 — нет)"),
            Param("maxRadius", "Max Radius", 16.0f, 0.0f, 64.0f, "Потолок радиуса в пикселях"),
            EnumParam("quality", "Quality", 1, {"Low", "Medium", "High"},
                      "Сколько выборок на пиксель размытия: выше — ровнее боке, дороже"),
            BoolParam("showFocus", "Show Focus", false,
                      "Раскрасить кадр: в фокусе — зелёный, ближе — синий, дальше — оранжевый")};
        k.Run = RunDepthOfField;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "motionblur";
        k.Label = "Motion Blur";
        k.Hint = "Смаз движения камеры (и объектов, если у кадра есть буфер скоростей)";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {Param("amount", "Amount", 0.5f, 0.0f, 1.0f,
                          "Доля движения за кадр, которую размазать (выдержка затвора)"),
                    Param("samples", "Samples", 12.0f, 2.0f, 32.0f, "Выборок вдоль вектора"),
                    Param("maxLength", "Max Length", 5.0f, 0.1f, 25.0f,
                          "Потолок длины смаза, % ширины кадра")};
        k.Run = RunMotionBlur;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "lensflare";
        k.Label = "Lens Flare";
        k.Hint = "Блики в объективе: солнце (призраки, ореол, лучи), каждая лампа и прожектор\n"
                 "в кадре и всё яркое на экране. Гаснут, когда источник закрыт предметом";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {Param("intensity", "Intensity", 1.0f, 0.0f, 4.0f, "Сила блика"),
                    Param("ghosts", "Ghosts", 6.0f, 0.0f, 12.0f, "Сколько переотражений"),
                    Param("ghostSpacing", "Ghost Spacing", 0.32f, 0.05f, 1.0f,
                          "Шаг призраков по линии «солнце — центр кадра»"),
                    Param("ghostSize", "Ghost Size", 0.055f, 0.01f, 0.3f, "Размер первого призрака"),
                    Param("blades", "Aperture Blades", 6.0f, 0.0f, 12.0f,
                          "Лепестков диафрагмы: форма призраков (0 — круглые)"),
                    Param("halo", "Halo", 0.55f, 0.0f, 2.0f, "Сила кольца"),
                    Param("haloRadius", "Halo Radius", 0.42f, 0.05f, 1.0f, "Радиус кольца"),
                    Param("starburst", "Starburst", 0.5f, 0.0f, 2.0f, "Лучи вокруг солнца"),
                    Param("glare", "Glare", 0.45f, 0.0f, 2.0f, "Мягкое свечение вокруг солнца"),
                    Param("chroma", "Chroma", 0.55f, 0.0f, 2.0f, "Радужность призраков"),
                    Param("threshold", "Threshold", 1.1f, 0.0f, 16.0f,
                          "Яркость, начиная с которой пиксель — это солнце"),
                    Heading("Lights"),
                    Param("lights", "Light Flares", 1.0f, 0.0f, 4.0f,
                          "Свечение и звезда у точечных ламп и прожекторов в кадре"),
                    Param("lightSize", "Light Flare Size", 0.03f, 0.005f, 0.2f,
                          "Размер свечения лампы в метре от камеры"),
                    Heading("Screen"),
                    Param("screen", "Screen Flares", 0.6f, 0.0f, 4.0f,
                          "Призраки и ореол от всего яркого в кадре (самосвечение, блики)"),
                    Param("screenThreshold", "Screen Threshold", 1.5f, 0.1f, 16.0f,
                          "Яркость (HDR), с которой пиксель даёт экранный блик")};
        k.Run = RunLensFlare;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "bloom";
        k.Label = "Bloom";
        k.Hint = "Свечение ярких участков; считается до тон-маппинга";
        k.Stage = PostStage::Bloom;
        k.ProducesAux = postaux::kBloom;
        k.Params = {Param("intensity", "Bloom Intensity", 0.55f, 0.0f, 4.0f,
                          "Сила добавляемого свечения"),
                    Param("threshold", "Bloom Threshold", 1.0f, 0.0f, 8.0f,
                          "Яркость, выше которой пиксель светится"),
                    Param("radius", "Bloom Radius", 1.0f, 0.25f, 4.0f,
                          "Ширина свечения: шаг размытия")};
        k.Run = RunBloom;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "color";
        k.Label = "Color Adjustments";
        k.Hint = "Яркость, контраст, насыщенность и баланс белого — до тон-маппинга";
        k.Stage = PostStage::Color;
        k.Params = {
            Param("brightness", "Brightness", 1.0f, 0.0f, 2.0f, "Множитель яркости"),
            Param("contrast", "Contrast", 1.0f, 0.0f, 2.0f, "Контраст вокруг средне-серого"),
            Param("saturation", "Saturation", 1.0f, 0.0f, 2.0f, "Насыщенность"),
            Param("temperature", "Temperature", 0.0f, -1.0f, 1.0f, "Теплее (+) или холоднее (-)"),
            Param("tint", "Tint", 0.0f, -1.0f, 1.0f, "В зелёный (+) или в пурпурный (-)")};
        k.Run = RunColor;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "grading";
        k.Label = "Color Grading";
        k.Hint = "Свой оттенок теням, средним тонам и светам";
        k.Stage = PostStage::Grading;
        k.Params = {ColorParam("shadows", "Shadows", "Оттенок тёмных участков"),
                    ColorParam("midtones", "Midtones", "Оттенок средних тонов"),
                    ColorParam("highlights", "Highlights", "Оттенок светлых участков")};
        k.Run = RunGrading;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "tonemap";
        k.Label = "Tonemapping";
        k.Hint = "Переводит HDR-кадр в готовый к показу: кривая света и гамма";
        k.Stage = PostStage::Tonemap;
        k.Tonemaps = true;
        // Номера кривых только ДОПИСЫВАЮТСЯ в конец: сохранённые сцены хранят
        // номер, и вставка в середину молча сменила бы им кривую.
        // Умолчание звена — ACES, как было: сохранённые сцены не хранят
        // нетронутые значения, и смена умолчания молча перекрасила бы их.
        // Кинематографическую кривую получает НОВАЯ камера (см. PostChain::Default).
        k.Params = {EnumParam("mode", "Mode", 2,
                              {"Clamp", "Reinhard", "ACES", "Filmic", "Cinematic", "AgX", "Neutral"},
                              "Кривая света: чем переводить HDR в картинку.\n"
                              "Cinematic — мягкая плёночная кривая (у новой камеры), AgX — мягкие света,\n"
                              "Neutral — Khronos PBR Neutral: цвет материала без искажений"),
                    EnumParam("output", "Output", 0, {"sRGB", "Gamma", "Linear"},
                              "Кодирование для экрана: sRGB — стандарт мониторов,\n"
                              "Gamma — степенная кривая с показателем ниже,\n"
                              "Linear — без кодирования (для внешней обработки)"),
                    Param("gamma", "Gamma", 2.2f, 1.0f, 3.0f,
                          "Гамма экрана: 2.2 — стандарт, больше — светлее средние тона,\n"
                          "меньше — темнее. Работает при любом выводе")};
        k.Run = RunTonemap;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "vignette";
        k.Label = "Vignette";
        k.Hint = "Затемнение к краям кадра";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Lens;
        k.Params = {Param("intensity", "Intensity", 0.35f, 0.0f, 1.0f, "Насколько темнеет край"),
                    Param("smoothness", "Smoothness", 0.6f, 0.01f, 1.0f, "Мягкость перехода")};
        k.Run = RunVignette;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "chromatic";
        k.Label = "Chromatic Aberration";
        k.Hint = "Расхождение каналов к краям кадра — как в настоящей оптике";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Lens;
        k.Params = {Param("amount", "Amount", 0.3f, 0.0f, 1.0f, "Сила расхождения")};
        k.Run = RunChromatic;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "grain";
        k.Label = "Film Grain";
        k.Hint = "Зерно плёнки: живой шум поверх готового кадра";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Film;
        k.Params = {Param("amount", "Amount", 0.12f, 0.0f, 1.0f, "Сила зерна"),
                    Param("size", "Size", 1.5f, 0.5f, 8.0f, "Размер зерна в пикселях")};
        k.Run = RunGrain;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "fxaa";
        k.Label = "FXAA";
        k.Hint = "Сглаживает кромки по готовой картинке. С включённым MSAA не нужен: "
                 "он лечит то же самое и только размоет кадр";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Output;
        k.Params = {Param("threshold", "Contrast Threshold", 0.0625f, 0.0f, 0.5f,
                          "Ниже этого перепада пиксель не считается кромкой")};
        k.Run = RunFxaa;
        catalog.Register(std::move(k));
    }
}

// Тракт НОВОГО компонента «Пост-обработка».
//
// Здесь, а не в PostEffect.cpp, ровно по той же причине, что и раньше: форма
// умолчания неотделима от того, какие виды зарегистрировал движок, — имена
// звеньев и их параметры заданы прямо выше.
//
// Что в нём есть: экспозиция, адаптация глаза, свечение, цвет и тон-маппинг
// кинематографической кривой (у самого звена умолчание ACES — ради старых
// сцен). То есть то, что делает картинку картинкой, без единой правки. Чего нет: глубины резкости, смаза, зерна,
// аберрации — эффектов, которые нужны НЕ всегда и которые человек добавляет
// осознанно. Компонент, приезжающий со всем сразу, пришлось бы первым делом
// раздевать.
PostChain PostChain::Default() {
    PostChain chain;
    chain.Effects.push_back(MakePostEffect("exposure"));
    chain.Effects.push_back(MakePostEffect("autoexposure"));
    chain.Effects.push_back(MakePostEffect("ssgi"));
    // Блики в объективе — у новой камеры сразу: солнце, лампы и всё яркое
    // «горят» в кадре, как в Unreal. Убрать — одна галочка.
    chain.Effects.push_back(MakePostEffect("lensflare"));
    chain.Effects.push_back(MakePostEffect("bloom"));
    chain.Effects.push_back(MakePostEffect("color"));
    PostEffect tonemap = MakePostEffect("tonemap");
    if (PostValue* mode = tonemap.Find("mode")) mode->V[0] = 4.0f;   // Cinematic
    chain.Effects.push_back(std::move(tonemap));
    return chain;
}

PostChain PostChain::Display() {
    PostChain chain;
    // Отражённый свет — часть ОСВЕЩЕНИЯ, а не эффект: без него всё, что вне
    // прямого света, заливается одним ровным ambient. Выключается он настройкой
    // графики (EngineConfig::ScreenSpaceGI), а не тем, что у камеры нет обработки.
    chain.Effects.push_back(MakePostEffect("ssgi"));
    PostEffect tonemap = MakePostEffect("tonemap");
    if (PostValue* mode = tonemap.Find("mode")) mode->V[0] = 4.0f;   // Cinematic, как у новой камеры
    chain.Effects.push_back(std::move(tonemap));
    return chain;
}

} // namespace sage::render
