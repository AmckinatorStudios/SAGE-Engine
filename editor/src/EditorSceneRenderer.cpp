#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include "EditorSceneRenderer.h"

#include "sage/anim/AnimationComponents.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/Screenshot.h"

#include "sage/ecs/DecalSystem.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/app/Application.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Config.h"
#include "sage/render/DebugView.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/PostFX.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

void EditorSceneRenderer::Init() {
    m_outlineShader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    EnsureShadowMap();   // разрешение и каскады — из конфига (см. ниже)
    m_sceneFbo.emplace(m_vpW, m_vpH);
    m_gameFbo.emplace(m_gameW, m_gameH);
    m_postFbo.emplace(m_vpW, m_vpH);
    m_postfx.emplace();
    m_gamePostFbo.emplace(m_gameW, m_gameH);
    m_gamePostfx.emplace();
    m_previewPostfx.emplace();
    m_debugDraw.emplace();
    m_sky.emplace();
    m_particles.emplace();
    m_outlineMask.emplace(m_vpW, m_vpH);
    m_outlineTri = sage::rhi::GraphicsDevice::Get().CreateGeometry(sage::rhi::VertexLayout{});
}

// Перевод конфига в тракт эффектов живёт в движке (sage/render/PostEffect.h):
// его читают и превью редактора, и собранная игра, и разойтись они не должны
// — «в редакторе одна картинка, в игре другая» началось ровно с того, что этот
// код был здесь и рантайму был недоступен.
//
// Чей тракт брать, решает sage::render::ResolvePostChain: обработка живёт
// компонентом «Пост-обработка» на камере, и вид через камеру обязан считаться
// по нему — и в превью, и в игре одинаково. Нет компонента — нет обработки.

// Небо кадра: кубическая текстура, если у сцены задан её каталог, иначе
// процедурный градиент. Одна точка на оба окна редактора — иначе вьюпорт и
// окно игры легко разъезжаются по фону.
void EditorSceneRenderer::DrawSky(const LightingEnvironment& env, const glm::mat4& view,
                                  const glm::mat4& proj) {
    // Выбор режима неба, запасной путь и цвета по времени суток — общие для
    // всего движка (sage/render/SkyDraw.h). Своя копия здесь означала бы, что
    // вьюпорт, панель Game и собранная игра показывают разное небо, стоит
    // появиться новому режиму.
    sage::render::DrawSceneSky(*m_sky, env, view, proj, m_sceneTime);
}

void EditorSceneRenderer::PrepareReflections(Scene& scene, const LightingEnvironment& env) {
    // Пересъёмка карт отражений. Здесь, а не внутри прохода сцены: захват
    // меняет привязанный буфер и viewport, и посреди отрисовки вьюпорта это
    // писало бы небо в панель редактора.
    // Настройка графики «Отражения» запрещает их так же, как в собранной игре
    // (PlayerLayer): иначе галка в окне настроек меняла бы только игру, а
    // вьюпорт и Game продолжали бы показывать отражения.
    const bool enabled = scene.Reflections.Enabled && sage::EngineConfig::Get().Reflections;
    m_reflections.SetEnabled(enabled);
    m_reflections.SetIntensity(scene.Reflections.Intensity);
    if (!enabled) return;

    // Небо: если у сцены задан набор граней, отражается ОН, а не процедурный
    // градиент — иначе вода отражала бы совсем другое небо, чем видно в кадре.
    std::shared_ptr<Skybox> skyAsset = sage::render::SceneSkyCubemap(env);
    const Skybox* cubemap = skyAsset.get();
    if (m_sky) m_reflections.UpdateSky(*m_sky, env, cubemap);

    // Зонды: не больше одного за кадр. Съёмка — это шесть проходов сцены, и
    // снять десяток зондов разом означало бы застывший на секунду редактор
    // ровно в тот момент, когда художник двинул свет.
    sage::render::UpdateReflectionProbes(
        scene,
        [&](const glm::mat4& v, const glm::mat4& p) {
            sage::rhi::GraphicsDevice& d = sage::Application::Get().Device();
            d.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            d.Clear(true, true);
            DrawSky(env, v, p);
            sage::render::SceneColorInput c;
            c.View = v;
            c.Proj = p;
            c.ViewPos = glm::vec3(glm::inverse(v)[3]);
            c.Env = &env;
            c.Shadows = FrameShadows();
            c.Time = m_sceneTime;
            sage::render::RenderSceneColor(scene, m_batch, c);
        },
        1);
}

void EditorSceneRenderer::EnsureShadowMap() {
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    const int wantRes = cfg.Shadows ? cfg.ShadowResolution : 512;
    const int wantCascades = std::clamp(cfg.ShadowCascades, 1, ShadowMap::kMaxCascades);
    if (!m_shadows || m_shadowRes != wantRes || m_shadowCascades != wantCascades) {
        m_shadows.emplace(wantRes, wantCascades);
        m_shadowRes = wantRes;
        m_shadowCascades = wantCascades;
    }

    // Атлас локальных теней. Плитка — четверть стороны: шестнадцать мест, ровно
    // под четыре прожектора и два точечных источника (см. ShadowAtlas.h).
    const int wantLocal = cfg.LocalShadows ? cfg.LocalShadowResolution : 0;
    if (m_localShadowRes == wantLocal) return;
    m_localShadowRes = wantLocal;
    if (wantLocal <= 0) {
        m_localShadows.reset();
        return;
    }
    m_localShadows.emplace(wantLocal, std::max(wantLocal / 4, 64));
}

