#include "sage/core/Config.h"

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

using json = nlohmann::json;

namespace sage {

namespace {
const char* ModeToStr(WindowMode m) {
    switch (m) {
        case WindowMode::Borderless: return "borderless";
        case WindowMode::Fullscreen: return "fullscreen";
        default: return "windowed";
    }
}
WindowMode ModeFromStr(const std::string& s) {
    if (s == "borderless") return WindowMode::Borderless;
    if (s == "fullscreen") return WindowMode::Fullscreen;
    return WindowMode::Windowed;
}
const char* AspectToStr(AspectMode a) {
    switch (a) {
        case AspectMode::R16x9:  return "16:9";
        case AspectMode::R16x10: return "16:10";
        case AspectMode::R4x3:   return "4:3";
        case AspectMode::R21x9:  return "21:9";
        default: return "free";
    }
}
AspectMode AspectFromStr(const std::string& s) {
    if (s == "16:9")  return AspectMode::R16x9;
    if (s == "16:10") return AspectMode::R16x10;
    if (s == "4:3")   return AspectMode::R4x3;
    if (s == "21:9")  return AspectMode::R21x9;
    return AspectMode::Free;
}

// Булев env: "0"/"false"/"off"/"no" -> false, иначе (существует) -> true.
bool EnvBool(const char* v, bool fallback) {
    if (!v) return fallback;
    std::string s(v);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "0" || s == "false" || s == "off" || s == "no") return false;
    return true;
}
} // namespace

float EngineConfig::AspectRatio() const {
    switch (Aspect) {
        case AspectMode::R16x9:  return 16.0f / 9.0f;
        case AspectMode::R16x10: return 16.0f / 10.0f;
        case AspectMode::R4x3:   return 4.0f / 3.0f;
        case AspectMode::R21x9:  return 21.0f / 9.0f;
        default: return 0.0f; // Free
    }
}

void EngineConfig::ScaledResolution(int windowW, int windowH, int& outW, int& outH) const {
    float s = RenderScale;
    if (s < 0.25f) s = 0.25f;
    if (s > 2.0f) s = 2.0f;
    outW = (int)(windowW * s);
    outH = (int)(windowH * s);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
}

void EngineConfig::LetterboxViewport(int winW, int winH, int& x, int& y, int& w, int& h) const {
    float ratio = AspectRatio();
    if (ratio <= 0.0f || winW <= 0 || winH <= 0) {
        x = 0; y = 0; w = winW; h = winH; // Free — весь экран
        return;
    }
    float winRatio = (float)winW / (float)winH;
    if (winRatio > ratio) {
        // Окно шире нужного — вертикальные полосы (pillarbox).
        h = winH;
        w = (int)(winH * ratio);
        x = (winW - w) / 2;
        y = 0;
    } else {
        // Окно выше нужного — горизонтальные полосы (letterbox).
        w = winW;
        h = (int)(winW / ratio);
        x = 0;
        y = (winH - h) / 2;
    }
}

void EngineConfig::ApplyPreset(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Low:
            // Слабый/старый ПК: убираем оба тяжёлых прохода (тени, HDR-пост со
            // всеми эффектами) и рендерим в 75% разрешения. Туман и скайбокс
            // остаются — они почти бесплатны, а картинку держат.
            Shadows = false;
            ShadowResolution = 512;
            ShadowCascades = 1;
            LocalShadows = false;
            PostProcessing = false;
            Bloom = false;
            AmbientOcclusion = false;
            Msaa = 0;
            RenderScale = 0.75f;
            break;
        case QualityPreset::Medium:
            Shadows = true;
            ShadowResolution = 1024;
            ShadowCascades = 1;
            // Тени ламп на средних настройках выключены: каждый точечный свет
            // стоит шести проходов геометрии, и это ровно та цена, ради которой
            // выбирают «средне».
            LocalShadows = false;
            LocalShadowResolution = 1024;
            PostProcessing = true;  // тон-маппинг/экспозиция — дёшево и заметно
            Bloom = false;
            AmbientOcclusion = false;
            Msaa = 0;
            RenderScale = 1.0f;
            break;
        case QualityPreset::High:
            // Значения по умолчанию движка — всё включено.
            Shadows = true;
            ShadowResolution = 2048;
            ShadowCascades = 3;
            LocalShadows = true;
            LocalShadowResolution = 2048;
            PostProcessing = true;
            Bloom = true;
            AmbientOcclusion = true;
            Msaa = 0;
            RenderScale = 1.0f;
            break;
        case QualityPreset::Ultra:
            Shadows = true;
            ShadowResolution = 4096;
            // Три, а не четыре: четвёртый каскад упирался в текстурный юнит,
            // который нужен карте самосвечения (см. render/ShadowMap.h). На
            // «ультре» разрешение вдвое выше, и подробность берётся оттуда.
            ShadowCascades = 3;
            LocalShadows = true;
            LocalShadowResolution = 4096;
            PostProcessing = true;
            Bloom = true;
            AmbientOcclusion = true;
            Msaa = 4;
            RenderScale = 1.0f;
            break;
    }
}

