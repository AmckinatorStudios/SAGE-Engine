// ---------------------------------------------------------------------------
// Небо и время суток.
//
// ЧТО ИМЕННО ПРОВЕРЯЕТСЯ. Жалоба звучала так: «солнце садится, а ничего не
// меняется — только звёзды и луна появляются». То есть ночь в движке была
// картинкой поверх дневной сцены: небо оставалось полуденным, окружающий свет
// тоже, а направленный свет продолжал бить прежней силой, теперь уже из-под
// земли.
//
// Ни один прежний тест этого не видел и увидеть не мог: все они снимают сцену
// при одном положении солнца. Поэтому здесь проверяется ЗАВИСИМОСТЬ — что
// меняется, когда солнце опускается, — а не отдельный кадр.
//
// Проверки числовые, без видеокарты: модель дня и ночи (sage/render/SkyModel.h)
// — чистая функция от настроек окружения, и это ровно тот случай, когда
// проверять надо арифметику, а не пиксели.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <cmath>
#include <string>

#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/SkyModel.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Skybox.h"
#include "sage/scene/Light.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scene/Scene.h"

namespace {

// Окружение с процедурным небом и солнцем, стоящим под заданным углом над
// горизонтом. Угол — то, чем время суток и задаётся: поворотом объекта-солнца.
LightingEnvironment EnvWithSunAt(float elevationDeg) {
    LightingEnvironment env;
    env.Skybox.Enabled = true;
    env.Skybox.Kind = SkyboxSettings::Source::Procedural;
    env.Skybox.DayNight = true;
    env.Skybox.Celestials = true;
    env.Sun.Intensity = 1.0f;
    env.Sun.Color = glm::vec3(1.0f, 0.95f, 0.85f);
    // Направление — КУДА летит свет: солнце над горизонтом светит вниз.
    const float rad = glm::radians(elevationDeg);
    env.Sun.Direction = glm::normalize(glm::vec3(0.3f, -std::sin(rad), -std::cos(rad)));
    return env;
}

float Luma(const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// --- 1. Ночь действительно темнее дня --------------------------------------
TEST(Sky_night_is_darker_than_day) {
    LightingEnvironment day = EnvWithSunAt(60.0f);
    LightingEnvironment night = EnvWithSunAt(-30.0f);
    sage::render::ApplySky(day);
    sage::render::ApplySky(night);

    CHECK_TRUE(day.DayFactor > 0.99f);
    CHECK_TRUE(night.DayFactor < 0.01f);

    // Небо.
    CHECK_TRUE(Luma(night.SkyTopNow) < Luma(day.SkyTopNow) * 0.3f);
    CHECK_TRUE(Luma(night.SkyHorizonNow) < Luma(day.SkyHorizonNow) * 0.3f);

    // Окружающий свет — то, чем освещены сами объекты. Именно он и не менялся:
    // ночная сцена оставалась освещена полуднем.
    glm::vec3 daySky, dayGround, nightSky, nightGround;
    day.ResolveAmbient(daySky, dayGround);
    night.ResolveAmbient(nightSky, nightGround);
    CHECK_TRUE(Luma(nightSky) < Luma(daySky) * 0.3f);
    CHECK_TRUE(Luma(nightGround) < Luma(dayGround) * 0.3f);

    // Направленный свет: днём солнце в полную силу, ночью — слабая луна.
    CHECK_NEAR(day.Sun.Intensity, 1.0f, 1e-3f);
    CHECK_TRUE(night.Sun.Intensity < 0.2f);
    CHECK_TRUE(night.Sun.Intensity > 0.0f); // ночь тёмная, но не чёрная
}

// --- 2. Ночью светит ЛУНА, и светит она с противоположной стороны -----------
//
// Это не украшение: тени ночью обязаны идти от луны. Пока слот занимало солнце,
// ушедшее под горизонт, направленный свет бил снизу — то есть подсвечивал
// объекты из-под земли.
TEST(Sky_moon_takes_over_at_night) {
    LightingEnvironment night = EnvWithSunAt(-30.0f);
    const glm::vec3 sunDir = night.Sun.Direction;
    sage::render::ApplySky(night);

    // Луна светит противоположно солнцу.
    const float dot = glm::dot(glm::normalize(night.Sun.Direction), glm::normalize(sunDir));
    CHECK_TRUE(dot < -0.99f);
    // И светит она сверху вниз: солнце ушло под горизонт, значит луна над ним.
    CHECK_TRUE(night.Sun.Direction.y < 0.0f);
    // Цвет — лунный, а не солнечный.
    CHECK_TRUE(night.Sun.Color.b > night.Sun.Color.r);
}

// --- 3. Диск солнца остаётся на своём месте ---------------------------------
//
// Ночью в слоте солнца лежит луна. Если бы небо рисовало диск по этому же полю,
// солнце всходило бы на западе.
TEST(Sky_sun_disc_keeps_authored_direction) {
    LightingEnvironment night = EnvWithSunAt(-30.0f);
    const glm::vec3 authored = -glm::normalize(night.Sun.Direction); // направление НА солнце
    sage::render::ApplySky(night);
    CHECK_TRUE(glm::dot(glm::normalize(night.SkySunDirection), authored) > 0.99f);
    // И оно под горизонтом — солнце село.
    CHECK_TRUE(night.SkySunDirection.y < 0.0f);
}

// --- 4. Закат — не «полдень потемнее» ---------------------------------------
TEST(Sky_sunset_adds_its_own_colour) {
    LightingEnvironment sunset = EnvWithSunAt(0.0f);
    LightingEnvironment noon = EnvWithSunAt(60.0f);
    sunset.Skybox.DuskColor = glm::vec3(1.0f, 0.4f, 0.1f);
    sage::render::ApplySky(sunset);
    sage::render::ApplySky(noon);

    // У горизонта на закате должно быть заметно «теплее», чем в полдень:
    // сравниваем разницу красного и синего.
    const float sunsetWarmth = sunset.SkyHorizonNow.r - sunset.SkyHorizonNow.b;
    const float noonWarmth = noon.SkyHorizonNow.r - noon.SkyHorizonNow.b;
    CHECK_TRUE(sunsetWarmth > noonWarmth + 0.15f);
}

// --- 5. Выключенная смена суток ничего не трогает ----------------------------
//
// Сцена со сценическим светом обязана остаться такой, какой её собрали: свет
// под выбранным углом и небо заданного цвета, сколько бы ни был опущен источник.
TEST(Sky_day_night_can_be_turned_off) {
    LightingEnvironment env = EnvWithSunAt(-40.0f);
    env.Skybox.DayNight = false;
    const glm::vec3 authoredTop = env.Skybox.TopColor;
    const float authoredIntensity = env.Sun.Intensity;
    sage::render::ApplySky(env);

    CHECK_NEAR(env.DayFactor, 1.0f, 1e-5f);
    CHECK_NEAR(env.SkyTopNow.r, authoredTop.r, 1e-5f);
    CHECK_NEAR(env.Sun.Intensity, authoredIntensity, 1e-5f);
}

// --- 6. Текстурное небо моделью суток не красится ----------------------------
//
// Время суток у кубической карты запечено в самих гранях. Подмешивать к ним
// ночной цвет — значит красить чужую фотографию.
TEST(Sky_textured_is_not_repainted_by_time_of_day) {
    LightingEnvironment env = EnvWithSunAt(-40.0f);
    env.Skybox.Kind = SkyboxSettings::Source::Cubemap;
    env.Skybox.CubemapDir = "assets/sky";
    const glm::vec3 authoredTop = env.Skybox.TopColor;
    sage::render::ApplySky(env);
    CHECK_NEAR(env.SkyTopNow.r, authoredTop.r, 1e-5f);
    CHECK_NEAR(env.DayFactor, 1.0f, 1e-5f);
}

// --- 7. Окружающий свет: свои значения остаются своими -----------------------
//
// Раньше правило было невидимым: включён скайбокс — ambient считается из него,
// и поля «свои значения» молча ничего не делали.
TEST(Ambient_custom_values_are_not_overridden_by_sky) {
    LightingEnvironment env = EnvWithSunAt(45.0f);
    env.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    env.SkyColor = glm::vec3(0.9f, 0.1f, 0.1f);
    env.GroundColor = glm::vec3(0.1f, 0.9f, 0.1f);
    sage::render::ApplySky(env);

    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    CHECK_NEAR(sky.r, 0.9f, 1e-5f);
    CHECK_NEAR(ground.g, 0.9f, 1e-5f);
}

TEST(Ambient_from_sky_follows_the_sky) {
    LightingEnvironment env = EnvWithSunAt(45.0f);
    env.AmbientMode = LightingEnvironment::AmbientSource::FromSky;
    env.SkyColor = glm::vec3(0.9f, 0.1f, 0.1f); // должно быть проигнорировано
    sage::render::ApplySky(env);

    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    // Небо голубое, а «свои значения» красные — берётся именно небо.
    CHECK_TRUE(sky.b > sky.r);
}

// --- 7b. НЕБА НЕТ — И СВЕТА ОТ НЕГО НЕТ --------------------------------------
//
// Ровно то, из-за чего сцена оставалась освещённой неизвестно чем: человек
// выключал небо, удалял ВСЕ источники света — и всё равно всё видел. Причина
// была в молчаливой подстановке: «от неба» при выключенном небе брало поля
// SkyColor/GroundColor, а они по умолчанию голубые и ненулевые.
TEST(Ambient_from_sky_is_black_without_a_sky) {
    LightingEnvironment env = EnvWithSunAt(45.0f);
    env.AmbientMode = LightingEnvironment::AmbientSource::FromSky;
    env.SkyColor = glm::vec3(0.9f, 0.9f, 0.9f);   // эти поля — чужого режима
    env.GroundColor = glm::vec3(0.9f, 0.9f, 0.9f);
    sage::render::ApplySky(env);
    env.Skybox.Enabled = false;

    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    CHECK_NEAR(sky.r, 0.0f, 1e-6f);
    CHECK_NEAR(sky.g, 0.0f, 1e-6f);
    CHECK_NEAR(sky.b, 0.0f, 1e-6f);
    CHECK_NEAR(ground.r, 0.0f, 1e-6f);
    CHECK_NEAR(ground.b, 0.0f, 1e-6f);

    // А художественный свет без неба никуда не делся: он живёт в «своих
    // значениях», и выключенное небо его не трогает.
    env.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    env.ResolveAmbient(sky, ground);
    CHECK_NEAR(sky.r, 0.9f, 1e-5f);
    CHECK_NEAR(ground.b, 0.9f, 1e-5f);
}

// Заливка кадра — тем же правилом: выключенное небо выглядит как ЕГО
// ОТСУТСТВИЕ, то есть чернота. Раньше здесь стоял SkyColor, и «небо выключено»
// оставалось голубым — то есть небо не исчезало, а становилось плоским.
TEST(Frame_clears_to_black_without_a_sky) {
    LightingEnvironment env;
    env.SkyColor = glm::vec3(0.5f, 0.6f, 0.9f);

    env.Skybox.Enabled = false;
    const glm::vec3 off = sage::render::SceneClearColor(env);
    CHECK_NEAR(off.r, 0.0f, 1e-6f);
    CHECK_NEAR(off.g, 0.0f, 1e-6f);
    CHECK_NEAR(off.b, 0.0f, 1e-6f);

    env.Skybox.Enabled = true;
    const glm::vec3 on = sage::render::SceneClearColor(env);
    CHECK_TRUE(on.b > 0.5f);
}

// --- 7c. НЕБО ОДНИМ ЦВЕТОМ ---------------------------------------------------
//
// Режим для сцен, где небо — фон, а не предмет разговора: схема уровня,
// студийная подложка, стилизованная игра. Проверяется именно то, чем он
// отличается от процедурного: один цвет вместо градиента, никакого времени
// суток — и при этом ЕСТЬ окружающий свет (в отличие от выключенного неба,
// которое означает темноту).
TEST(Sky_solid_colour_is_flat_and_has_no_time_of_day) {
    LightingEnvironment env = EnvWithSunAt(45.0f);
    env.Skybox.Enabled = true;
    env.Skybox.Kind = SkyboxSettings::Source::Solid;
    env.Skybox.TopColor = glm::vec3(0.2f, 0.6f, 0.3f);
    env.Skybox.HorizonColor = glm::vec3(1.0f, 0.0f, 0.0f); // цвет ЧУЖОГО режима
    env.Skybox.DayNight = true;
    sage::render::ApplySky(env);

    // Верх и горизонт совпадают — это и есть «одним цветом».
    CHECK_NEAR(env.SkyTop().g, 0.6f, 1e-4f);
    CHECK_NEAR(env.SkyHorizon().g, 0.6f, 1e-4f);
    CHECK_NEAR(env.SkyHorizon().r, 0.2f, 1e-4f);

    // Солнце под горизонтом ничего не меняет: у заливки нет ни ночи, ни заката.
    LightingEnvironment night = env;
    night.Sun.Direction = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f)); // светит вверх = село
    sage::render::ApplySky(night);
    CHECK_NEAR(night.SkyTop().g, 0.6f, 1e-4f);
    CHECK_NEAR(night.DayFactor, 1.0f, 1e-4f);