ShadowBinding EditorSceneRenderer::FrameShadows() const {
    // Тени включены — по НАСТРОЙКЕ, а не всегда. Здесь стояло жёсткое true, и
    // выключенные тени в настройках редактор игнорировал: в собранной игре их
    // не было, а во вьюпорте и в панели Game они оставались. То есть настройка
    // качества, которую человек трогает первой, когда картинка кажется слишком
    // тёмной, ничего в редакторе не меняла.
    ShadowBinding b(*m_shadows, sage::EngineConfig::Get().Shadows);
    if (m_localShadows) b.Local = m_localShadows->Binding();
    return b;
}

// Подгоняет каскады солнца ПОД ЗАДАННЫЙ ВИД и перерисовывает глубину.
//
// Отдельной функцией, потому что видов ДВА: вьюпорт редактора и панель Game. У
// каскадной карты нет «просто теней сцены» — она обязана быть натянута на
// пирамиду конкретной камеры, и карта, подогнанная под чужую, даёт не «чуть
// хуже», а мусор: выборки уходят за пределы каскада, и по земле идёт чёрная
// рябь. Ровно это и было видно в панели Game, пока она пользовалась картой
// вьюпорта, — при том что панель обещает «как в собранной игре», а собранная
// игра считает тени под свою камеру.
void EditorSceneRenderer::FitSunShadows(Scene& scene, const LightingEnvironment& env,
                                        const ShadowMap::CameraView& view) {
    if (!m_shadows) return;
    Window& window = sage::Application::Get().GetWindow();
    if (m_shadows->CascadeCount() > 1) m_shadows->SetCascades(env.Sun.Direction, view);
    else m_shadows->FitSingle(env.Sun.Direction, view);
    sage::render::RenderShadowDepth(*m_shadows, scene, m_batch, window.Width(), window.Height());
}


// РАБОТАЕТ ЛИ ПОСТ-ОБРАБОТКА НА ЭТОЙ ВИДЕОКАРТЕ.
//
// Обычный вид отличается от отладочных ровно одним: у отладочных
// пост-обработки нет. Поэтому её отказ выглядит не как «пропало свечение», а
// как «во вьюпорте ВСЕ ОБЪЕКТЫ ЧЁРНЫЕ, а отладочные режимы работают» — то есть
// как поломка шейдинга, которой нет. Именно с этим и пришли: на одной машине
// всё хорошо, на другой чёрный экран и никаких следов в логе.
//
// Проверяем один раз на заведомо сером кадре (PostFX::CheckPipeline) и при
// отказе показываем кадр БЕЗ эффектов. Картинка без свечения — это картинка;
// чёрный экран — это неработающий редактор.
bool EditorSceneRenderer::PostWorks() {
    if (!m_postfx) return false;
    const sage::render::PostFX::SelfCheck& check = m_postfx->CheckPipeline();
    if (!check.Ran) return true;
    if (!check.Ok && !m_postWarned) {
        m_postWarned = true;
        m_renderWarning =
            "Пост-обработка на этой видеокарте не работает — кадр показывается без эффектов "
            "(подробности в консоли). Отладочные режимы вида её не используют.";
        LOG_ERROR("Render") << "Пост-обработка на этой видеокарте не работает ("
                            << check.Reason << ") — кадр показывается без эффектов. "
                            << "Отладочные режимы вида её не используют и потому выглядели "
                            << "исправными.";
    }
    return check.Ok;
}

void EditorSceneRenderer::RenderShadow(Scene& scene, LightingEnvironment& env,
                                       const Camera& camera) {
    Window& window = sage::Application::Get().GetWindow();
    const sage::EngineConfig& cfg = sage::EngineConfig::Get();

    // Разрешение и КАСКАДЫ — из конфига, как у рантайма и у игр.
    //
    // Здесь стояло `m_shadows.emplace(2048)`, то есть одна карта 2048 и ноль
    // связи с настройками. Одна карта обязана растянуть свой тексель на всю
    // дальность теней: при ShadowDistance 120 м сфера кадра выходит радиусом
    // под сотню метров, и тексель получается около десяти САНТИМЕТРОВ. Никакая
    // фильтрация этого не лечит — информации в карте просто нет, и тень от
    // столба под ногами превращается в мыльную лесенку. Ровно то, что видно в
    // редакторе, и ровно то, чего не видно в собранной игре: она-то каскады из
    // конфига читала. Пресеты качества при этом честно писали 4096 и три
    // каскада, и всё это пропадало впустую.
    //
    // Пересоздаём только при смене настроек: карты — это память видеокарты, а
    // не покадровый объект.
    EnsureShadowMap();

    // Карта едет за камерой вьюпорта. Раньше здесь стоял ортобокс радиусом
    // 24 м в точке (0,0,0): стоило отлететь от начала мира, и сцена оставалась
    // без теней — что читается как «тени выключены», а не как ошибка.
    ShadowMap::CameraView v;
    v.Position = camera.Position;
    v.Forward = camera.Front;
    v.Up = camera.Up;
    v.FovY = glm::radians(camera.Fov);
    v.Aspect = window.Height() > 0 ? (float)window.Width() / (float)window.Height() : 1.0f;
    v.Near = camera.NearClip;
    // Дальность теней: сцена главнее настроек. Масштаб мира знает игра, а не
    // игрок в меню качества (см. sage::ShadowSettings).
    v.ShadowDistance = env.Shadows.Distance > 0.0f ? env.Shadows.Distance : cfg.ShadowDistance;
    // Солнце — только при включённых тенях, ровно как в рантайме. Раньше проход
    // шёл всегда: карта честно заполнялась и честно стоила своего времени, а
    // привязка объявляла тени включёнными независимо от настройки (см.
    // FrameShadows), — то есть выключить их в редакторе было нельзя.
    // Тени ЛАМП живут по своей настройке (LocalShadows) и здесь ни при чём:
    // рантайм разделяет их так же.
    if (cfg.Shadows) FitSunShadows(scene, env, v);

    // Тени ламп — своим проходом и по своим правилам: у них нет ни каскадов, ни
    // привязки к камере, зато есть раздача мест в атласе.
    if (m_localShadows) {
        m_localShadows->Prepare(env);
        sage::render::RenderLocalShadowDepth(*m_localShadows, scene, m_batch, window.Width(),
                                             window.Height());
    }
}