bool EngineConfig::LoadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        LOG_WARN("Config") << "Не удалось разобрать " << path << ": " << e.what();
        return false;
    }

    auto win = j.value("window", json::object());
    Width     = win.value("width", Width);
    Height    = win.value("height", Height);
    Title     = win.value("title", Title);
    Mode      = ModeFromStr(win.value("mode", std::string(ModeToStr(Mode))));
    Resizable = win.value("resizable", Resizable);
    VSync     = win.value("vsync", VSync);
    FrameCap  = win.value("frameCap", FrameCap);
    Msaa      = win.value("msaa", Msaa);

    auto disp = j.value("display", json::object());
    Aspect      = AspectFromStr(disp.value("aspect", std::string(AspectToStr(Aspect))));
    RenderScale = disp.value("renderScale", RenderScale);

    auto gfx = j.value("graphics", json::object());
    Backend          = gfx.value("backend", Backend);
    Shadows          = gfx.value("shadows", Shadows);
    ShadowResolution = gfx.value("shadowResolution", ShadowResolution);
    ShadowCascades = std::clamp(gfx.value("shadowCascades", ShadowCascades), 1, 3);
    LocalShadows = gfx.value("localShadows", LocalShadows);
    LocalShadowResolution = gfx.value("localShadowResolution", LocalShadowResolution);
    OcclusionCulling = gfx.value("occlusionCulling", OcclusionCulling);
    Reflections      = gfx.value("reflections", Reflections);
    PlanarReflections= gfx.value("planarReflections", PlanarReflections);
    ShadowDistance = gfx.value("shadowDistance", ShadowDistance);
    PostProcessing   = gfx.value("postProcessing", PostProcessing);
    Fog              = gfx.value("fog", Fog);
    Skybox           = gfx.value("skybox", Skybox);

    auto ui = j.value("ui", json::object());
    UiFont = ui.value("font", UiFont);
    UiFontPixelHeight = ui.value("fontPixelHeight", UiFontPixelHeight);
    UiFontPixelArt = ui.value("fontPixelArt", UiFontPixelArt);

    auto pp = j.value("postProcess", json::object());
    Exposure   = pp.value("exposure", Exposure);
    Gamma      = pp.value("gamma", Gamma);
    Saturation = pp.value("saturation", Saturation);
    Contrast   = pp.value("contrast", Contrast);
    Vignette   = pp.value("vignette", Vignette);
    Bloom            = pp.value("bloom", Bloom);
    BloomThreshold   = pp.value("bloomThreshold", BloomThreshold);
    BloomIntensity   = pp.value("bloomIntensity", BloomIntensity);
    Volumetrics        = pp.value("volumetrics", Volumetrics);
    VolumetricShafts   = pp.value("volumetricShafts", VolumetricShafts);
    VolumetricClouds   = pp.value("volumetricClouds", VolumetricClouds);
    VolumetricDensity  = pp.value("volumetricDensity", VolumetricDensity);
    VolumetricIntensity= pp.value("volumetricIntensity", VolumetricIntensity);
    VolumetricSteps    = pp.value("volumetricSteps", VolumetricSteps);
    DebugView          = pp.value("debugView", DebugView);
    VolumetricMaxDistance = pp.value("volumetricMaxDistance", VolumetricMaxDistance);
    VolumetricHeightFalloff = pp.value("volumetricHeightFalloff", VolumetricHeightFalloff);
    CloudSteps         = pp.value("cloudSteps", CloudSteps);
    CloudCoverage      = pp.value("cloudCoverage", CloudCoverage);
    CloudBottom        = pp.value("cloudBottom", CloudBottom);
    CloudTop           = pp.value("cloudTop", CloudTop);
    VolumetricScale    = pp.value("volumetricScale", VolumetricScale);
    AmbientOcclusion = pp.value("ao", AmbientOcclusion);
    AOStrength       = pp.value("aoStrength", AOStrength);
    AORadius         = pp.value("aoRadius", AORadius);
    DepthOfField     = pp.value("depthOfField", DepthOfField);
    FocusDistance    = pp.value("focusDistance", FocusDistance);
    Aperture         = pp.value("aperture", Aperture);
    DofMaxRadius     = pp.value("dofMaxRadius", DofMaxRadius);
    MotionBlur       = pp.value("motionBlur", MotionBlur);
    MotionBlurAmount = pp.value("motionBlurAmount", MotionBlurAmount);
    MotionBlurSamples = pp.value("motionBlurSamples", MotionBlurSamples);
    ChromaticAberration = pp.value("chromaticAberration", ChromaticAberration);
    Fxaa = pp.value("fxaa", Fxaa);
    FxaaContrastThreshold = pp.value("fxaaContrastThreshold", FxaaContrastThreshold);

    auto sys = j.value("system", json::object());
    WorkerThreads       = sys.value("workerThreads", WorkerThreads);
    MultithreadedRender = sys.value("multithreadedRender", MultithreadedRender);

    LOG_INFO("Config") << "Загружены настройки: " << path;
    return true;
}

