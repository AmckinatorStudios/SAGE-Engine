// ---------------------------------------------------------------------------
// ЭФФЕКТЫ ИЗОБРАЖЕНИЯ — звенья пост-обработки поверх готового кадра (этап
// «Линза», после тон-маппинга): Invert, Negative, Grayscale, Sepia,
// Posterize, Pixelate, Blur, Glow, Distortion, Dissolve.
//
// Это ИНСТРУМЕНТЫ, а не готовые сцены «урон», «сон», «телепорт»: каждое звено
// делает одно преобразование и у каждого есть Intensity 0..1 (0 — кадр как
// был), так что их можно складывать в любом порядке и количестве и плавно
// включать из скрипта (sage.post.Set). Порядок в цепочке камеры — порядок
// наложения.
//
// Все звенья — один полноэкранный проход (RunSimplePass): дешево и без
// своих промежуточных буферов. Blur и Glow — выборка по диску (24 точки) за
// один проход: для эффекта «расфокус/свечение» этого хватает, а честное
// гауссово размытие с ядром любого размера — это Depth of Field и Bloom.
// ---------------------------------------------------------------------------
#include "sage/render/PostFX.h"
#include "sage/render/PostFXDetail.h"

#include <algorithm>

#include "sage/core/Profiler.h"

namespace sage::render {

namespace {

using namespace postfx;

// Общее начало фрагментного шейдера: кадр, интенсивность, размер пикселя.
#define SAGE_IMG_HEAD                                   \
    "#version 330 core\n"                               \
    "in vec2 vUV;\n"                                    \
    "out vec4 FragColor;\n"                             \
    "uniform sampler2D uScene;\n"                       \
    "uniform float uIntensity;\n"                       \
    "uniform vec2 uTexel;\n"                            \
    "uniform float uTime;\n"                            \
    "float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"

const char* kImgInvertFrag = SAGE_IMG_HEAD R"(
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    FragColor = vec4(mix(c, vec3(1.0) - c, uIntensity), 1.0);
}
)";

// Негатив: яркость наоборот, оттенок прежний (красное остаётся красным, но
// светлое становится тёмным) — в отличие от Invert, где меняется и цвет.
const char* kImgNegativeFrag = SAGE_IMG_HEAD R"(
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    float y = Luma(c);
    vec3 n = clamp(c + vec3(1.0 - 2.0 * y), 0.0, 1.0);
    FragColor = vec4(mix(c, n, uIntensity), 1.0);
}
)";

const char* kImgGrayscaleFrag = SAGE_IMG_HEAD R"(
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    FragColor = vec4(mix(c, vec3(Luma(c)), uIntensity), 1.0);
}
)";

const char* kImgSepiaFrag = SAGE_IMG_HEAD R"(
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    vec3 s = vec3(dot(c, vec3(0.393, 0.769, 0.189)),
                  dot(c, vec3(0.349, 0.686, 0.168)),
                  dot(c, vec3(0.272, 0.534, 0.131)));
    FragColor = vec4(mix(c, clamp(s, 0.0, 1.0), uIntensity), 1.0);
}
)";

const char* kImgPosterizeFrag = SAGE_IMG_HEAD R"(
uniform float uLevels;
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    // uLevels уровней — это uLevels-1 ступеней между 0 и 1.
    float steps = max(uLevels - 1.0, 1.0);
    vec3 p = floor(c * steps + 0.5) / steps;
    FragColor = vec4(mix(c, p, uIntensity), 1.0);
}
)";

// Пикселизация: блок растёт с интенсивностью от 1 пикселя до uSize.
const char* kImgPixelateFrag = SAGE_IMG_HEAD R"(
uniform float uSize;
void main() {
    float block = max(1.0, mix(1.0, uSize, uIntensity));
    vec2 cell = block * uTexel;
    vec2 uv = (floor(vUV / cell) + 0.5) * cell;
    FragColor = vec4(texture(uScene, uv).rgb, 1.0);
}
)";

// 24 точки по диску (золотой угол): ровное покрытие без узора сетки.
#define SAGE_IMG_DISC                                                           \
    "vec3 Disc(vec2 uv, float radiusPx, float threshold) {\n"                    \
    "    vec3 acc = vec3(0.0); float wsum = 0.0;\n"                             \
    "    for (int i = 0; i < 24; ++i) {\n"                                      \
    "        float r = sqrt((float(i) + 0.5) / 24.0);\n"                         \
    "        float a = float(i) * 2.39996323;\n"                                 \
    "        vec2 o = vec2(cos(a), sin(a)) * r * radiusPx * uTexel;\n"           \
    "        vec3 s = texture(uScene, uv + o).rgb;\n"                            \
    "        float w = 1.0 - r * 0.6;\n"                                         \
    "        acc += max(s - vec3(threshold), vec3(0.0)) * w; wsum += w;\n"      \
    "    }\n"                                                                    \
    "    return acc / max(wsum, 0.0001);\n"                                      \
    "}\n"