void EditorSceneRenderer::DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                                  const glm::mat4& proj, glm::vec3 viewPos, int shadingMode,
                                  bool wireframe, int viewId) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);
    // Статика — RenderBatch (отсечение по фрустуму + инстансный батчинг).
    sage::render::SceneColorInput color;
    color.View = view;
    color.Proj = proj;
    color.ViewPos = viewPos;
    color.Env = &env;
    color.Shadows = FrameShadows();
    color.ShadingMode = shadingMode;
    color.OcclusionCulling = sage::EngineConfig::Get().OcclusionCulling;
    // Своя таблица перекрытия на каждое окно. Общая означала бы, что игровая
    // камера решает за редакторскую: объект, закрытый от неё стеной, исчезал
    // бы и во вьюпорте — там, где между ним и наблюдателем ничего нет.
    color.ViewId = viewId;
    color.Time = m_sceneTime;
    color.Reflection = m_reflections.Binding(1, 1);
    // Зонды — каждому объекту свой, по его положению (см. ReflectionProbeSet):
    // выбор по камере раздавал интерьер комнаты всему кадру.
    if (m_reflections.Enabled()) {
        m_probeSet = sage::render::CollectReflectionProbes(scene);
        color.Reflection.Probes = &m_probeSet;
    }
    m_lastStats = sage::render::RenderSceneColor(scene, m_batch, color);
    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill);
}

namespace {
// Краевой проход аутлайна: дилатация силуэта из масочного буфера на ПОСТОЯННУЮ
// ширину в пикселях (kOutlineRadius), поверх готового кадра. Пиксель вне
// силуэта, но рядом с ним (в радиусе) — кайма. Ширина не зависит ни от размера
// объекта, ни от расстояния до камеры. Работает для любой геометрии.
const char* kOutlineEdgeVert = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kOutlineEdgeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask;
uniform vec2 uTexel;   // 1 / размер маски
uniform vec3 uColor;
const int R = 3;       // ширина каймы в пикселях (постоянная на экране)
void main() {
    float center = texture(uMask, vUV).r;
    if (center > 0.5) discard;              // внутри силуэта каймы нет
    float near = 0.0;
    for (int y = -R; y <= R; ++y)
        for (int x = -R; x <= R; ++x)
            near = max(near, texture(uMask, vUV + vec2(x, y) * uTexel).r);
    if (near < 0.5) discard;                // вокруг нет объекта — не кайма
    FragColor = vec4(uColor, 1.0);
}
)";
Shader& OutlineEdgeShader() {
    static Shader* s = new Shader(Shader::FromSource(kOutlineEdgeVert, kOutlineEdgeFrag, "OutlineEdge"));
    return *s;
}

// Возврат готового кадра в буфер сцены — один полноэкранный треугольник.
const char* kCopyFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
void main() { FragColor = vec4(texture(uSrc, vUV).rgb, 1.0); }
)";
Shader& CopyShader() {
    static Shader* s = new Shader(Shader::FromSource(kOutlineEdgeVert, kCopyFrag, "EditorCopy"));
    return *s;
}
} // namespace