    // Окружающий свет «от неба» берёт этот же цвет — ради этого режим и нужен
    // там, где выключенное небо не годится.
    env.AmbientMode = LightingEnvironment::AmbientSource::FromSky;
    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    CHECK_TRUE(sky.g > sky.r);
    CHECK_TRUE(sky.g > 0.1f);
}

// Светил на заливке быть не может: солнце и звёзды сразу превращают её обратно
// в небо, ради отсутствия которого режим и выбирают.
TEST(Sky_solid_colour_draws_no_sun_or_stars) {
    LightingEnvironment env = EnvWithSunAt(30.0f);
    env.Skybox.Enabled = true;
    env.Skybox.Celestials = true;
    env.Skybox.Kind = SkyboxSettings::Source::Procedural;
    CHECK_TRUE(CelestialsFromEnvironment(env).Enabled);

    env.Skybox.Kind = SkyboxSettings::Source::Solid;
    CHECK_FALSE(CelestialsFromEnvironment(env).Enabled);
}

// Режим переживает запись и чтение сцены: номер лежит в .sage, и добавление
// нового значения не имеет права переназначить небо в старых файлах.
TEST(Sky_solid_colour_survives_save_and_load) {
    auto scene = std::make_unique<Scene>("SolidSky");
    scene->Lighting.Skybox.Enabled = true;
    scene->Lighting.Skybox.Kind = SkyboxSettings::Source::Solid;
    scene->Lighting.Skybox.TopColor = glm::vec3(0.1f, 0.2f, 0.3f);

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    CHECK_TRUE(back->Lighting.Skybox.Kind == SkyboxSettings::Source::Solid);
    CHECK_NEAR(back->Lighting.Skybox.TopColor.b, 0.3f, 1e-4f);
}

