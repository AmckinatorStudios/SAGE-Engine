#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include "EditorSceneRenderer.h"

#include "sage/ecs/DecalSystem.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/core/Application.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Config.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/PostFX.h"
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
    m_debugDraw.emplace();
    m_sky.emplace();
    m_particles.emplace();
    m_outlineMask.emplace(m_vpW, m_vpH);
    m_outlineTri = sage::rhi::GraphicsDevice::Get().CreateGeometry(sage::rhi::VertexLayout{});
}

// Перевод конфига в настройки эффектов живёт в движке (sage/render/PostFX.h):
// его читают и превью редактора, и собранная игра, и разойтись они не должны
// — «в редакторе одна картинка, в игре другая» началось ровно с того, что этот
// код был здесь и рантайму был недоступен.
sage::render::PostFXSettings EditorSceneRenderer::FxFromConfig(const sage::EngineConfig& cfg) {
    return sage::render::FxFromConfig(cfg);
}

// Небо кадра: кубическая текстура, если у сцены задан её каталог, иначе
// процедурный градиент. Одна точка на оба окна редактора — иначе вьюпорт и
// окно игры легко разъезжаются по фону.
void EditorSceneRenderer::DrawSky(const LightingEnvironment& env, const glm::mat4& view,
                                  const glm::mat4& proj) {
    if (!env.Skybox.Enabled) return;
    if (env.Skybox.HasCubemap()) {
        if (std::shared_ptr<Skybox> sky = ResourceManager::Instance().GetSkybox(env.Skybox.CubemapDir)) {
            sky->Draw(view, proj, env.Skybox.Intensity, env.Skybox.RotationDeg);
            return;
        }
        // Каталог задан, но не читается — падать на этом нельзя, поэтому
        // молча остаёмся на градиенте (причина уже в логе от ResourceManager).
    }
    m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor,
                CelestialsFromEnvironment(env));
}

void EditorSceneRenderer::PrepareReflections(Scene& scene, const LightingEnvironment& env) {
    // Пересъёмка карт отражений. Здесь, а не внутри прохода сцены: захват
    // меняет привязанный буфер и viewport, и посреди отрисовки вьюпорта это
    // писало бы небо в панель редактора.
    m_reflections.SetEnabled(scene.Reflections.Enabled);
    m_reflections.SetIntensity(scene.Reflections.Intensity);
    if (!scene.Reflections.Enabled) return;

    // Небо: если у сцены задан набор граней, отражается ОН, а не процедурный
    // градиент — иначе вода отражала бы совсем другое небо, чем видно в кадре.
    const Skybox* cubemap = nullptr;
    std::shared_ptr<Skybox> skyAsset;
    if (env.Skybox.HasCubemap()) {
        skyAsset = ResourceManager::Instance().GetSkybox(env.Skybox.CubemapDir);
        cubemap = skyAsset.get();
    }
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
    ShadowBinding b(*m_shadows, true);
    if (m_localShadows) b.Local = m_localShadows->Binding();
    return b;
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
    if (m_shadows->CascadeCount() > 1) m_shadows->SetCascades(env.Sun.Direction, v);
    else m_shadows->FitSingle(env.Sun.Direction, v);
    sage::render::RenderShadowDepth(*m_shadows, scene, m_batch, window.Width(), window.Height());

    // Тени ламп — своим проходом и по своим правилам: у них нет ни каскадов, ни
    // привязки к камере, зато есть раздача мест в атласе.
    if (m_localShadows) {
        m_localShadows->Prepare(env);
        sage::render::RenderLocalShadowDepth(*m_localShadows, scene, m_batch, window.Width(),
                                             window.Height());
    }
}

