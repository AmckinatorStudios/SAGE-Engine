#include "sage/render/SkyPresets.h"

#include "sage/scene/Light.h"

namespace sage::render {

const char* SkyPresetId(SkyPreset p) {
    switch (p) {
        case SkyPreset::Overcast:  return "overcast";
        default:                   return "default";
    }
}

const char* SkyPresetLabel(SkyPreset p) {
    switch (p) {
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
    // случайные остатки прошлого (облака от «пасмурно» на ясном небе).
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
        case SkyPreset::Overcast:
            sky.TopColor = {0.42f, 0.45f, 0.50f};
            sky.HorizonColor = {0.62f, 0.64f, 0.67f};
            sky.GradientExponent = 1.2f;
            sky.Celestials = false;
            sky.Clouds = true;
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