// Рисует силуэт выбранного меша сплошным белым в масочный буфер (без теста
// глубины — полный силуэт даже при частичном перекрытии другими объектами).
void EditorSceneRenderer::RenderOutlineMask(Scene& scene, const std::vector<int>& selection,
                                            const glm::mat4& view, const glm::mat4& proj, int w,
                                            int h) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    m_outlineMaskW = w;
    m_outlineMaskH = h;
    m_outlineMask->Resize(w, h);
    m_outlineMask->Bind();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear();

    m_outlineShader->Use();
    m_outlineShader->SetMat4("uView", view);
    m_outlineShader->SetMat4("uProjection", proj);
    m_outlineShader->SetInt("uShadingMode", 1); // unlit — плоский белый силуэт
    m_outlineShader->SetInt("uUseTexture", 0);
    m_outlineShader->SetInt("uFogEnabled", 0);
    m_outlineShader->SetVec3("uObjectColor", {1.0f, 1.0f, 1.0f});

    device.SetDepthTest(false); // полный силуэт независимо от перекрытий
    // Все выбранные сущности с мешем — в одну маску (кайма охватит их все).
    const glm::mat4 viewProj = proj * view;
    for (int id : selection) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;

        // ПЕРСОНАЖ ОБВОДИТСЯ ПО ТОЙ ПОЗЕ, В КОТОРОЙ ОН НАРИСОВАН.
        //
        // У сущности со скелетом на экране скелетная модель в позе клипа, а
        // статическая копия в Mesh — это поза привязки (растопыренная
        // Т-поза). Пока кайма шла по ней, выделенный персонаж стоял внутри
        // чужого контура: выделено правильно, а выглядит как поломка.
        if (const AnimationComponent* am =
                scene.Registry().try_get<AnimationComponent>(obj.Entity())) {
            if (am->Model) {
                am->Model->DrawSilhouette(scene.WorldMatrix(obj.Entity()), viewProj,
                                          am->Anim.BoneMatrices(), &am->MorphWeights);
                // Шейдер силуэта сменил текущую программу — возвращаем свою,
                // иначе следующий статический меш нарисуется его шейдером.
                m_outlineShader->Use();
                continue;
            }
        }

        const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(obj.Entity());
        if (!mr || !mr->MeshPtr) continue;
        m_outlineShader->SetMat4("uModel", scene.WorldMatrix(obj.Entity()));
        mr->MeshPtr->Draw();
    }
    device.SetDepthTest(true);
}

// Накладывает кайму из маски на итоговый кадр (target — то, что уходит во вьюпорт).
void EditorSceneRenderer::CompositeOutline(Framebuffer& target) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    target.Bind();
    device.SetViewport(0, 0, m_outlineMaskW, m_outlineMaskH);
    device.SetDepthTest(false);
    device.SetBlend(true);

    Shader& edge = OutlineEdgeShader();
    edge.Use();
    edge.SetInt("uMask", 0);
    edge.SetVec2("uTexel", {1.0f / (float)m_outlineMaskW, 1.0f / (float)m_outlineMaskH});
    edge.SetVec3("uColor", {1.0f, 0.62f, 0.12f}); // янтарная кайма выделения
    device.BindTexture2D(0, m_outlineMask->ColorTexture());
    m_outlineTri->DrawArrays(3);

    device.SetDepthTest(true);
    // Смешивание ОБЯЗАНО выключиться здесь же. Это последний проход вьюпорта, и
    // оставленное включённым оно утекало в следующий кадр и в панель Game: там
    // сцена рисуется поверх заливки фона, и чей-нибудь материал с alpha < 1
    // (или небо) начинал смешиваться с ней вместо того, чтобы её заменить.
    // Состояние, выставленное проходом, снимает тот же проход.
    device.SetBlend(false);
}