std::string EngineConfig::ToJsonString() const {
    json j;
    j["window"] = {
        {"width", Width}, {"height", Height}, {"title", Title},
        {"mode", ModeToStr(Mode)}, {"resizable", Resizable},
        {"vsync", VSync}, {"frameCap", FrameCap}, {"msaa", Msaa},
    };
    j["display"] = {
        {"aspect", AspectToStr(Aspect)}, {"renderScale", RenderScale},
    };
    j["graphics"] = {
        {"backend", Backend},
        {"shadows", Shadows}, {"shadowResolution", ShadowResolution},
        {"shadowCascades", ShadowCascades}, {"shadowDistance", ShadowDistance},
        {"localShadows", LocalShadows}, {"localShadowResolution", LocalShadowResolution},
        {"occlusionCulling", OcclusionCulling},
        {"reflections", Reflections}, {"planarReflections", PlanarReflections},
        {"postProcessing", PostProcessing}, {"fog", Fog}, {"skybox", Skybox},
    };
    j["ui"] = {
        {"font", UiFont}, {"fontPixelHeight", UiFontPixelHeight},
        {"fontPixelArt", UiFontPixelArt},
    };
    j["postProcess"] = {
        {"exposure", Exposure}, {"gamma", Gamma}, {"saturation", Saturation},
        {"contrast", Contrast}, {"vignette", Vignette},
        {"bloom", Bloom}, {"bloomThreshold", BloomThreshold}, {"bloomIntensity", BloomIntensity},
        {"volumetrics", Volumetrics}, {"volumetricShafts", VolumetricShafts},
        {"volumetricClouds", VolumetricClouds}, {"volumetricDensity", VolumetricDensity},
        {"volumetricIntensity", VolumetricIntensity}, {"volumetricSteps", VolumetricSteps},
        {"volumetricMaxDistance", VolumetricMaxDistance},
        {"volumetricHeightFalloff", VolumetricHeightFalloff},
        {"cloudSteps", CloudSteps}, {"cloudCoverage", CloudCoverage},
        {"cloudBottom", CloudBottom}, {"cloudTop", CloudTop},
        {"volumetricScale", VolumetricScale},
        {"ao", AmbientOcclusion}, {"aoStrength", AOStrength}, {"aoRadius", AORadius},
        {"depthOfField", DepthOfField}, {"focusDistance", FocusDistance},
        {"aperture", Aperture}, {"dofMaxRadius", DofMaxRadius},
        {"motionBlur", MotionBlur}, {"motionBlurAmount", MotionBlurAmount},
        {"motionBlurSamples", MotionBlurSamples},
        {"chromaticAberration", ChromaticAberration},
        {"fxaa", Fxaa},
        {"fxaaContrastThreshold", FxaaContrastThreshold},
    };
    j["system"] = {
        {"workerThreads", WorkerThreads}, {"multithreadedRender", MultithreadedRender},
    };
    return j.dump(2);
}

bool EngineConfig::SaveFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f) {
        LOG_ERROR("Config") << "Не удалось записать настройки: " << path;
        return false;
    }
    f << ToJsonString() << "\n";
    LOG_INFO("Config") << "Сохранены настройки: " << path;
    return true;
}