void EditorSceneRenderer::DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                                  const glm::mat4& proj, glm::vec3 viewPos, int shadingMode, bool wireframe) {
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
    color.Time = m_sceneTime;
    color.Reflection = m_reflections.Binding(1, 1);
    // Зонд перебивает небо: если камера стоит в его коробке, отражается
    // окружение, снятое им, а не небо вообще.
    float probeIntensity = 1.0f;
    if (const sage::render::EnvironmentMap* probe =
            sage::render::PickReflectionProbe(scene, viewPos, &probeIntensity)) {
        color.Reflection.Env = probe;
        color.Reflection.Intensity = probeIntensity;
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
    for (int id : selection) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
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
}

// Гизмо невидимых/физических сущностей (камера/свет/эмиттер/коллайдер) — всегда
// видны и кликабельны. Накопление в DebugDraw, Flush — у вызывающего.
void EditorSceneRenderer::DrawEntityGizmos(Scene& scene, const std::vector<int>& selection, float gameAspect) {
    auto& reg = scene.Registry();
    auto isSel = [&](int id) {
        return std::find(selection.begin(), selection.end(), id) != selection.end();
    };
    // Камеры: каркас усечённой пирамиды (frustum).
    auto camView = reg.view<CameraComponent, Transform, IdComponent>();
    for (auto e : camView) {
        bool selected = isSel(camView.get<IdComponent>(e).Id);
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.8f, 0.2f) : glm::vec3(0.5f, 0.7f, 0.9f);
        glm::mat4 world = scene.WorldMatrix(e); // мировая (иерархия)
        glm::vec3 wpos = glm::vec3(world[3]);
        glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
        const CameraComponent& cam = camView.get<CameraComponent>(e);
        m_debugDraw->WireFrustum(wpos, fwd, cam.Fov, gameAspect, 0.3f, 2.2f, color);
    }
    // Свет-сущности: маркер-сфера в позиции (у выбранного — ярче; крупная зона
    // выбранного примитива рисуется в RenderViewport поверх).
    auto lightView = reg.view<LightComponent, Transform, IdComponent>();
    for (auto e : lightView) {
        bool selected = isSel(lightView.get<IdComponent>(e).Id);
        const LightComponent& lc = lightView.get<LightComponent>(e);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, selected ? 0.30f : 0.25f, glm::vec3(lc.Color) * (selected ? 1.2f : 0.9f), 10);
    }
    // Эмиттеры частиц: маркер в позиции.
    auto fxView = reg.view<ParticleEmitterComponent, Transform, IdComponent>();
    for (auto e : fxView) {
        bool selected = isSel(fxView.get<IdComponent>(e).Id);
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.85f, 0.3f) : glm::vec3(0.9f, 0.6f, 0.3f);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, 0.18f, color, 8);
        m_debugDraw->Axes(glm::translate(glm::mat4(1.0f), wpos), 0.4f);
    }
    // Зонды отражений: сфера-маркер и КОРОБКА ВЛИЯНИЯ. Коробка важнее маркера —
    // именно она отвечает на вопрос «какой зонд подхватит камера здесь», а
    // ответить на него по числам в инспекторе нельзя.
    auto probeView = reg.view<ReflectionProbeComponent, Transform, IdComponent>();
    for (auto e : probeView) {
        const bool selected = isSel(probeView.get<IdComponent>(e).Id);
        const ReflectionProbeComponent& rp = probeView.get<ReflectionProbeComponent>(e);
        const glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        // Не снятый зонд — тускло-красный: «здесь будет отражение, но его ещё
        // нет». Молчащий маркер выглядел бы как рабочий зонд.
        const glm::vec3 color = rp.Dirty ? glm::vec3(0.85f, 0.35f, 0.30f)
                                         : (selected ? glm::vec3(0.45f, 0.95f, 1.0f)
                                                     : glm::vec3(0.30f, 0.65f, 0.75f));
        m_debugDraw->WireSphere(wpos, selected ? 0.34f : 0.26f, color, 12);
        if (selected || rp.Dirty) m_debugDraw->WireBox(wpos, rp.BoxHalfExtents, color * 0.8f);
    }

    // Коллайдеры: каркас формы в МИРОВОМ трансформе сущности.
    //
    // Раньше здесь стояли tr.Position и tr.Scale — то есть ЛОКАЛЬНЫЙ трансформ
    // и ни следа поворота. Получался осевой ящик, который у повёрнутого объекта
    // висит сам по себе, а у дочерней сущности ещё и не в том месте. Это не
    // косметика: гизмо — единственный способ увидеть, где на самом деле форма
    // столкновения, и врущее гизмо хуже отсутствующего — по нему принимают
    // решения.
    auto colView = reg.view<ColliderComponent, Transform, IdComponent>();
    for (auto e : colView) {
        const ColliderComponent& col = colView.get<ColliderComponent>(e);
        bool selected = isSel(colView.get<IdComponent>(e).Id);
        glm::vec3 color = selected ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(0.25f, 0.55f, 0.30f);

        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 wpos = glm::vec3(world[3]);
        // Масштаб — длины столбцов мировой матрицы: он мог прийти и от родителя.
        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                              glm::length(glm::vec3(world[2])));
        const float uniform = glm::max(scale.x, glm::max(scale.y, scale.z));
        // Мировой поворот без масштаба: масштаб уже учтён в размерах формы, и
        // умножать на него второй раз значило бы раздувать каркас квадратично.
        glm::mat4 rot = world;
        for (int i = 0; i < 3; ++i) {
            const float len = scale[i] > 1e-6f ? scale[i] : 1.0f;
            rot[i] /= len;
        }
        rot[3] = glm::vec4(wpos, 1.0f);

        switch (col.Shape) {
            case sage::physics::ShapeType::Box:
                m_debugDraw->WireBox(rot * glm::scale(glm::mat4(1.0f),
                                                      col.HalfExtents * scale * 2.0f),
                                     color);
                break;
            case sage::physics::ShapeType::Sphere:
                // Шар поворачивать незачем — он от поворота не меняется.
                m_debugDraw->WireSphere(wpos, col.Radius * uniform, color);
                break;
            case sage::physics::ShapeType::Capsule:
                m_debugDraw->WireCapsule(rot, col.Radius * uniform, col.HalfHeight * scale.y,
                                         color);
                break;
        }

        // Составная форма: каждая часть в своём локальном смещении и повороте.
        // Без этого у молотка или Т-образной детали гизмо показывало ОДИН ящик
        // по полям, которые физика в этом случае вообще игнорирует.
        for (const ColliderComponent::Part& p : col.Parts) {
            const glm::mat4 partWorld =
                rot * glm::translate(glm::mat4(1.0f), p.Offset * scale) *
                glm::eulerAngleXYZ(glm::radians(p.EulerDeg.x), glm::radians(p.EulerDeg.y),
                                   glm::radians(p.EulerDeg.z));
            switch (p.Shape) {
                case sage::physics::ShapeType::Box:
                    m_debugDraw->WireBox(
                        partWorld * glm::scale(glm::mat4(1.0f), p.HalfExtents * scale * 2.0f),
                        color * 0.8f);
                    break;
                case sage::physics::ShapeType::Sphere:
                    m_debugDraw->WireSphere(glm::vec3(partWorld[3]), p.Radius * uniform,
                                            color * 0.8f);
                    break;
                case sage::physics::ShapeType::Capsule:
                    m_debugDraw->WireCapsule(partWorld, p.Radius * uniform, p.HalfHeight * scale.y,
                                             color * 0.8f);
                    break;
            }
        }
    }
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
    EnsureFramebuffer(primary ? m_sceneFbo : m_extraFbo[slot], w, h, samples);
    EnsureFramebuffer(primary ? m_postFbo : m_extraPostFbo[slot], w, h);
    Framebuffer& sceneFbo = primary ? *m_sceneFbo : *m_extraFbo[slot];
    Framebuffer& postFbo = primary ? *m_postFbo : *m_extraPostFbo[slot];

    // Режим вёрстки с непрозрачным холстом — это ПЛОСКИЙ РАБОЧИЙ СТОЛ, а не
    // сцена с плёнкой поверх. Пока сцена рисовалась под полупрозрачной
    // подложкой, она проступала сквозь неё: мир выглядел залитым чернотой
    // (объекты «почернели»), а глаз цеплялся за геометрию вместо кнопок.
    // Здесь трёхмерный проход просто не выполняется — заодно вёрстка перестаёт
    // стоить полного кадра сцены.
    const bool flatCanvas = primary && m_drawUIOverlay && m_uiBackdrop >= 0.999f;

    sceneFbo.Bind();
    device.SetClearColor(flatCanvas ? 0.16f : 0.10f, flatCanvas ? 0.17f : 0.11f,
                         flatCanvas ? 0.20f : 0.13f, 1.0f);
    device.Clear();

    // Ортогональные виды приходят готовыми матрицами: у них нет свободной
    // камеры, а есть плоскость, центр и масштаб. Матрицы нужны вызывающему в
    // любом случае — по ним он считает пикинг и гизмо.
    outView = viewOverride.Use ? viewOverride.View : camera.GetViewMatrix();
    outProj = viewOverride.Use ? viewOverride.Proj
                           : camera.GetProjectionMatrix((float)w / (float)std::max(h, 1));
    const glm::vec3 eye = viewOverride.Use ? viewOverride.EyePos : camera.Position;

    if (!flatCanvas) DrawSky(env, outView, outProj);

    // Режим рендера из тулбара: Shaded(0)/Unlit(1)/Normals(2); Wireframe — unlit + линии.
    int shadingMode = 0;
    bool wireframe = false;
    switch (mode) {
        case EditorRenderMode::Shaded:    shadingMode = 0; break;
        case EditorRenderMode::Wireframe: shadingMode = 1; wireframe = true; break;
        case EditorRenderMode::Unlit:     shadingMode = 1; break;
        case EditorRenderMode::Normals:   shadingMode = 2; break;
    }
    if (!flatCanvas) {
        DrawLit(scene, env, outView, outProj, eye, shadingMode, wireframe);
        m_particles->Draw(camera, outView, outProj);
    }

    GameObject selectedObj = scene.Get(selectedId);

    // Гизмо-графика (DebugDraw) — в тот же буфер, с тестом глубины (объекты заслоняют сетку).
    if (showGrid && !flatCanvas) m_debugDraw->Grid({0.0f, 0.0f, 0.0f}, 12.0f, 1.0f, {0.32f, 0.33f, 0.38f});
    if (!flatCanvas) DrawEntityGizmos(scene, selection, (float)m_gameW / (float)std::max(m_gameH, 1));
    if (selectedObj.Valid() && !flatCanvas) {
        glm::mat4 world = scene.WorldMatrix(selectedObj.Entity());
        m_debugDraw->Axes(world, 1.4f);
        if (const LightComponent* light = scene.Registry().try_get<LightComponent>(selectedObj.Entity())) {
            glm::vec3 wpos = glm::vec3(world[3]);
            glm::vec3 lightColor = glm::vec3(light->Color) * 0.9f;
            if (light->Kind == LightComponent::Type::Spot) {
                glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
                m_debugDraw->WireCone(wpos, dir, light->Range, light->OuterConeDeg, lightColor);
            } else {
                m_debugDraw->WireSphere(wpos, light->Range, lightColor);
            }
        }
    }
    // Габариты выделенного — ровно та коробка, по которой считается попадание
    // мышью (sage::render::RayMesh). Показывать её незачем всегда, но когда
    // «клик выбрал не то» непонятен, увидеть её — самый короткий ответ.
    if (m_showBounds && !flatCanvas) {
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

    m_debugDraw->Flush(outView, outProj);

    // Объём — после геометрии и до пост-обработки: свечение лучей должно
    // попасть в bloom. В плоском холсте вёрстки его нет — там нет и сцены.
    if (!flatCanvas && cfg.Volumetrics && cfg.PostProcessing) {
        sceneFbo.Resolve();
        if (!m_volumetrics) m_volumetrics.emplace();
        m_volumetrics->Render(sceneFbo, sceneFbo.DepthTexture(), w, h, outProj, outView, eye, env,
                              FrameShadows(), sage::render::VolumetricsFromConfig(cfg),
                              m_sceneTime);
    }

    // Игровой интерфейс поверх сцены — В ТОМ ЖЕ буфере и в тех же пикселях, в
    // которых его увидит игрок. Раскладка UI зависит от размера экрана (якоря,
    // проценты), поэтому рисовать его надо именно в размер вьюпорта, а не в
    // какой-нибудь эталонный: иначе редактор показывал бы не то, что получится.
    if (m_drawUIOverlay && primary) {
        // Рисуем ВСЕГДА, а не только когда в сцене уже есть элементы: пустой
        // холст — нормальное начало работы, и человек должен видеть, куда
        // класть первый элемент, а не смотреть в трёхмерную сцену.
        {
            if (!m_ui) m_ui = std::make_unique<UIRenderer>();
            device.SetViewport(0, 0, w, h);

            // РАЗМЕР ЭКРАНА НЕ МЕНЯЕТСЯ — меняется только показ.
            //
            // Это важнее, чем кажется. Экран игры (w x h) — то, подо что
            // сверстан интерфейс: от него считаются якоря, растяжения и
            // проценты. Возьми мы «экран побольше, чтобы влезло поле», вёрстка
            // пересчиталась бы под другое разрешение — и человек правил бы не
            // ту раскладку, которую увидит игрок. Поэтому экран прежний, а
            // масштаб уходит в матрицу вывода: холст занимает середину кадра, а
            // по краям остаётся поле, где ВИДНО всё, что вышло за границу.
            const int sw = w, sh = h;
            const glm::vec2 origin(((float)w - (float)w * m_uiZoom) * 0.5f + m_uiPan.x,
                                   ((float)h - (float)h * m_uiZoom) * 0.5f + m_uiPan.y);

            // Рабочий стол вёрстки: тёмное поле вокруг и СВЕТЛЕЕ внутри
            // прямоугольника экрана. Без этой пары человек не видит, где
            // кончается экран игры, — а именно по его границам считаются
            // якоря, и элемент, «случайно» уехавший за край, на глаз ничем не
            // отличается от стоящего у края.
            //
            // При Backdrop < 1 (правят худ поверх игры) полупрозрачная плёнка
            // остаётся: там сцена под интерфейсом и нужна.
            if (m_uiBackdrop > 0.0f) {
                m_ui->Begin(w, h);
                if (m_uiBackdrop < 0.999f) {
                    m_ui->Rect(0.0f, 0.0f, (float)w, (float)h, glm::vec3(0.10f, 0.11f, 0.13f),
                               m_uiBackdrop);
                } else {
                    const float sx = ((float)w - (float)w * m_uiZoom) * 0.5f + m_uiPan.x;
                    const float sy = ((float)h - (float)h * m_uiZoom) * 0.5f + m_uiPan.y;
                    const float sw2 = (float)w * m_uiZoom, sh2 = (float)h * m_uiZoom;
                    // Экран игры и рамка вокруг него.
                    m_ui->Rect(sx, sy, sw2, sh2, glm::vec3(0.115f, 0.125f, 0.15f), 1.0f);
                    const float b = 1.0f;
                    const glm::vec3 edge(0.42f, 0.47f, 0.56f);
                    m_ui->Rect(sx - b, sy - b, sw2 + 2.0f * b, b, edge, 1.0f);
                    m_ui->Rect(sx - b, sy + sh2, sw2 + 2.0f * b, b, edge, 1.0f);
                    m_ui->Rect(sx - b, sy, b, sh2, edge, 1.0f);
                    m_ui->Rect(sx + sw2, sy, b, sh2, edge, 1.0f);
                    // Осевые линии по центру: по ним ставят то, что должно быть
                    // ровно посередине, и промах в один пиксель виден сразу.
                    const glm::vec3 mid(0.24f, 0.27f, 0.33f);
                    m_ui->Rect(sx + sw2 * 0.5f, sy, 1.0f, sh2, mid, 1.0f);
                    m_ui->Rect(sx, sy + sh2 * 0.5f, sw2, 1.0f, mid, 1.0f);
                }
                m_ui->End();
            }

            m_ui->Begin(sw, sh);
            m_ui->SetView(origin, m_uiZoom, w, h);
            sage::ui::DrawSceneUI(scene, *m_ui, sw, sh);
            m_ui->End();
        }
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
    bool postApplied = false;
    if (cfg.PostProcessing && mode == EditorRenderMode::Shaded && !viewOverride.Use) {
        postFbo.Resize(w, h);
        m_postfx->Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), sceneFbo.Width(),
                         sceneFbo.Height(), outProj, outView, FxFromConfig(cfg),
                         /*output=*/&postFbo, 0, 0, w, h);
        postApplied = true;
    }
    if (primary) m_postApplied = postApplied;
    else m_extraPostApplied[slot] = postApplied;

    // Кайма выделения — поверх ИТОГОВОГО кадра (после поста, постоянная ширина
    // в пикселях, крайне читаемая независимо от размера объекта и дистанции).
    if (!selection.empty()) CompositeOutline(postApplied ? postFbo : sceneFbo);

    device.BindDefaultFramebuffer();
}