void EditorSceneRenderer::RenderViewport(Scene& scene, Camera& camera, const LightingEnvironment& env,
                                         int selectedId, const std::vector<int>& selection,
                                         EditorRenderMode mode, bool showGrid,
                                         const sage::EngineConfig& cfg, glm::mat4& outView,
                                         glm::mat4& outProj, int slot,
                                         const EditorViewOverride& viewOverride) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    // Наклейки — до отрисовки: только помеченные, поэтому вызов дешёвый. В
    // редакторе это ещё и «правка параметров видна сразу»: инспектор взводит
    // Dirty, и следующий кадр уже с новой проекцией.
    sage::ecs::BuildDecals(scene, sage::ecs::MakeDecalMesh);

    slot = std::max(0, std::min(slot, kMaxViews - 1));
    const bool primary = (slot == 0);
    const int w = primary ? m_vpW : m_extraW[slot];
    const int h = primary ? m_vpH : m_extraH[slot];
    if (w < 8 || h < 8) return;   // панель ещё не разложена — рисовать не во что

    // MSAA — на буфере СЦЕНЫ, а не экранного окна.
    //
    // Раньше настройка `msaa` жила в конфиге, читалась, сохранялась и даже
    // ставилась пресетом Ultra в 4 — и не делала ничего: сглаживание
    // растеризатора работает там, где растеризуется геометрия, а сцена рисуется
    // в свой offscreen-буфер, который заводился всегда односэмпловым. Экранный
    // буфер, к которому эту настройку обычно и привязывают (GLFW_SAMPLES), в
    // цепочке с пост-обработкой не участвует вовсе — в него уходит уже готовая
    // картинка одним треугольником, сглаживать в нём нечего.
    //
    // Буферы пост-обработки остаются односэмпловыми: у них нет геометрии.
    const int samples = sage::render::SceneSamples(cfg);
    EnsureFramebuffer(primary ? m_sceneFbo : m_extraFbo[slot], w, h, samples,
                      sage::render::SceneWantsGBuffer(cfg));
    EnsureFramebuffer(primary ? m_postFbo : m_extraPostFbo[slot], w, h);
    Framebuffer& sceneFbo = primary ? *m_sceneFbo : *m_extraFbo[slot];
    Framebuffer& postFbo = primary ? *m_postFbo : *m_extraPostFbo[slot];

    sceneFbo.Bind();
    // Тем же правилом, что и панель Game: небо выключено — за сценой чернота.
    // Свой серый фон вьюпорта означал бы, что «нет неба» выглядит в редакторе
    // одним, а в игре другим.
    const glm::vec3 viewportClear = sage::render::SceneClearColor(env);
    device.SetClearColor(viewportClear.r, viewportClear.g, viewportClear.b, 1.0f);
    device.Clear();

    // Ортогональные виды приходят готовыми матрицами: у них нет свободной
    // камеры, а есть плоскость, центр и масштаб. Матрицы нужны вызывающему в
    // любом случае — по ним он считает пикинг и гизмо.
    outView = viewOverride.Use ? viewOverride.View : camera.GetViewMatrix();
    outProj = viewOverride.Use ? viewOverride.Proj
                           : camera.GetProjectionMatrix((float)w / (float)std::max(h, 1));
    const glm::vec3 eye = viewOverride.Use ? viewOverride.EyePos : camera.Position;

    // Съёмка в именованные картинки — до сцены и в свои буферы; после неё
    // возвращаем буфер сцены и его вьюпорт.
    if (primary && sage::render::RenderTextureViews(scene, m_batch, env, m_sceneTime) > 0) {
        sceneFbo.Bind();
        device.SetViewport(0, 0, w, h);
    }
    DrawSky(env, outView, outProj);

    // Режим рендера из тулбара. Shaded — обычная отрисовка, Wireframe — тот же
    // unlit плюс линии, всё остальное переводится в DebugView сдвигом на один:
    // после Shaded в списке стоит Wireframe, которого в DebugView нет.
    int shadingMode = 0;
    bool wireframe = false;
    if (mode == EditorRenderMode::Wireframe) {
        shadingMode = (int)sage::render::DebugView::Unlit;
        wireframe = true;
    } else if (mode != EditorRenderMode::Shaded) {
        shadingMode = (int)mode - 1;
    }
    DrawLit(scene, env, outView, outProj, eye, shadingMode, wireframe, /*viewId=*/slot);
    // Скорости — СРАЗУ после геометрии: проход берёт список видимого у
    // последнего цветного прохода батча, и любой другой проход между ними
    // подменил бы его чужим.
    sage::rhi::TextureHandle viewportVelocity;
    {
        if (primary && mode == EditorRenderMode::Shaded && !viewOverride.Use &&
            sage::render::ChainNeedsVelocity(
                sage::render::FramePostChain(scene, sage::ecs::PrimaryCameraEntity(scene)))) {
            viewportVelocity = m_viewportVelocity.Render(m_batch, outView, outProj, w, h);
            sceneFbo.Bind();
            device.SetViewport(0, 0, w, h);
        }
    }
    m_particles->Draw(camera, outView, outProj);

    GameObject selectedObj = scene.Get(selectedId);

    // Объём — после геометрии и до пост-обработки: свечение лучей должно
    // попасть в bloom. В плоском холсте вёрстки его нет — там нет и сцены.
    if (cfg.Volumetrics) {
        sceneFbo.Resolve();
        if (!m_volumetrics) m_volumetrics.emplace();
        m_volumetrics->Render(sceneFbo, sceneFbo.DepthTexture(), w, h, outProj, outView, eye, env,
                              FrameShadows(), sage::render::VolumetricsFromConfig(cfg),
                              m_sceneTime, /*viewId=*/slot);
    }


    // Силуэты ВСЕХ выбранных объектов в масочный буфер (до поста, свой FBO).
    if (!selection.empty()) RenderOutlineMask(scene, selection, outView, outProj, w, h);

    // Сведение многосэмплового буфера в обычные текстуры — ПОСЛЕ всей отрисовки
    // сцены и ДО первого чтения. Без него весь пост-процесс (и сам показ кадра
    // в панели) читал бы пустую текстуру: при MSAA рисование идёт в
    // многосэмпловые renderbuffer'ы, а наружу отдаются разрешённые копии.
    // Без MSAA это пустышка.
    sceneFbo.Resolve();

    // Пост-обработка — только Shaded + включена в конфиге (отладочные режимы как есть).
    //
    // У ортогональных видов её нет НАМЕРЕННО: свечение и глубина резкости на
    // схематичном виде сверху мешают попасть по объекту, а именно ради точного
    // попадания такой вид и открывают.
    // ВЬЮПОРТ ПОКАЗЫВАЕТ ОБРАБОТКУ ГЛАВНОЙ КАМЕРЫ СЦЕНЫ.
    //
    // Смотрит он редакторской камерой, и своего компонента у неё быть не может.
    // Раньше здесь брался тракт ПРОЕКТА — настройка, которой больше нет:
    // обработка принадлежит камере. Главная камера и есть тот кадр, ради
    // которого настраивают вид, поэтому вьюпорт показывает её обработку: «в
    // редакторе одно, в игре другое» начинается ровно с расхождения этих двух.
    // Нет камеры с компонентом — кадр всё равно ВЫВОДИТСЯ базовым трактом
    // (кривая и кодирование для экрана, см. FramePostChain): сырой линейный
    // HDR на экране — тёмная кислотная картинка, а не «кадр без обработки».
    bool postApplied = false;
    const sage::render::PostChain viewportChain =
        sage::render::FramePostChain(scene, sage::ecs::PrimaryCameraEntity(scene));
    if (mode == EditorRenderMode::Shaded && !viewOverride.Use && PostWorks()) {
        postFbo.Resize(w, h);
        m_postfx->SetLighting(&env);   // солнце — звену «Lens Flare»
        m_postfx->SetGBuffer(sceneFbo.AlbedoTexture(), sceneFbo.NormalTexture());
        m_postfx->Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), sceneFbo.Width(),
                         sceneFbo.Height(), outProj, outView, viewportChain,
                         /*output=*/&postFbo, 0, 0, w, h, viewportVelocity);
        postApplied = true;
    }
    // ГОТОВЫЙ КАДР ВОЗВРАЩАЕТСЯ В БУФЕР СЦЕНЫ — и дальше всё рисуется в него.
    //
    // Так гизмо оказываются ЗА пост-обработкой, а не под ней. Пока они шли
    // вместе со сценой, их проглатывал тон-маппинг: чистый красный превращался
    // в бурый, свечение размывало линии, а глубина резкости просто расфокусиро-
    // вывала оси у дальнего объекта. Гизмо — это не часть картинки мира, это
    // инструмент поверх неё, и «настройки красоты» не имеют права на него
    // влиять.
    //
    // Копией, а не отрисовкой гизмо в буфер поста: у буфера поста нет глубины
    // сцены, и линии в нём ничем не заслонялись бы — сетка светила бы сквозь
    // стены. В буфере сцены глубина цела (копия идёт с выключенным тестом
    // глубины и потому её не трогает).
    if (postApplied) {
        sceneFbo.Bind();
        device.SetViewport(0, 0, w, h);
        device.SetDepthTest(false);
        device.SetBlend(false);
        Shader& copy = CopyShader();
        copy.Use();
        copy.SetInt("uSrc", 0);
        device.BindTexture2D(0, postFbo.ColorTexture());
        m_outlineTri->DrawArrays(3);
    } else {
        sceneFbo.Bind();
        device.SetViewport(0, 0, w, h);
    }
    // Тест глубины включаем ЯВНО и всегда: выше прошли проходы, которые его
    // выключают (копия кадра, маска силуэта, пост-обработка), а гизмо без него
    // светили бы сквозь стены. Полагаться на то, что каждый проход прибрал за
    // собой, здесь нельзя — это ровно тот случай, когда «обычно прибирает»
    // означает «однажды не прибрал, и сетка поехала поверх сцены».
    device.SetDepthTest(true);
    // И РЕЖИМ ЗАЛИВКИ — ТОЖЕ ЯВНО. В каркасном режиме вся сцена рисуется
    // линиями (SetPolygonMode(Line)), и полноэкранный треугольник сетки в нём
    // превратился бы в три отрезка по краю экрана. Дебаг не обязан меняться от
    // того, как человек смотрит на сцену.
    device.SetPolygonMode(sage::rhi::PolygonMode::Fill);

    // --- СЕТКА: ТОТ ЖЕ ДЕБАГ, ЧТО И ГИЗМО ---------------------------------
    //
    // ШЕЙДЕРОМ, А НЕ ЛИНИЯМИ (sage::render::GridRenderer). Линейная сетка
    // задаёт вопрос, на который нет хорошего ответа: сколько линий рисовать.
    // Сколько ни рисуй, они кончаются — и человек видит ровно то, чего в
    // опорной сетке быть не должно: границу квадрата, вторую границу там, где
    // мелкая клетка сменяется крупной, и пустоту за ними, стоит отлететь или
    // опуститься ниже пола. Здесь вместо линий один полноэкранный треугольник:
    // шейдер пересекает луч с плоскостью и считает линию аналитически, по
    // производной координаты, — толщина выходит постоянной в пикселях, у
    // горизонта сетка не рябит, а гаснет, и «бесконечная» она в буквальном
    // смысле, потому что её протяжённость ничего не стоит.
    //
    // РИСУЕТСЯ ПОСЛЕ ПОСТ-ОБРАБОТКИ, вместе с остальным дебагом. Раньше она шла
    // вместе со сценой — и тон-маппинг с экспозицией красили её в тот цвет,
    // который сейчас у картинки: на тёмной сцене она тонула, на яркой белела, а
    // глубина резкости размывала её у горизонта. Опорная сетка, которая меняет
    // вид от настроек красоты, перестаёт быть опорной. Сглаживание при этом не
    // теряется: рисуем в многосэмпловый буфер сцены, он сводится в самом конце
    // (см. Resolve ниже).
    if (showGrid) {
        if (!m_grid) m_grid.emplace();
        sage::render::GridSettings gs;
        gs.Mode = sage::render::GridSettings::Extent::Infinite;
        // ШАГ КЛЕТКИ — ДЕСЯТКАМИ ПО ВЫСОТЕ. Метровая клетка с километровой
        // высоты — это сплошная заливка (на пиксель приходится десяток линий),
        // а стометровая у самого пола — три полосы на весь экран. Переход
        // происходит там же, где у сетки и так гаснет мелкая клетка, поэтому
        // на глаз он не читается как скачок.
        const float height = std::max(std::abs(eye.y), 1.0f);
        float cell = 1.0f;
        while (height / cell > 60.0f) cell *= 10.0f;
        gs.CellSize = cell;
        gs.MajorEvery = 10;
        gs.MinorColor = glm::vec3(0.24f, 0.26f, 0.30f);
        gs.MajorColor = glm::vec3(0.38f, 0.41f, 0.47f);
        gs.Opacity = 1.0f;
        // Оси сетки — теми же чистыми цветами, что гизмо и оси объекта: в
        // приглушённом виде они на тёмном полу читались как «ещё две серые
        // линии», и было не понять, где X, а где Z.
        gs.XAxisColor = glm::vec3(1.00f, 0.22f, 0.22f);
        gs.ZAxisColor = glm::vec3(0.22f, 0.46f, 1.00f);
        // Глубину сетка в редакторе НЕ пишет: следом рисуются гизмо, и линии на
        // её плоскости иначе проигрывают ей тест глубины (см. GridSettings).
        gs.WriteDepth = false;
        // ДАЛЬНОСТЬ ЗАТУХАНИЯ — от высоты, но с приличным запасом: автоматика
        // шейдера рассчитана на «сетку под ногами», а во вьюпорте по ней
        // выравнивают предметы в десятках метров от камеры, и обрыв в двух
        // шагах читается как та же граница, от которой уходили.
        gs.FadeDistance = std::max(height * 30.0f, 120.0f);
        m_grid->Draw(outView, outProj, eye, gs);
    }

    // --- ГИЗМО-ГРАФИКА: оси, каркасы, значки (DebugDraw) -------------------
    //
    // С тестом глубины по буферу сцены: объекты заслоняют гизмо, как и должны.
    DrawEntityGizmos(scene, selection, (float)m_gameW / (float)std::max(m_gameH, 1));
    if (selectedObj.Valid()) {
        glm::mat4 world = scene.WorldMatrix(selectedObj.Entity());
        m_debugDraw->Axes(world, 1.4f);
        if (const LightComponent* light = scene.Registry().try_get<LightComponent>(selectedObj.Entity())) {
            glm::vec3 wpos = glm::vec3(world[3]);
            glm::vec3 lightColor = glm::vec3(light->Color) * 0.9f;
            if (light->Kind == LightComponent::Type::Spot) {
                glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
                m_debugDraw->WireCone(wpos, dir, light->Range, light->OuterConeDeg, lightColor);
            } else if (light->Kind == LightComponent::Type::Area) {
                // Сама панель и короткий штрих лицевой стороны: светит она
                // одной стороной, и без штриха не видно, какой.
                const glm::vec3 r = glm::normalize(glm::vec3(world[0])) * (light->AreaWidth * 0.5f);
                const glm::vec3 u = glm::normalize(glm::vec3(world[1])) * (light->AreaHeight * 0.5f);
                const glm::vec3 f = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
                const glm::vec3 c[4] = {wpos + r + u, wpos - r + u, wpos - r - u, wpos + r - u};
                for (int i = 0; i < 4; ++i) m_debugDraw->Line(c[i], c[(i + 1) % 4], lightColor);
                m_debugDraw->Line(wpos, wpos + f * std::min(light->Range, 1.0f), lightColor);
            } else {
                m_debugDraw->WireSphere(wpos, light->Range, lightColor);
            }
        }
    }
    // Габариты выделенного — ровно та коробка, по которой считается попадание
    // мышью (sage::render::RayMesh). Показывать её незачем всегда, но когда
    // «клик выбрал не то» непонятен, увидеть её — самый короткий ответ.
    if (m_showBounds) {
        for (int id : selection) {
            GameObject o = scene.Get(id);
            if (!o.Valid()) continue;
            const MeshRendererComponent* mr =
                scene.Registry().try_get<MeshRendererComponent>(o.Entity());
            if (!mr || !mr->MeshPtr) continue;
            const glm::vec3 bmin = mr->MeshPtr->BoundsMin();
            const glm::vec3 bmax = mr->MeshPtr->BoundsMax();
            // Коробка рисуется в ЛОКАЛЬНЫХ осях объекта (мировая матрица целиком,
            // включая поворот) — именно в них её и проверяет пикинг.
            const glm::mat4 box = scene.WorldMatrix(o.Entity()) *
                                  glm::translate(glm::mat4(1.0f), (bmin + bmax) * 0.5f) *
                                  glm::scale(glm::mat4(1.0f), bmax - bmin);
            m_debugDraw->WireBox(box, glm::vec3(1.0f, 0.65f, 0.2f));
        }
    }
    // Отладочная графика игры — вместе с гизмо редактора: и то и другое линии
    // с тестом глубины, и рисовать их двумя разными способами незачем.
    DrawScriptDebug();
    m_debugDraw->Flush(outView, outProj);

    // Кайма выделения — поверх ИТОГОВОГО кадра (постоянная ширина в пикселях,
    // читаемая независимо от размера объекта и дистанции).
    if (!selection.empty()) CompositeOutline(sceneFbo);

    // Буфер сцены при MSAA многосэмпловый, а наружу отдаётся разрешённая копия:
    // всё, что дописано после первого Resolve (кадр поста, гизмо, кайма), без
    // повторного сведения просто не попало бы в показываемую текстуру.
    sceneFbo.Resolve();

    device.BindDefaultFramebuffer();
}