const char* kImgBlurFrag = SAGE_IMG_HEAD SAGE_IMG_DISC R"(
uniform float uRadius;
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    vec3 b = Disc(vUV, uRadius * uIntensity, 0.0);
    FragColor = vec4(uIntensity > 0.0 ? mix(c, b, min(1.0, uIntensity * 4.0)) : c, 1.0);
}
)";

// Свечение: светлое выше порога размывается и прибавляется цветом свечения.
const char* kImgGlowFrag = SAGE_IMG_HEAD SAGE_IMG_DISC R"(
uniform float uRadius;
uniform float uThreshold;
uniform vec3 uColor;
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    vec3 g = Disc(vUV, uRadius, uThreshold) / max(1.0 - uThreshold, 0.05);
    FragColor = vec4(c + g * uColor * uIntensity * 2.0, 1.0);
}
)";

// Искажение: кадр колышется волнами (жара, под водой, «плывёт голова»).
const char* kImgDistortionFrag = SAGE_IMG_HEAD R"(
uniform float uAmplitude;
uniform float uFrequency;
uniform float uSpeed;
void main() {
    float t = uTime * uSpeed;
    vec2 off = vec2(sin(vUV.y * uFrequency + t), cos(vUV.x * uFrequency * 0.8 + t * 1.3));
    vec2 uv = vUV + off * uAmplitude * 0.01 * uIntensity;
    FragColor = vec4(texture(uScene, clamp(uv, uTexel, vec2(1.0) - uTexel)).rgb, 1.0);
}
)";

// Растворение: кадр уходит в цвет по шуму, с каймой цвета Edge по границе.
const char* kImgDissolveFrag = SAGE_IMG_HEAD R"(
uniform float uScale;
uniform float uEdge;
uniform vec3 uColor;
uniform vec3 uEdgeColor;
float Hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float Noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash(i), Hash(i + vec2(1, 0)), f.x), mix(Hash(i + vec2(0, 1)), Hash(i + vec2(1, 1)), f.x), f.y);
}
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    vec2 p = vUV / uTexel / max(uScale, 1.0);
    float n = Noise(p) * 0.65 + Noise(p * 2.7) * 0.35;
    // Порог чуть шире 0..1, чтобы на 0 не было ни одной дыры, а на 1 — ни
    // одного островка кадра.
    float cut = uIntensity * (1.0 + uEdge) - uEdge * 0.5;
    if (n < cut) c = uColor;
    else if (n < cut + uEdge) c = uEdgeColor;
    FragColor = vec4(c, 1.0);
}
)";

Shader& Make(const char* frag, const char* name) {
    // Программы живут до конца процесса, как и у остальных звеньев.
    return *new Shader(Shader::FromSource(kFsVert, frag, name));
}

void Common(Shader& sh, PostContext& ctx, const PostEffect& e) {
    sh.SetFloat("uIntensity", glm::clamp(e.Float("intensity", 1.0f), 0.0f, 1.0f));
    sh.SetVec2("uTexel", glm::vec2(1.0f / (float)std::max(ctx.Width, 1), 1.0f / (float)std::max(ctx.Height, 1)));
    sh.SetFloat("uTime", ctx.Time);
}

glm::vec3 Rgb(const PostEffect& e, const char* name) {
    const PostValue* v = e.Find(name);
    return v ? glm::vec3(v->V[0], v->V[1], v->V[2]) : glm::vec3(1.0f);
}

// Одно звено — одно описание: вид, шейдер, параметры, свои юниформы.
struct ImageKind {
    const char* Id;
    const char* Label;
    const char* Hint;
    const char* Frag;
    std::vector<PostParamDesc> Extra;
    void (*Setup)(Shader&, const PostEffect&);
};

} // namespace