// --- 8. Режим неба переживает запись и чтение --------------------------------
TEST(Sky_mode_and_faces_survive_save_and_load) {
    auto scene = std::make_unique<Scene>("SkyTest");
    scene->Lighting.Skybox.Enabled = true;
    scene->Lighting.Skybox.Kind = SkyboxSettings::Source::Faces;
    for (int i = 0; i < 6; ++i)
        scene->Lighting.Skybox.FacePaths[i] = "assets/sky/face" + std::to_string(i) + ".png";
    scene->Lighting.Skybox.CubemapDir = "assets/other";   // не должен потеряться
    scene->Lighting.Skybox.DayNight = false;
    scene->Lighting.Skybox.MoonlightIntensity = 0.33f;
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*scene));
    CHECK_TRUE(back != nullptr);
    const SkyboxSettings& sky = back->Lighting.Skybox;
    CHECK_TRUE(sky.Kind == SkyboxSettings::Source::Faces);
    CHECK_TRUE(sky.HasFaces());
    CHECK_EQ(sky.FacePaths[3], std::string("assets/sky/face3.png"));
    // Путь прежнего режима сохраняется: вернуться к нему надо уметь без
    // повторного выбора папки.
    CHECK_EQ(sky.CubemapDir, std::string("assets/other"));
    CHECK_TRUE(!sky.DayNight);
    CHECK_NEAR(sky.MoonlightIntensity, 0.33f, 1e-4f);
    CHECK_TRUE(back->Lighting.AmbientMode == LightingEnvironment::AmbientSource::Custom);
}