void EditorSceneRenderer::RenderGame(Scene& scene, const LightingEnvironment& env, const sage::EngineConfig& cfg) {
    // Кадр от Primary-камеры сцены — ЧЕРЕЗ ТОТ ЖЕ ХЕЛПЕР, что и собранная игра
    // (sage::ecs::PrimaryCameraFrame): превью Game-панели = реальный вид в игре.
    // Нет Primary-камеры — кадр не рисуем (панель Game покажет подсказку).
    float aspect = (float)m_gameW / (float)std::max(m_gameH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(scene, aspect);
    if (!frame.HasPrimary) { m_gamePostApplied = false; return; }

    const glm::mat4& view = frame.View;
    const glm::mat4& proj = frame.Proj;
    glm::vec3 camPos = frame.Position;

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    EnsureFramebuffer(m_gameFbo, m_gameW, m_gameH, sage::render::SceneSamples(cfg));
    m_gameFbo->Bind();
    device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
    device.Clear();

    DrawSky(env, view, proj);
    // Игровое окно — всегда Shaded, без гизмо (как увидит игрок).
    DrawLit(scene, env, view, proj, camPos, /*shadingMode=*/0, /*wireframe=*/false);
    m_particles->DrawFromView(view, proj);

    m_gameFbo->Resolve();   // MSAA -> обычные текстуры (без MSAA — пустышка)

    m_gamePostApplied = false;
    if (cfg.PostProcessing) {
        m_gamePostFbo->Resize(m_gameW, m_gameH);
        m_gamePostfx->Render(m_gameFbo->ColorTexture(), m_gameFbo->DepthTexture(),
                             m_gameFbo->Width(), m_gameFbo->Height(), proj, view,
                             FxFromConfig(cfg),
                             /*output=*/&*m_gamePostFbo, 0, 0, m_gameW, m_gameH);
        m_gamePostApplied = true;
    }

    // UI сцены (компоненты интерфейса) — поверх ИТОГОВОЙ картинки (после поста),
    // ровно как его увидит игрок в собранной игре (WYSIWYG панели Game).
    auto uiView = scene.Registry().view<sage::ui::Transform>();
    if (uiView.begin() != uiView.end()) {
        if (!m_ui) m_ui = std::make_unique<UIRenderer>();
        Framebuffer& target = m_gamePostApplied ? *m_gamePostFbo : *m_gameFbo;
        target.Bind();
        device.SetViewport(0, 0, m_gameW, m_gameH);
        m_ui->Begin(m_gameW, m_gameH);
        sage::ui::DrawSceneUI(scene, *m_ui, m_gameW, m_gameH);
        m_ui->End();
    }
    device.BindDefaultFramebuffer();
}

void EditorSceneRenderer::SetViewportSize(int slot, int w, int h) {
    if (slot <= 0) { m_vpW = w; m_vpH = h; return; }
    if (slot >= kMaxViews) return;
    m_extraW[slot] = w;
    m_extraH[slot] = h;
}

uint64_t EditorSceneRenderer::ViewportTexture(int slot) const {
    if (slot <= 0) return ViewportTexture();
    if (slot >= kMaxViews || !m_extraFbo[slot]) return 0;
    return m_extraPostApplied[slot] && m_extraPostFbo[slot]
               ? m_extraPostFbo[slot]->NativeColorTexture()
               : m_extraFbo[slot]->NativeColorTexture();
}

uint64_t EditorSceneRenderer::ViewportTexture() const {
    return m_postApplied && m_postFbo ? m_postFbo->NativeColorTexture()
                                      : m_sceneFbo->NativeColorTexture();
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
