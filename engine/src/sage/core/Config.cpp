#include "sage/core/Config.h"
#include "sage/core/EngineContext.h"

#include <cctype>
#include <cmath>
#include <type_traits>
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

namespace {
// ТАБЛИЦА ПРОФИЛЕЙ КАЧЕСТВА. Каждый профиль задаёт ВСЕ поля ниже — раньше
// «низкий» не трогал отражения, а «ультра» — дальность теней, и итог зависел от
// того, что было выставлено до нажатия: кнопка подсвечивалась, а картинка
// оставалась прежней. Применение и сравнение идут по одному списку полей
// (ForEachQualityField), поэтому новое поле качества достаточно дописать сюда.
struct Profile {
    bool Shadows;
    int ShadowResolution;
    int ShadowCascades;
    float ShadowDistance;
    bool LocalShadows;
    int LocalShadowResolution;
    int Msaa;
    float RenderScale;
    bool ScreenSpaceGI;
    bool Reflections;
    bool PlanarReflections;
    float VolumetricScale;
    int VolumetricSteps;
    int CloudSteps;
};

const Profile& ProfileOf(QualityPreset p) {
    // Тени ламп — только от «высокого»: каждый точечный свет стоит шести
    // проходов геометрии. Объём профиль не включает — его включает игра, — но
    // задаёт его цену (сторона буфера и шаги марша).
    static const Profile table[kQualityPresetCount] = {
        /* Minimal */ {false, 512, 1, 60.0f, false, 512, 0, 0.5f, false, false, false, 0.5f, 12, 16},
        /* Low     */ {false, 512, 1, 80.0f, false, 512, 0, 0.75f, false, true, false, 0.5f, 16, 24},
        /* Medium  */ {true, 1024, 1, 100.0f, false, 1024, 0, 1.0f, true, true, true, 0.5f, 20, 32},
        /* High    */ {true, 2048, 3, 120.0f, true, 2048, 0, 1.0f, true, true, true, 0.5f, 24, 40},
        /* Ultra   */ {true, 4096, 3, 200.0f, true, 4096, 4, 1.0f, true, true, true, 1.0f, 32, 56},
    };
    return table[std::clamp((int)p, 0, kQualityPresetCount - 1)];
}

// Пары «поле конфига — поле профиля» одним списком: и для записи, и для сравнения.
template <class Cfg, class F> void ForEachQualityField(Cfg& c, const Profile& p, F&& f) {
    f(c.Shadows, p.Shadows);
    f(c.ShadowResolution, p.ShadowResolution);
    f(c.ShadowCascades, p.ShadowCascades);
    f(c.ShadowDistance, p.ShadowDistance);
    f(c.LocalShadows, p.LocalShadows);
    f(c.LocalShadowResolution, p.LocalShadowResolution);
    f(c.Msaa, p.Msaa);
    f(c.RenderScale, p.RenderScale);
    f(c.ScreenSpaceGI, p.ScreenSpaceGI);
    f(c.Reflections, p.Reflections);
    f(c.PlanarReflections, p.PlanarReflections);
    f(c.VolumetricScale, p.VolumetricScale);
    f(c.VolumetricSteps, p.VolumetricSteps);
    f(c.CloudSteps, p.CloudSteps);
}
} // namespace

const char* QualityPresetKey(QualityPreset p) {
    switch (p) {
        case QualityPreset::Minimal: return "minimal";
        case QualityPreset::Low: return "low";
        case QualityPreset::Medium: return "medium";
        case QualityPreset::High: return "high";
        case QualityPreset::Ultra: return "ultra";
    }
    return "high";
}

bool QualityPresetFromKey(const std::string& key, QualityPreset& out) {
    std::string k = key;
    for (auto& ch : k) ch = (char)std::tolower((unsigned char)ch);
    for (int i = 0; i < kQualityPresetCount; ++i)
        if (k == QualityPresetKey((QualityPreset)i)) {
            out = (QualityPreset)i;
            return true;
        }
    return false;
}

void EngineConfig::ApplyPreset(QualityPreset preset) {
    ForEachQualityField(*this, ProfileOf(preset), [](auto& field, const auto& value) { field = value; });
    QualityProfile = QualityPresetKey(preset);
}

bool EngineConfig::MatchesPreset(QualityPreset preset) const {
    bool same = true;
    ForEachQualityField(*this, ProfileOf(preset), [&](const auto& field, const auto& value) {
        using T = std::decay_t<decltype(field)>;
        if constexpr (std::is_floating_point_v<T>) same = same && std::abs(field - value) < 1e-4f;
        else same = same && field == value;
    });
    return same;
}