// --- 9. Старые сцены открываются с прежним небом -----------------------------
//
// До появления режимов «есть путь к каталогу» и означало «небо текстурное».
// Сцена, сохранённая тогда, обязана открыться с тем же небом, а не с внезапным
// градиентом на месте набора.
TEST(Sky_legacy_scene_keeps_its_textured_sky) {
    const char* legacy = R"({
      "sage_scene_version": 6,
      "name": "Legacy",
      "objects": [],
      "lighting": { "skybox": { "enabled": true, "cubemapDir": "assets/sky/day" } }
    })";
    std::unique_ptr<Scene> scene = SceneSerializer::LoadFromString(legacy);
    CHECK_TRUE(scene != nullptr);
    CHECK_TRUE(scene->Lighting.Skybox.Kind == SkyboxSettings::Source::Cubemap);
    CHECK_TRUE(scene->Lighting.Skybox.HasCubemap());
}

// --- 10. Время суток задаётся ПОВОРОТОМ ОБЪЕКТА-СОЛНЦА ----------------------
//
// Это и есть весь способ задать время: часов у движка нет намеренно. Значит,
// путь «повернул объект → стемнело» обязан работать целиком, а не только на
// половине из поля LightingEnvironment::Sun, которое кадр не трогает вовсе.
// Всё, что читает время суток мимо сбора освещения кадра, получает полдень —
// на этом уже попалась панель «Среда».
TEST(Sky_time_of_day_comes_from_the_sun_object) {
    auto scene = std::make_unique<Scene>("SunObject");
    scene->Lighting.Skybox.Enabled = true;
    scene->Lighting.Skybox.Kind = SkyboxSettings::Source::Procedural;
    scene->Lighting.Skybox.DayNight = true;

    GameObject sun = scene->CreateObject("Sun");
    LightComponent& lc = sun.Registry()->emplace<LightComponent>(sun.Entity());
    lc.Kind = LightComponent::Type::Directional;
    lc.Intensity = 1.0f;

    // Солнце высоко: светит вниз.
    sun.GetTransform().Rotation =
        sage::ecs::EulerFromForward(glm::normalize(glm::vec3(0.2f, -1.0f, -0.3f)));
    LightingEnvironment day = sage::ecs::CollectLighting(*scene);
    CHECK_TRUE(day.DayFactor > 0.99f);

    // Тот же объект, повёрнутый: солнце светит ВВЕРХ, значит оно под горизонтом.
    sun.GetTransform().Rotation =
        sage::ecs::EulerFromForward(glm::normalize(glm::vec3(0.2f, 1.0f, -0.3f)));
    LightingEnvironment night = sage::ecs::CollectLighting(*scene);
    CHECK_TRUE(night.DayFactor < 0.01f);
    CHECK_TRUE(Luma(night.SkyTopNow) < Luma(day.SkyTopNow) * 0.3f);

    // Поле окружения при этом так и осталось нетронутым — именно поэтому читать
    // время суток надо из собранного кадра.
    CHECK_TRUE(scene->Lighting.DayFactor > 0.99f);
}

