#include "sage/render/SkyModel.h"

#include <algorithm>
#include <cmath>

namespace sage::render {
namespace {

float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

SkyState EvaluateSky(const LightingEnvironment& env) {
    const SkyboxSettings& sky = env.Skybox;

    SkyState out;
    out.Light = env.Sun;
    // Направление НА солнце: у направленного света хранится, КУДА он светит.
    const glm::vec3 dir = env.Sun.Direction;
    const glm::vec3 toSun = glm::length(dir) > 1e-6f ? -glm::normalize(dir)
                                                     : glm::vec3(0.0f, 1.0f, 0.0f);
    out.SunDirection = toSun;
    out.Top = sky.TopColor;
    out.Horizon = sky.HorizonColor;

    // ОДНИМ ЦВЕТОМ — значит ровно одним: верх и горизонт совпадают, времени
    // суток нет. Без этой строки «одноцветное» небо продолжало бы уходить в
    // градиент к HorizonColor, то есть не было бы одноцветным.
    if (sky.Kind == SkyboxSettings::Source::Solid) {
        out.Horizon = sky.TopColor;
        return out;
    }

    // НЕБА НЕТ — НЕТ И ВРЕМЕНИ СУТОК. Сцена без неба (интерьер, схема, превью
    // ассета) освещена так, как её собрали: гасить в ней направленный свет за
    // то, что он смотрит вверх, — значит менять смысл сцены, которая про небо
    // ничего не говорила.
    //
    // Смена суток выключена или небо текстурное — то же самое, вечный день. У
    // кубической карты своё время суток запечено в самих гранях, и менять её
    // цвета по высоте солнца значило бы красить чужую фотографию.
    if (!sky.Enabled || !sky.DayNight || sky.Kind != SkyboxSettings::Source::Procedural)
        return out;

    // ВЫСОТА СОЛНЦА — синус угла над горизонтом, то есть просто вертикальная
    // составляющая направления на него. Ноль — солнце на горизонте.
    const float elevation = toSun.y;

    // День наступает не мгновенно: нижняя граница ниже горизонта (небо светлеет
    // ещё до восхода — это сумерки), верхняя чуть выше (солнце уже встало, но
    // светит вскользь). Числа взяты из гражданских сумерек: -6° это примерно
    // -0.10 по синусу.
    out.DayFactor = Smoothstep(-0.10f, 0.15f, elevation);

    // Закат — узкая полоса вокруг горизонта, симметричная: рассвет выглядит так
    // же. Пик ровно на горизонте, к ±0.22 (примерно ±13°) сходит на нет.
    out.DuskFactor = 1.0f - Smoothstep(0.0f, 0.22f, std::abs(elevation));

    // --- Цвета градиента ---------------------------------------------------
    out.Top = glm::mix(sky.NightTopColor, sky.TopColor, out.DayFactor);
    out.Horizon = glm::mix(sky.NightHorizonColor, sky.HorizonColor, out.DayFactor);
    // Закатная полоса ДОБАВЛЯЕТСЯ к горизонту, а не заменяет его: солнце
    // подсвечивает воздух, а не перекрашивает небо. Множитель 0.8 подобран так,
    // чтобы полоса читалась, но не выжигала кадр в белое.
    out.Horizon += sky.DuskColor * (out.DuskFactor * 0.8f);
    // Зенит на закате трогаем ВТРОЕ слабее: свет идёт по касательной и до верха
    // неба почти не доходит. Без этого закат выглядит как оранжевый купол.
    out.Top += sky.DuskColor * (out.DuskFactor * 0.25f);

    // --- Кто освещает сцену ------------------------------------------------
    //
    // Солнце гаснет вместе с днём; луна светит тем сильнее, чем темнее. Оба
    // вклада считаются всегда, а слот занимает больший — см. заголовок о том,
    // почему источник один.
    const float sunPower = env.Sun.Intensity * out.DayFactor;
    const float moonPower = sky.Moon ? sky.MoonlightIntensity * (1.0f - out.DayFactor) : 0.0f;

    if (moonPower > sunPower) {
        // Луна — напротив солнца: светит она с той стороны, где солнца нет.
        // Направление ЗАПИСЫВАЕТСЯ так же, как у солнца, — куда летит свет.
        out.Light.Direction = -dir;
        out.Light.Color = sky.MoonlightColor;
        out.Light.Intensity = moonPower;
        out.MoonLit = true;
        // Тени от луны остаются включёнными, если они включены у солнца: ночная
        // тень слабая, но она есть, и без неё предметы ночью «висят».
    } else {
        out.Light.Intensity = sunPower;
    }
    return out;
}

void ApplySky(LightingEnvironment& env) {
    const SkyState state = EvaluateSky(env);
    env.SkyTopNow = state.Top;
    env.SkyHorizonNow = state.Horizon;
    env.SkySunDirection = state.SunDirection;
    env.DayFactor = state.DayFactor;
    env.SkyResolved = true;
    // ГЛАВНОЕ СВЕТИЛО занимает слот солнца: дальше по кадру всё — шейдинг,
    // тени, отражения — читает env.Sun и не обязано знать, который час.
    env.Sun = state.Light;
}

} // namespace sage::render