void RegisterImageEffects(PostEffectCatalog& catalog) {
    static const auto noExtra = [](Shader&, const PostEffect&) {};
    std::vector<ImageKind> kinds;
    kinds.push_back({"invert", "Invert", "Цвета наоборот: белое — чёрное, красное — голубое", kImgInvertFrag, {}, noExtra});
    kinds.push_back({"negative", "Negative", "Негатив: светлое и тёмное наоборот, оттенок прежний", kImgNegativeFrag, {},
                     noExtra});
    kinds.push_back({"grayscale", "Grayscale", "Чёрно-белое изображение", kImgGrayscaleFrag, {}, noExtra});
    kinds.push_back({"sepia", "Sepia", "Тёплый тон старой фотографии", kImgSepiaFrag, {}, noExtra});
    kinds.push_back({"posterize", "Posterize", "Меньше оттенков: ступенчатый цвет",
                     kImgPosterizeFrag,
                     {Param("levels", "Levels", 6.0f, 2.0f, 32.0f, "Сколько уровней на канал")},
                     [](Shader& sh, const PostEffect& e) { sh.SetFloat("uLevels", std::max(2.0f, e.Float("levels", 6.0f))); }});
    kinds.push_back({"pixelate", "Pixelate", "Крупные пиксели: блок растёт с интенсивностью",
                     kImgPixelateFrag,
                     {Param("size", "Pixel Size", 12.0f, 1.0f, 128.0f, "Размер блока при полной интенсивности, пикселей")},
                     [](Shader& sh, const PostEffect& e) { sh.SetFloat("uSize", std::max(1.0f, e.Float("size", 12.0f))); }});
    kinds.push_back({"blur", "Blur", "Размытие всего кадра",
                     kImgBlurFrag,
                     {Param("radius", "Radius", 8.0f, 0.0f, 64.0f, "Радиус размытия при полной интенсивности, пикселей")},
                     [](Shader& sh, const PostEffect& e) { sh.SetFloat("uRadius", std::max(0.0f, e.Float("radius", 8.0f))); }});
    kinds.push_back({"glow", "Glow", "Свечение светлого: ореол вокруг ярких мест",
                     kImgGlowFrag,
                     {Param("radius", "Radius", 12.0f, 0.0f, 64.0f, "Ширина ореола, пикселей"),
                      Param("threshold", "Threshold", 0.6f, 0.0f, 0.99f, "Светлее чего светится"),
                      ColorParam("color", "Color", "Цвет свечения")},
                     [](Shader& sh, const PostEffect& e) {
                         sh.SetFloat("uRadius", std::max(0.0f, e.Float("radius", 12.0f)));
                         sh.SetFloat("uThreshold", glm::clamp(e.Float("threshold", 0.6f), 0.0f, 0.99f));
                         sh.SetVec3("uColor", Rgb(e, "color"));
                     }});
    kinds.push_back({"distortion", "Distortion", "Кадр колышется волнами",
                     kImgDistortionFrag,
                     {Param("amplitude", "Amplitude", 1.0f, 0.0f, 10.0f, "Сила волн, процентов кадра"),
                      Param("frequency", "Frequency", 20.0f, 1.0f, 100.0f, "Сколько волн на кадр"),
                      Param("speed", "Speed", 2.0f, 0.0f, 20.0f, "Скорость волн")},
                     [](Shader& sh, const PostEffect& e) {
                         sh.SetFloat("uAmplitude", e.Float("amplitude", 1.0f));
                         sh.SetFloat("uFrequency", e.Float("frequency", 20.0f));
                         sh.SetFloat("uSpeed", e.Float("speed", 2.0f));
                     }});
    {
        std::vector<PostParamDesc> extra = {
            Param("scale", "Noise Scale", 24.0f, 1.0f, 256.0f, "Размер пятен, пикселей"),
            Param("edge", "Edge Width", 0.06f, 0.0f, 0.5f, "Ширина каймы на границе"),
            ColorParam("color", "Color", "Во что уходит кадр"),
            ColorParam("edgeColor", "Edge Color", "Цвет каймы")};
        // По умолчанию: уходит в чёрное, кайма огненная.
        extra[2].Default[0] = extra[2].Default[1] = extra[2].Default[2] = 0.0f;
        extra[3].Default[0] = 1.0f;
        extra[3].Default[1] = 0.45f;
        extra[3].Default[2] = 0.1f;
        kinds.push_back({"dissolve", "Dissolve", "Кадр растворяется пятнами в цвет", kImgDissolveFrag, extra,
                         [](Shader& sh, const PostEffect& e) {
                             sh.SetFloat("uScale", std::max(1.0f, e.Float("scale", 24.0f)));
                             sh.SetFloat("uEdge", glm::clamp(e.Float("edge", 0.06f), 0.0f, 0.5f));
                             sh.SetVec3("uColor", Rgb(e, "color"));
                             sh.SetVec3("uEdgeColor", Rgb(e, "edgeColor"));
                         }});
    }

    for (ImageKind& ik : kinds) {
        PostEffectKind k;
        k.Id = ik.Id;
        k.Label = ik.Label;
        k.Hint = ik.Hint;
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Lens;
        k.Params = {Param("intensity", "Intensity", 1.0f, 0.0f, 1.0f, "0 — кадр как был, 1 — эффект целиком")};
        for (PostParamDesc& p : ik.Extra) k.Params.push_back(std::move(p));
        const std::string id = ik.Id;
        Shader* shader = nullptr;
        const char* frag = ik.Frag;
        auto setup = ik.Setup;
        // Шейдер собирается при первом использовании: зарегистрированный, но
        // не включённый эффект не стоит ни одной компиляции.
        k.Run = [id, frag, setup, shader](PostContext& ctx, const PostEffect& e) mutable {
            SAGE_PROFILE("Эффект изображения");
            if (!shader) shader = &Make(frag, ("PostFX.Image." + id).c_str());
            RunSimplePass(ctx, *shader, /*ldrOut=*/true, [&](Shader& sh) {
                Common(sh, ctx, e);
                setup(sh, e);
            });
        };
        catalog.Register(std::move(k));
    }
}

} // namespace sage::render