void EngineConfig::ApplyEnvOverrides() {
    // Пресет качества ПЕРВЫМ — он выставляет группу полей разом, а точечные
    // SAGE_*-переменные ниже могут поправить любое поле поверх пресета.
    if (const char* v = std::getenv("SAGE_QUALITY")) {
        std::string s(v);
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        if (s == "low")         ApplyPreset(QualityPreset::Low);
        else if (s == "medium") ApplyPreset(QualityPreset::Medium);
        else if (s == "high")   ApplyPreset(QualityPreset::High);
        else if (s == "ultra")  ApplyPreset(QualityPreset::Ultra);
        else LOG_WARN("Config") << "SAGE_QUALITY: неизвестный пресет '" << v
                                << "' (ожидается low/medium/high/ultra) — игнорирую";
    }

    // Размер/окно
    if (const char* v = std::getenv("SAGE_WINDOW_WIDTH")) Width = std::atoi(v);
    if (const char* v = std::getenv("SAGE_WINDOW_HEIGHT")) Height = std::atoi(v);
    if (const char* v = std::getenv("SAGE_WINDOW_MODE")) Mode = ModeFromStr(v);
    if (const char* v = std::getenv("SAGE_VSYNC")) VSync = EnvBool(v, VSync);
    if (const char* v = std::getenv("SAGE_FRAME_CAP")) FrameCap = std::atoi(v);
    if (const char* v = std::getenv("SAGE_MSAA")) Msaa = std::atoi(v);
    // Графический бэкенд из окружения — то, чем переключают его в тестах и в
    // отчёте об ошибке у игрока («запустите с SAGE_BACKEND=opengl»), не трогая
    // файл настроек.
    if (const char* v = std::getenv("SAGE_BACKEND")) Backend = v;

    // Дисплей
    if (const char* v = std::getenv("SAGE_ASPECT")) Aspect = AspectFromStr(v);
    if (const char* v = std::getenv("SAGE_RENDER_SCALE")) RenderScale = (float)std::atof(v);
    // Объём — самый дорогой проход кадра, и на слабой машине (или в headless-
    // прогоне на программном растеризаторе) его надо уметь придавить снаружи,
    // не трогая ни файл настроек, ни скрипты игры.
    if (const char* v = std::getenv("SAGE_VOLUMETRICS")) Volumetrics = EnvBool(v, Volumetrics);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_STEPS")) VolumetricSteps = std::atoi(v);
    if (const char* v = std::getenv("SAGE_CLOUD_STEPS")) CloudSteps = std::atoi(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_SCALE")) VolumetricScale = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_DENSITY")) VolumetricDensity = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_INTENSITY")) VolumetricIntensity = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_CLOUD_COVERAGE")) CloudCoverage = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_DEBUG")) VolumetricDebug = EnvBool(v, VolumetricDebug);
    if (const char* v = std::getenv("SAGE_DEBUG_VIEW")) DebugView = v;

    // Графика. SAGE_NO_SHADOWS / SAGE_NO_POST оставлены для обратной
    // совместимости (их наличие ВЫКЛЮЧАЕТ проход); плюс явные SAGE_SHADOWS/…
    if (std::getenv("SAGE_NO_SHADOWS")) Shadows = false;
    if (std::getenv("SAGE_NO_POST")) PostProcessing = false;
    if (const char* v = std::getenv("SAGE_SHADOWS")) Shadows = EnvBool(v, Shadows);
    if (const char* v = std::getenv("SAGE_SHADOW_RES")) ShadowResolution = std::atoi(v);
    if (const char* v = std::getenv("SAGE_OCCLUSION")) OcclusionCulling = EnvBool(v, OcclusionCulling);
    if (const char* v = std::getenv("SAGE_SHADOW_CASCADES"))
        ShadowCascades = std::clamp(std::atoi(v), 1, 3);
    if (const char* v = std::getenv("SAGE_LOCAL_SHADOWS")) LocalShadows = EnvBool(v, LocalShadows);
    if (const char* v = std::getenv("SAGE_SHADOW_DISTANCE")) ShadowDistance = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_POST")) PostProcessing = EnvBool(v, PostProcessing);
    // Отдельные эффекты — переменными окружения: проверять их поодиночке
    // (и особенно сравнивать «с ним» и «без него» на одном кадре) иначе
    // приходится правкой файла настроек между запусками.
    if (const char* v = std::getenv("SAGE_BLOOM")) Bloom = EnvBool(v, Bloom);
    if (const char* v = std::getenv("SAGE_EXPOSURE")) Exposure = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_SATURATION")) Saturation = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_CONTRAST")) Contrast = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_SSAO")) AmbientOcclusion = EnvBool(v, AmbientOcclusion);
    if (const char* v = std::getenv("SAGE_FXAA")) Fxaa = EnvBool(v, Fxaa);
    if (const char* v = std::getenv("SAGE_DOF")) DepthOfField = EnvBool(v, DepthOfField);
    if (const char* v = std::getenv("SAGE_MOTION_BLUR")) MotionBlur = EnvBool(v, MotionBlur);
    if (const char* v = std::getenv("SAGE_REFLECTIONS")) Reflections = EnvBool(v, Reflections);
    if (const char* v = std::getenv("SAGE_PLANAR_REFLECTIONS"))
        PlanarReflections = EnvBool(v, PlanarReflections);
    if (const char* v = std::getenv("SAGE_FOG")) Fog = EnvBool(v, Fog);
    if (const char* v = std::getenv("SAGE_SKYBOX")) Skybox = EnvBool(v, Skybox);
}

EngineConfig& EngineConfig::Get() {
    static EngineConfig g;
    return g;
}

void EngineConfig::Set(const EngineConfig& cfg) {
    Get() = cfg;
}

} // namespace sage