// --- 11. Окружение, собранное КОДОМ, остаётся своим -------------------------
//
// Превью ассета, тест, инструмент собирают LightingEnvironment руками и сбор
// освещения кадра не зовут. Поля «состояние кадра» у такого окружения содержат
// значения по умолчанию — то есть ЧУЖОЕ небо, — и потребитель, читающий их
// напрямую, рисовал бы студийную сцену под небом, которого ему не задавали:
// молча, без единой ошибки. Ровно так и вышло с отражениями в превью ассета.
TEST(Sky_hand_built_environment_keeps_its_own_colours) {
    LightingEnvironment studio;                 // ApplySky НЕ зовём — как в превью
    studio.Skybox.Enabled = true;
    studio.Skybox.TopColor = glm::vec3(0.20f, 0.34f, 0.62f);
    studio.Skybox.HorizonColor = glm::vec3(0.78f, 0.80f, 0.86f);

    CHECK_TRUE(!studio.SkyResolved);
    CHECK_NEAR(studio.SkyTop().r, 0.20f, 1e-5f);
    CHECK_NEAR(studio.SkyHorizon().b, 0.86f, 1e-5f);

    // И засветка объектов считается из ЭТОГО неба, а не из умолчаний.
    glm::vec3 sky, ground;
    studio.ResolveAmbient(sky, ground);
    CHECK_NEAR(ground.b, 0.86f * 0.5f, 1e-5f);

    // После расчёта кадра — уже разрешённые цвета.
    sage::render::ApplySky(studio);
    CHECK_TRUE(studio.SkyResolved);
}