void EditorSceneRenderer::DrawScriptDebug() {
    if (!m_scriptDebug || !m_debugDraw) return;
    for (const sage::render::DebugLine& l : m_scriptDebug->All())
        m_debugDraw->Line(l.A, l.B, l.Color);
}

void EditorSceneRenderer::SetCameraPreviewSize(int w, int h) {
    // Нижняя граница — чтобы буфер не стал вырожденным, верхняя — чтобы превью
    // не превратилось во второй полноразмерный вьюпорт: это карточка в углу, и
    // каждый её пиксель стоит прохода сцены.
    m_previewW = std::clamp(w, 64, 960);
    m_previewH = std::clamp(h, 36, 540);
}

uint64_t EditorSceneRenderer::CameraPreviewTexture() const {
    if (!m_previewValid) return 0;
    if (m_previewPostApplied && m_previewPostFbo) return m_previewPostFbo->NativeColorTexture();
    return m_previewFbo ? m_previewFbo->NativeColorTexture() : 0;
}


void EditorSceneRenderer::SetViewportSize(int slot, int w, int h) {
    if (slot <= 0) { m_vpW = w; m_vpH = h; return; }
    if (slot >= kMaxViews) return;
    m_extraW[slot] = w;
    m_extraH[slot] = h;
}

// ПОКАЗЫВАЕТСЯ БУФЕР СЦЕНЫ — ВСЕГДА.
//
// Кадр после пост-обработки возвращается в него копией (см. RenderViewport), и
// только в нём поверх лежат гизмо и кайма выделения. Выбор «пост или сцена»,
// который был здесь раньше, теперь означал бы картинку без инструментов.
uint64_t EditorSceneRenderer::ViewportTexture(int slot) const {
    if (slot <= 0) return ViewportTexture();
    if (slot >= kMaxViews || !m_extraFbo[slot]) return 0;
    return m_extraFbo[slot]->NativeColorTexture();
}