bool EngineConfig::CurrentPreset(QualityPreset& out) const {
    // Сначала тот, что выбран последним: у соседних профилей наборы не
    // совпадают, но так ответ не зависит от порядка таблицы.
    QualityPreset chosen;
    if (QualityPresetFromKey(QualityProfile, chosen) && MatchesPreset(chosen)) {
        out = chosen;
        return true;
    }
    for (int i = 0; i < kQualityPresetCount; ++i)
        if (MatchesPreset((QualityPreset)i)) {
            out = (QualityPreset)i;
            return true;
        }
    return false;
}

void EngineConfig::Sanitize() {
    auto pow2 = [](int v, int def) {
        for (int s : {512, 1024, 2048, 4096})
            if (v == s) return v;
        return def;
    };
    Width = std::clamp(Width, 320, 16384);
    Height = std::clamp(Height, 240, 16384);
    Monitor = std::max(Monitor, -1);
    RefreshRate = std::clamp(RefreshRate, 0, 1000);
    FrameCap = std::clamp(FrameCap, 0, 1000);
    BackgroundFrameCap = std::clamp(BackgroundFrameCap, 0, 1000);
    Msaa = Msaa >= 8 ? 8 : Msaa >= 4 ? 4 : Msaa >= 2 ? 2 : 0;
    RenderScale = std::clamp(RenderScale, 0.25f, 2.0f);
    ShadowResolution = pow2(ShadowResolution, 2048);
    LocalShadowResolution = pow2(LocalShadowResolution, 2048);
    ShadowCascades = std::clamp(ShadowCascades, 1, 3);
    ShadowDistance = std::clamp(ShadowDistance, 10.0f, 2000.0f);
    VolumetricScale = VolumetricScale < 0.75f ? 0.5f : 1.0f;
    VolumetricSteps = std::clamp(VolumetricSteps, 4, 128);
    CloudSteps = std::clamp(CloudSteps, 4, 128);
    QualityPreset p;
    if (!QualityPresetFromKey(QualityProfile, p)) QualityProfile = "high";
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
    Monitor   = win.value("monitor", Monitor);
    RefreshRate = win.value("refreshRate", RefreshRate);
    Resizable = win.value("resizable", Resizable);
    VSync     = win.value("vsync", VSync);
    FrameCap  = win.value("frameCap", FrameCap);
    Msaa      = win.value("msaa", Msaa);
    // Ключ «cursorLock» прежних версий (способ захвата) больше не читается:
    // захват один — системный (см. Window::SetCursorCaptured). Старый файл
    // остаётся валидным, ключ просто пропускается.
    BackgroundFrameCap = std::max(0, win.value("backgroundFrameCap", BackgroundFrameCap));

    auto disp = j.value("display", json::object());
    Aspect      = AspectFromStr(disp.value("aspect", std::string(AspectToStr(Aspect))));
    RenderScale = disp.value("renderScale", RenderScale);

    auto gfx = j.value("graphics", json::object());
    Backend          = gfx.value("backend", Backend);
    QualityProfile   = gfx.value("profile", QualityProfile);
    Shadows          = gfx.value("shadows", Shadows);
    ShadowResolution = gfx.value("shadowResolution", ShadowResolution);
    ShadowCascades = std::clamp(gfx.value("shadowCascades", ShadowCascades), 1, 3);
    LocalShadows = gfx.value("localShadows", LocalShadows);
    LocalShadowResolution = gfx.value("localShadowResolution", LocalShadowResolution);
    OcclusionCulling = gfx.value("occlusionCulling", OcclusionCulling);
    Reflections      = gfx.value("reflections", Reflections);
    PlanarReflections= gfx.value("planarReflections", PlanarReflections);
    ScreenSpaceGI    = gfx.value("screenSpaceGI", ScreenSpaceGI);
    ShadowDistance = gfx.value("shadowDistance", ShadowDistance);
    Fog              = gfx.value("fog", Fog);
    Skybox           = gfx.value("skybox", Skybox);

    auto ui = j.value("ui", json::object());
    UiFont = ui.value("font", UiFont);
    UiFontPixelHeight = ui.value("fontPixelHeight", UiFontPixelHeight);
    // Фильтрация шрифта словом. Старые game.json писали галку «fontPixelArt» —
    // она и есть Nearest.
    if (ui.contains("fontFiltering") && ui["fontFiltering"].is_string())
        sage::TextureFilteringFromKey(ui["fontFiltering"].get<std::string>(), UiFontFiltering);
    else if (ui.value("fontPixelArt", false))
        UiFontFiltering = sage::TextureFiltering::Nearest;

    // РАЗДЕЛ postProcess ОСТАЛСЯ ТОЛЬКО ПОД ОБЪЁМ И БЛИК. Экспозиция, свечение,
    // цвет, тон-маппинг, виньетка, зерно, SSAO, глубина резкости, смаз, FXAA
    // уехали в компонент камеры (sage/render/PostProcessComponent.h) — старые
    // ключи в файле просто не читаются, и это правильнее, чем читать их в
    // никуда: настройка, которая ничего не делает, хуже отсутствующей.
    auto pp = j.value("postProcess", json::object());
    Volumetrics        = pp.value("volumetrics", Volumetrics);
    VolumetricShafts   = pp.value("volumetricShafts", VolumetricShafts);
    VolumetricClouds   = pp.value("volumetricClouds", VolumetricClouds);
    VolumetricDensity  = pp.value("volumetricDensity", VolumetricDensity);
    VolumetricIntensity= pp.value("volumetricIntensity", VolumetricIntensity);
    VolumetricSteps    = pp.value("volumetricSteps", VolumetricSteps);
    DebugView          = pp.value("debugView", DebugView);
    VolumetricMaxDistance = pp.value("volumetricMaxDistance", VolumetricMaxDistance);
    VolumetricHeightFalloff = pp.value("volumetricHeightFalloff", VolumetricHeightFalloff);
    VolumetricSkyScatter = pp.value("volumetricSkyScatter", VolumetricSkyScatter);
    CloudSteps         = pp.value("cloudSteps", CloudSteps);
    CloudCoverage      = pp.value("cloudCoverage", CloudCoverage);
    CloudBottom        = pp.value("cloudBottom", CloudBottom);
    CloudTop           = pp.value("cloudTop", CloudTop);
    VolumetricScale    = pp.value("volumetricScale", VolumetricScale);
    VolumetricAnisotropy = pp.value("volumetricAnisotropy", VolumetricAnisotropy);
    VolumetricTemporal = pp.value("volumetricTemporal", VolumetricTemporal);
    VolumetricTemporalBlend = pp.value("volumetricTemporalBlend", VolumetricTemporalBlend);
    CloudDensity       = pp.value("cloudDensity", CloudDensity);

    auto sys = j.value("system", json::object());
    WorkerThreads       = sys.value("workerThreads", WorkerThreads);
    MultithreadedRender = sys.value("multithreadedRender", MultithreadedRender);

    auto build = j.value("build", json::object());
    BuildProjectFile = build.value("projectFile", BuildProjectFile);

    Sanitize();
    LOG_INFO("Config") << "Загружены настройки: " << path;
    return true;
}