// --- Небо из ОДНОГО файла ---------------------------------------------------
//
// Жалоба была прямая: «почему мы должны папку загружать, а не конкретную
// текстуру, странно». И это справедливо: скачанный набор — это два десятка
// готовых небес, каждое ОДНОЙ картинкой (крест 4:3, полоса, панорама), а
// редактор просил каталог с шестью файлами по нашим именам px/nx/py/ny/pz/nz.
// Ни одно небо из набора выбрать было нельзя, не нарезав его руками.
//
// Раскладка определяется по соотношению сторон — это и проверяется: числа, а не
// картинка, потому что ошибка здесь означает грани, разложенные не по тем
// сторонам куба, и увидеть её можно только глазами на готовой сцене.
TEST(Sky_single_image_layout_is_detected_by_aspect) {
    using L = Skybox::Layout;
    // Ровно то, что лежит у человека: 2048x1536 при гранях 512.
    CHECK_TRUE(Skybox::DetectLayout(2048, 1536) == L::HorizontalCross);
    CHECK_TRUE(Skybox::DetectLayout(1536, 2048) == L::VerticalCross);
    CHECK_TRUE(Skybox::DetectLayout(3072, 512) == L::Row);
    CHECK_TRUE(Skybox::DetectLayout(512, 3072) == L::Column);
    CHECK_TRUE(Skybox::DetectLayout(4096, 2048) == L::Equirectangular);
    // Квадрат — ни то, ни другое: молча принять его значило бы нарезать небо
    // наугад и показать мешанину вместо ошибки.
    CHECK_TRUE(Skybox::DetectLayout(1024, 1024) == L::Auto);
    CHECK_TRUE(Skybox::DetectLayout(0, 0) == L::Auto);

    // Допуск на лишний пиксель по краю: наборы этим грешат, а отбраковывать
    // годную картинку из-за одного пикселя — значит возвращать человека к
    // ручной нарезке.
    CHECK_TRUE(Skybox::DetectLayout(2049, 1536) == L::HorizontalCross);
}

} // namespace