uint64_t EditorSceneRenderer::ViewportTexture() const {
    return m_sceneFbo->NativeColorTexture();
}
bool EditorSceneRenderer::SaveGameFrame(const std::string& path) {
    if (!m_gameFrameValid) return false;   // чужой кадр за свой не выдаём
    Framebuffer* target = m_gamePostApplied ? (m_gamePostFbo ? &*m_gamePostFbo : nullptr)
                                            : (m_gameFbo ? &*m_gameFbo : nullptr);
    if (!target) return false;
    // Читаем из ТОГО ЖЕ буфера, который показывает панель Game: снимок обязан
    // совпасть с тем, что человек видит, иначе обложка врёт по-другому.
    target->Bind();
    SaveScreenshot(path, m_gameW, m_gameH);
    sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    return true;
}

// Снимок ВЬЮПОРТА в файл — запасной путь для обложки сцены, у которой нет
// игровой камеры. Тот же буфер, что показывает панель: обложка обязана
// совпасть с тем, что человек видел, когда сохранял.
bool EditorSceneRenderer::SaveViewportFrame(const std::string& path) {
    Framebuffer* target = m_sceneFbo ? &*m_sceneFbo : nullptr;
    if (!target || target->Width() <= 0 || target->Height() <= 0) return false;
    target->Bind();
    SaveScreenshot(path, target->Width(), target->Height());
    sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    return true;
}