std::string EngineConfig::ToJsonString() const {
    json j;
    j["window"] = {
        {"width", Width}, {"height", Height}, {"title", Title},
        {"mode", ModeToStr(Mode)}, {"resizable", Resizable},
        {"monitor", Monitor}, {"refreshRate", RefreshRate},
        {"vsync", VSync}, {"frameCap", FrameCap}, {"msaa", Msaa},
        {"backgroundFrameCap", BackgroundFrameCap},
    };
    j["display"] = {
        {"aspect", AspectToStr(Aspect)}, {"renderScale", RenderScale},
    };
    j["graphics"] = {
        {"backend", Backend}, {"profile", QualityProfile},
        {"shadows", Shadows}, {"shadowResolution", ShadowResolution},
        {"shadowCascades", ShadowCascades}, {"shadowDistance", ShadowDistance},
        {"localShadows", LocalShadows}, {"localShadowResolution", LocalShadowResolution},
        {"occlusionCulling", OcclusionCulling},
        {"reflections", Reflections}, {"planarReflections", PlanarReflections},
        {"screenSpaceGI", ScreenSpaceGI},
        {"fog", Fog}, {"skybox", Skybox},
    };
    j["ui"] = {
        {"font", UiFont}, {"fontPixelHeight", UiFontPixelHeight},
        {"fontFiltering", sage::TextureFilteringKey(UiFontFiltering)},
    };
    j["postProcess"] = {
        {"volumetrics", Volumetrics}, {"volumetricShafts", VolumetricShafts},
        {"volumetricClouds", VolumetricClouds}, {"volumetricDensity", VolumetricDensity},
        {"volumetricIntensity", VolumetricIntensity}, {"volumetricSteps", VolumetricSteps},
        {"volumetricMaxDistance", VolumetricMaxDistance},
        {"volumetricHeightFalloff", VolumetricHeightFalloff},
        {"volumetricSkyScatter", VolumetricSkyScatter},
        {"cloudSteps", CloudSteps}, {"cloudCoverage", CloudCoverage},
        {"cloudBottom", CloudBottom}, {"cloudTop", CloudTop},
        {"volumetricScale", VolumetricScale},
        {"volumetricAnisotropy", VolumetricAnisotropy},
        {"volumetricTemporal", VolumetricTemporal},
        {"volumetricTemporalBlend", VolumetricTemporalBlend},
        {"cloudDensity", CloudDensity},
    };
    j["system"] = {
        {"workerThreads", WorkerThreads}, {"multithreadedRender", MultithreadedRender},
    };
    // Сборка игры — своей секцией, а не в system: это не про то, как движок
    // работает, а про то, что получится в папке сборки (см. Config.h).
    j["build"] = {
        {"projectFile", BuildProjectFile},
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
        QualityPreset p;
        if (QualityPresetFromKey(s, p)) ApplyPreset(p);
        else LOG_WARN("Config") << "SAGE_QUALITY: неизвестный пресет '" << v
                                << "' (ожидается minimal/low/medium/high/ultra) — игнорирую";
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
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_TEMPORAL"))
        VolumetricTemporal = EnvBool(v, VolumetricTemporal);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_ANISOTROPY"))
        VolumetricAnisotropy = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_CLOUD_DENSITY")) CloudDensity = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_DENSITY")) VolumetricDensity = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_INTENSITY")) VolumetricIntensity = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_CLOUD_COVERAGE")) CloudCoverage = (float)std::atof(v);
    if (const char* v = std::getenv("SAGE_VOLUMETRIC_DEBUG")) VolumetricDebug = EnvBool(v, VolumetricDebug);
    if (const char* v = std::getenv("SAGE_DEBUG_VIEW")) DebugView = v;

    // Графика. SAGE_NO_SHADOWS оставлен для обратной совместимости (его
    // наличие ВЫКЛЮЧАЕТ проход); плюс явные SAGE_SHADOWS/…
    //
    // Переменных вида SAGE_POST/SAGE_BLOOM/SAGE_EXPOSURE больше нет: эффекты
    // принадлежат камере, а не процессу, и «выключить свечение переменной
    // окружения» означало бы вторую систему поверх компонента.
    if (std::getenv("SAGE_NO_SHADOWS")) Shadows = false;
    if (const char* v = std::getenv("SAGE_SHADOWS")) Shadows = EnvBool(v, Shadows);
    if (const char* v = std::getenv("SAGE_SHADOW_RES")) ShadowResolution = std::atoi(v);
    if (const char* v = std::getenv("SAGE_OCCLUSION")) OcclusionCulling = EnvBool(v, OcclusionCulling);
    if (const char* v = std::getenv("SAGE_SHADOW_CASCADES"))
        ShadowCascades = std::clamp(std::atoi(v), 1, 3);
    if (const char* v = std::getenv("SAGE_LOCAL_SHADOWS")) LocalShadows = EnvBool(v, LocalShadows);
    if (const char* v = std::getenv("SAGE_SHADOW_DISTANCE")) ShadowDistance = (float)std::atof(v);
    // Отдельные эффекты — переменными окружения: проверять их поодиночке
    // (и особенно сравнивать «с ним» и «без него» на одном кадре) иначе
    // приходится правкой файла настроек между запусками.
    if (const char* v = std::getenv("SAGE_REFLECTIONS")) Reflections = EnvBool(v, Reflections);
    if (const char* v = std::getenv("SAGE_PLANAR_REFLECTIONS"))
        PlanarReflections = EnvBool(v, PlanarReflections);
    if (const char* v = std::getenv("SAGE_FOG")) Fog = EnvBool(v, Fog);
    if (const char* v = std::getenv("SAGE_SKYBOX")) Skybox = EnvBool(v, Skybox);

    // Сборка игры. Из окружения — потому что собирают её и из скриптов: CI
    // проверяет ОБА расклада файлов, не заводя ради этого двух sage.cfg.
    if (const char* v = std::getenv("SAGE_BUILD_PROJECT_FILE"))
        BuildProjectFile = EnvBool(v, BuildProjectFile);
    Sanitize();
}

EngineConfig& EngineConfig::Get() { return EngineContext::Current().Config(); }

void EngineConfig::Set(const EngineConfig& cfg) {
    Get() = cfg;
}

} // namespace sage
