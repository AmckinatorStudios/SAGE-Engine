#include "sage/render/SkyPresets.h"

#include "sage/scene/Light.h"

namespace sage::render {

const char* SkyPresetId(SkyPreset p) {
    switch (p) {
        case SkyPreset::Blocky:    return "blocky";
        case SkyPreset::Overcast:  return "overcast";
        default:                   return "default";
    }
}

const char* SkyPresetLabel(SkyPreset p) {
    switch (p) {
        case SkyPreset::Blocky:    return "Blocky";
        case SkyPreset::Overcast:  return "Overcast";
        default:                   return "Default";
    }
}

bool ParseSkyPreset(const std::string& id, SkyPreset& out) {
    for (int i = 0; i < (int)SkyPreset::Count; ++i) {
        if (id == SkyPresetId((SkyPreset)i)) {
            out = (SkyPreset)i;
            return true;
        }
    }
    return false;
}

void ApplySkyPreset(SkyboxSettings& sky, SkyPreset p) {
    // Сначала — вид по умолчанию целиком: пресет не должен наследовать
    // случайные остатки прошлого (облака от «пасмурно» на блочном небе).
    // Потом возвращаем то, что к виду не относится.
    const SkyboxSettings keep = sky;
    sky = SkyboxSettings{};
    sky.Enabled = true;
    sky.Kind = SkyboxSettings::Source::Procedural;
    sky.DayNight = keep.DayNight;
    sky.Celestials = keep.Celestials;
    sky.CubemapDir = keep.CubemapDir;
    for (int i = 0; i < 6; ++i) sky.FacePaths[i] = keep.FacePaths[i];
    sky.ImagePath = keep.ImagePath;
    sky.ImageLayout = keep.ImageLayout;
    sky.Intensity = keep.Intensity;
    sky.RotationDeg = keep.RotationDeg;

    switch (p) {
        case SkyPreset::Blocky:
            // Цвета — линейные значения sRGB-цветов: небо #78A7FF, дымка у
            // горизонта #C0D8FF, «пустота» под горизонтом — небо, умноженное
            // на (0.2, 0.2, 0.6).
            sky.TopColor = {0.188f, 0.386f, 1.0f};
            sky.HorizonColor = {0.527f, 0.686f, 1.0f};
            sky.NightTopColor = {0.0f, 0.0f, 0.004f};
            sky.NightHorizonColor = {0.004f, 0.006f, 0.018f};
            sky.DuskColor = {0.55f, 0.18f, 0.05f};
            // Ровный переход к полосе у горизонта и резкий край «пустоты».
            sky.GradientExponent = 0.75f;
            sky.HorizonSoftness = 0.0f;
            sky.HorizonOffset = -0.02f;
            sky.Ground = true;
            sky.GroundColor = {0.030f, 0.060f, 0.60f};
            sky.NightGroundColor = {0.0f, 0.0f, 0.002f};
            sky.GroundBlend = 0.015f;
            // Квадратные солнце и луна без ореола, звёзды — крупные квадраты.
            sky.Celestials = true;
            sky.SunShape = SkyboxSettings::DiscShape::Square;
            sky.MoonShape = SkyboxSettings::DiscShape::Square;
            sky.SunColor = {1.0f, 1.0f, 0.78f};
            sky.SunSize = 0.085f;
            sky.SunBrightness = 2.5f;
            sky.SunGlow = 0.0f;
            sky.MoonColor = {0.85f, 0.88f, 0.95f};
            sky.MoonSize = 0.06f;
            sky.MoonPhase = false;
            sky.PixelArt = true;
            sky.StarDensity = 2.0f;
            sky.StarSize = 1.6f;
            // Плоские блочные облака: клетка 12 м, слой на 120 м выше глаз,
            // медленный дрейф по X.
            sky.Clouds = true;
            sky.CloudKind = SkyboxSettings::CloudStyle::Blocky;
            sky.CloudColor = {1.0f, 1.0f, 1.0f};
            sky.NightCloudColor = {0.025f, 0.025f, 0.04f};
            sky.CloudHeight = 120.0f;
            sky.CloudScale = 12.0f;
            sky.CloudCoverage = 0.35f;
            sky.CloudOpacity = 0.80f;
            sky.CloudWind = {0.6f, 0.0f};
            sky.CloudFade = 900.0f;
            break;
        case SkyPreset::Overcast:
            sky.TopColor = {0.42f, 0.45f, 0.50f};
            sky.HorizonColor = {0.62f, 0.64f, 0.67f};
            sky.GradientExponent = 1.2f;
            sky.Celestials = false;
            sky.Clouds = true;
            sky.CloudKind = SkyboxSettings::CloudStyle::Soft;
            sky.CloudColor = {0.70f, 0.72f, 0.75f};
            sky.CloudHeight = 400.0f;
            sky.CloudScale = 40.0f;
            sky.CloudCoverage = 0.85f;
            sky.CloudOpacity = 0.9f;
            sky.CloudWind = {3.0f, 1.0f};
            sky.CloudFade = 4000.0f;
            break;
        default:
            break;
    }
}

} // namespace sage::render