bool EditorSceneRenderer::ReadViewportPixels(std::vector<unsigned char>& out, int& outW,
                                             int& outH) {
    // Тот же буфер, что и у ViewportTexture: показываем и читаем одно и то же,
    // иначе проверка сравнивала бы не то, что видно.
    Framebuffer* target = m_sceneFbo ? &*m_sceneFbo : nullptr;
    if (!target) return false;
    outW = target->Width();
    outH = target->Height();
    if (outW <= 0 || outH <= 0) return false;
    out.assign((size_t)outW * (size_t)outH * 3u, 0);
    target->Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.ReadPixelsRGB(0, 0, outW, outH, out.data());
    device.BindDefaultFramebuffer();
    return true;
}

bool EditorSceneRenderer::ReadGamePixels(std::vector<unsigned char>& out, int& outW, int& outH) {
    if (!m_gameFrameValid) return false;   // чужой кадр за свой не выдаём
    // Тот же буфер, что и у GameTexture: читаем ровно то, что видно.
    Framebuffer* target = m_gamePostApplied ? (m_gamePostFbo ? &*m_gamePostFbo : nullptr)
                                            : (m_gameFbo ? &*m_gameFbo : nullptr);
    if (!target) return false;
    outW = target->Width();
    outH = target->Height();
    if (outW <= 0 || outH <= 0) return false;
    out.assign((size_t)outW * (size_t)outH * 3u, 0);
    target->Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.ReadPixelsRGB(0, 0, outW, outH, out.data());
    device.BindDefaultFramebuffer();
    return true;
}

uint64_t EditorSceneRenderer::GameTexture() const {
    return m_gamePostApplied && m_gamePostFbo ? m_gamePostFbo->NativeColorTexture()
                                              : m_gameFbo->NativeColorTexture();
}

sage::ecs::RenderStats EditorSceneRenderer::RenderColorForTest(Scene& scene, const glm::mat4& view,
                                                              const glm::mat4& proj, const glm::vec3& viewPos,
                                                              const LightingEnvironment& env) {
    return m_batch.RenderColor(scene, view, proj, viewPos, env,
                               FrameShadows(), 0);
}
