#include "sage/app/CoreSystems.h"
#include "PlayerLayer.h"

#include "sage/audio/AudioSystem.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/DebugView.h"
#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "sage/app/Application.h"
#include "sage/core/Config.h"
#include "sage/core/Paths.h"
#include "sage/assets/SaveGame.h"
#include "sage/scene/Prefab.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/ecs/DecalSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/Screenshot.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneFlow.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"
#include "sage/ecs/CameraView.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace fs = std::filesystem;

// Кадр собранной игры: тени, проходы сцены, интерфейс, снимок (см. PlayerLayer.h).

ShadowBinding PlayerLayer::FrameShadows(bool sunEnabled) const {
    ShadowBinding b(*m_shadows, sunEnabled);
    if (m_localShadows) b.Local = m_localShadows->Binding();
    return b;
}

void PlayerLayer::OnRender() {
    // Отказ рисуется ВМЕСТО кадра: сцены нет, рисовать нечего, а окно обязано
    // объяснить, почему оно пустое.
    if (Failed()) { DrawFatalScreen(); return; }
    if (!m_scene) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    sage::rhi::GraphicsDevice& device = app.Device();

    // Наклейки: пересобираются только помеченные (см. DecalComponent::Dirty),
    // поэтому вызов каждый кадр стоит обхода их списка и ничего больше. Здесь,
    // до сбора кадра, чтобы поставленная скриптом наклейка попала в ЭТОТ кадр,
    // а не появилась через один.
    sage::ecs::BuildDecals(*m_scene, sage::ecs::MakeDecalMesh);

    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
    // Флаги качества из конфига переопределяют настройки сцены.
    //
    // НО СВЕТ ПРИ ЭТОМ ОСТАЁТСЯ. Настройка качества выключает ОТРИСОВКУ неба, а
    // не солнце над миром: «поставил пресет полегче — мир погас» было бы не
    // оптимизацией, а поломкой. Поэтому посчитанный от неба ambient
    // замораживается своими значениями, и дальше сцена освещена ровно так же,
    // как при включённом небе. (Автор, выключивший небо В СЦЕНЕ, получает
    // темноту намеренно — см. LightingEnvironment::ResolveAmbient.)
    if (!cfg.Skybox && env.Skybox.Enabled) {
        glm::vec3 ambientSky, ambientGround;
        env.ResolveAmbient(ambientSky, ambientGround);
        env.SkyColor = ambientSky;
        env.GroundColor = ambientGround;
        env.AmbientMode = LightingEnvironment::AmbientSource::Custom;
        env.Skybox.Enabled = false;
    }
    if (!cfg.Fog) env.Fog.Enabled = false;

    // --- Соотношение сторон: letterbox-viewport по центру окна (или весь экран) ---
    int vpX, vpY, vpW, vpH;
    cfg.LetterboxViewport(window.Width(), window.Height(), vpX, vpY, vpW, vpH);

    // --- Камера: Primary-CameraComponent сцены либо запасной вид ---
    // Кадр берём ТЕМ ЖЕ хелпером, что и панель Game редактора — вид в собранной
    // игре побайтово совпадает с превью (никакой «фальшивости» превью↔игра).
    glm::mat4 view, proj;
    glm::vec3 viewPos;
    float aspect = (float)vpW / (float)std::max(vpH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(*m_scene, aspect);
    if (frame.HasPrimary) {
        view = frame.View;
        proj = frame.Proj;
        viewPos = frame.Position;
    } else {
        // Явно предупреждаем (один раз): без Primary-камеры игра показывает
        // ЗАПАСНОЙ вид — и он НАРОЧНО не совпадает с редакторской камерой, чтобы
        // «нет камеры» нельзя было спутать с нормальным игровым видом.
        if (!m_warnedNoCamera) {
            LOG_WARN("Player") << "В сцене нет Primary-камеры (CameraComponent) — показываю "
                                  "запасной вид. Добавьте камеру: Entity > Create Camera в редакторе.";
            m_warnedNoCamera = true;
        }
        view = m_fallbackCamera.GetViewMatrix();
        proj = m_fallbackCamera.GetProjectionMatrix(aspect);
        viewPos = m_fallbackCamera.Position;
    }

    // Слушатель звука — на игровой камере: 3D-звуки (PlaySound3D из Lua) должны
    // приходить с той стороны, куда игрок реально смотрит. Базис берём из
    // матрицы вида по той же причине, что и ниже для теней — камера может быть
    // как компонентом сцены, так и запасной.
    if (m_audio) {
        const glm::mat3 basis = glm::mat3(view);
        glm::vec3 forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
        glm::vec3 up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
        m_audio->SetListener(viewPos, forward, up);
    }

    // --- Кадр как ГРАФ ПРОХОДОВ ---------------------------------------------
    //
    // Проходы объявляются данными (что читают, во что пишут), а не порядком
    // вызовов. Даёт это три вещи, каждая из которых работает уже сегодня:
    //
    //   • Проход, результат которого никому не нужен, НЕ ВЫПОЛНЯЕТСЯ. Раньше
    //     это были ручные `if` перед каждым захватом; теперь условие живёт в
    //     описании кадра, и забыть его негде.
    //   • Описание проверяется ДО отрисовки: прочитать то, во что никто не
    //     писал, — ошибка кадра, и находится она сообщением, а не чёрным
    //     экраном.
    //   • На вопрос «что и почему было в кадре» отвечает Describe().
    //
    // Порядок при этом задаёт человек — граф его не переставляет (см.
    // FrameGraph.h): он проверяет, что порядок осмыслен, и выбрасывает лишнее.
    m_frame.Reset();
    const auto rShadow = m_frame.DeclareResource("ShadowMap");
    const auto rEnv = m_frame.DeclareResource("EnvCube");
    const auto rPlanar = m_frame.DeclareResource("PlanarMirror");
    const auto rScreen = m_frame.DeclareResource("Screen");

    const bool wantReflections = m_scene->Reflections.Enabled && cfg.Reflections;
    const bool wantPlanar =
        wantReflections && m_scene->Reflections.PlanarEnabled && cfg.PlanarReflections;

    // Отладочный вид кадра (SAGE_DEBUG_VIEW / настройка debugView): показать
    // одну величину вместо освещения. В игре он нужен не меньше, чем в
    // редакторе, — «в редакторе нормально, в игре тёмно» разбирают именно так.
    sage::render::DebugView debugView = sage::render::DebugView::None;
    if (!sage::render::ParseDebugView(cfg.DebugView.c_str(), debugView) &&
        !cfg.DebugView.empty() && cfg.DebugView != "none") {
        static std::string said;
        if (said != cfg.DebugView) {
            said = cfg.DebugView;
            LOG_WARN("Render") << "неизвестный debugView '" << cfg.DebugView << "'";
        }
    }
    const bool debugging = debugView != sage::render::DebugView::None;

    // --- Отражения: карта окружения ---
    // Строго ДО прохода теней и сцены: захват меняет привязанный буфер и
    // viewport, и посреди кадра это стоило бы лишних переключений. Пересъёмка
    // происходит только при смене цвета неба.
    {
        sage::render::RenderPassDesc pass{"Отражения: окружение"};
        pass.Writes = {rEnv};
        pass.Enabled = wantReflections;
        pass.Execute = [&, this] {
        m_reflections.SetEnabled(wantReflections);
        m_reflections.SetIntensity(m_scene->Reflections.Intensity);
        if (m_sky) {
            // Настоящее небо сцены (набор граней), если оно задано, — иначе
            // отражение показывало бы градиент вместо того, что видно в кадре.
            std::shared_ptr<Skybox> skyAsset = sage::render::SceneSkyCubemap(env);
            m_reflections.UpdateSky(*m_sky, env, skyAsset.get());
        }

        // Зонды сцены: не больше одного за кадр (шесть проходов геометрии).
        if (wantReflections) {
            sage::render::UpdateReflectionProbes(
                *m_scene,
                [&](const glm::mat4& v, const glm::mat4& p) {
                    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    device.Clear(true, true);
                    if (m_sky && env.Skybox.Enabled)
                        sage::render::DrawSceneSky(*m_sky, env, v, p, m_sceneTime);
                    sage::render::SceneColorInput c;
                    c.View = v;
                    c.Proj = p;
                    c.ViewPos = glm::vec3(glm::inverse(v)[3]);
                    c.Env = &env;
                    c.Shadows = FrameShadows(cfg.Shadows);
                    c.Time = m_sceneTime;
                    sage::render::RenderSceneColor(*m_scene, m_batch, c);
                },
                1);
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Тени: глубина от солнца (можно отключить в настройках) ---
    // Строго ПОСЛЕ выбора камеры: каскады делят дальность именно её взгляда, и
    // без неё считать их не из чего. Порядок «тени, потом камера» держался
    // только на том, что одной карте камера была не нужна.
    {
        sage::render::RenderPassDesc pass{"Тени: глубина"};
        pass.Writes = {rShadow};
        // Тени ламп НЕ привязаны к солнечным: сцена без солнца (подвал, ночь,
        // интерьер) — самое место для теней от прожекторов, и выключать их
        // заодно с солнцем значило бы отнимать тени ровно там, где кроме них
        // ничего нет.
        pass.Enabled = cfg.Shadows || m_localShadows.has_value();
        pass.Execute = [&, this] {
        if (cfg.Shadows) {
            ShadowMap::CameraView v;
            v.Position = viewPos;
            // Направление и «верх» достаём из матрицы вида: камера здесь может быть
            // как компонентом сцены, так и запасной, и общего объекта Camera у них
            // нет.
            const glm::mat3 basis = glm::mat3(view);
            v.Forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
            v.Up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
            // Угол обзора и ближнюю плоскость восстанавливаем из матрицы проекции —
            // по той же причине.
            v.FovY = 2.0f * std::atan(1.0f / proj[1][1]);
            v.Aspect = aspect;
            v.Near = proj[3][2] / (proj[2][2] - 1.0f);
            // Дальность теней: сцена главнее настроек. Сто двадцать метров на
            // мир в тридцать метров — это карта теней, четыре пятых которой
            // ушли в пустоту, и оторванная от предмета тень (см.
            // sage::ShadowSettings).
            v.ShadowDistance =
                env.Shadows.Distance > 0.0f ? env.Shadows.Distance : cfg.ShadowDistance;
            // Одна карта — это тоже карта ВОКРУГ КАМЕРЫ, а не вокруг начала мира:
            // раньше здесь стоял ортобокс радиусом 24 м в точке (0,0,0), и всё, что
            // игра успевала отплыть или отойти от неё, оставалось без теней —
            // молча, потому что это выглядит как «тени просто выключены», а не как
            // ошибка.
            if (m_shadows->CascadeCount() > 1) m_shadows->SetCascades(env.Sun.Direction, v);
            else m_shadows->FitSingle(env.Sun.Direction, v);
            sage::render::RenderShadowDepth(*m_shadows, *m_scene, m_batch, window.Width(),
                                            window.Height());
        }
        if (m_localShadows) {
            m_localShadows->Prepare(env);
            sage::render::RenderLocalShadowDepth(*m_localShadows, *m_scene, m_batch,
                                                 window.Width(), window.Height());
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Плоское отражение (вода, зеркало) ---
    // Сцена рисуется второй раз зеркально относительно плоскости. Внутри этого
    // прохода плоского отражения НЕТ: иначе зеркало смотрело бы в себя, и
    // каждый кадр стоил бы вдвое дороже предыдущего.
    m_planar.Reset();
    {
        // Зеркальный проход ЧИТАЕТ тени и карту окружения: отражённая сцена
        // освещается тем же светом, что и прямая.
        sage::render::RenderPassDesc pass{"Плоское отражение"};
        pass.Reads = {rShadow, rEnv};
        pass.Writes = {rPlanar};
        pass.Enabled = wantPlanar;
        pass.Execute = [&, this] {
            if (wantPlanar) {
            const glm::vec4 plane = m_scene->Reflections.Plane;
            m_planar.Capture(plane, view, proj, vpW, vpH,
                             [&](const glm::mat4& mv, const glm::mat4& mp) {
                                 const glm::vec3 mirrorEye =
                                     glm::vec3(glm::inverse(mv)[3]);
                                 if (m_sky && env.Skybox.Enabled)
                                     sage::render::DrawSceneSky(*m_sky, env, mv, mp, m_sceneTime);
                                 sage::render::SceneColorInput rc;
                                 rc.View = mv;
                                 rc.Proj = mp;
                                 rc.ViewPos = mirrorEye;
                                 rc.Env = &env;
                                 rc.Shadows = FrameShadows(cfg.Shadows);
                                 rc.Time = m_sceneTime;
                                 rc.Reflection = m_reflections.Binding(vpW, vpH);
                                 rc.Reflection.CapturingPlanar = true;
                                 sage::render::RenderSceneColor(*m_scene, m_batch, rc);
                             });
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // --- Основной проход: полное освещение + тени + туман/скайбокс ---
    // Полосы letterbox чёрные: сперва чистим весь экран, затем рендерим в
    // центральный viewport нужного соотношения.
    {
        // Главный проход читает ВСЁ, что насчитали предыдущие. Именно эти
        // связи и делают отбрасывание лишнего осмысленным: выключи отражения —
        // и проходы, которые их готовили, перестанут выполняться сами, потому
        // что их результат больше никто не читает.
        sage::render::RenderPassDesc pass{"Сцена: освещение, тени, отражения"};
        pass.Reads = {rShadow};
        if (wantReflections) pass.Reads.push_back(rEnv);
        if (wantPlanar) pass.Reads.push_back(rPlanar);
        pass.Writes = {rScreen};
        pass.ColorLoad = sage::render::LoadOp::Clear;
        pass.ClearColor = glm::vec4(env.SkyColor * 0.9f, 1.0f);
        pass.Execute = [&, this] {
        // Пост-обработка: сцена уходит в HDR-буфер, а на экран — результат
        // цепочки. Тот же PostFX и те же настройки из конфига, что у окна Game
        // в редакторе, — иначе превью и игра показывают разное.
        //
        // В отладочном виде пост-обработка ВЫКЛЮЧЕНА, и это не экономия.
        // Тон-маппинг, гамма, виньетка и насыщенность придуманы для картинки, а
        // здесь на экране не картинка, а величина: 0.2 шероховатости обязаны
        // остаться 0.2, иначе вид отвечает не на тот вопрос, ради которого его
        // включили. Кадр глубины после ACES и гаммы становился почти белым.
        // Пост-обработка ПРОВЕРЯЕТСЯ на этой видеокарте (см. PostFX::CheckPipeline).
        // Её отказ выглядит как чёрный экран при живом кадре — с игрой это ещё
        // хуже, чем с редактором: там хотя бы есть отладочные виды, а здесь
        // человек видит просто чёрное окно. Лучше кадр без свечения, чем ничего.
        // ТРАКТ БЕРЁТСЯ У КАМЕРЫ. Нет компонента «Пост-обработка» или он
        // выключен — кадр выводится базовым трактом (кривая и кодирование для
        // экрана, см. FramePostChain): эффектов нет, но картинка правильная.
        // Сырой HDR с одной аппаратной sRGB-записью пересвечивался и выглядел
        // кислотным — кривой, сжимающей света, у него не было.
        const sage::render::PostChain cameraChain =
            sage::render::FramePostChain(*m_scene, sage::ecs::PrimaryCameraEntity(*m_scene));

        bool postOk = true;
        if (m_postfx) {
            const sage::render::PostFX::SelfCheck& check = m_postfx->CheckPipeline();
            postOk = !check.Ran || check.Ok;
            if (!postOk && !m_postWarned) {
                m_postWarned = true;
                LOG_ERROR("Player") << "Пост-обработка на этой видеокарте не работает ("
                                    << check.Reason << ") — кадр без эффектов";
            }
        }
        const bool usePost = !debugging && postOk;
        if (usePost) {
            if (!m_postfx) m_postfx.emplace();
            // Число сэмплов — по конфигу и той же функцией, что у редактора:
            // MSAA работает на буфере СЦЕНЫ (в нём растеризуется геометрия), а
            // не на экранном, куда уходит уже готовая картинка.
            EnsureFramebuffer(m_sceneFbo, vpW, vpH, sage::render::SceneSamples(cfg),
                              sage::render::SceneWantsGBuffer(cfg));
            m_sceneFbo->Bind();
            device.SetViewport(0, 0, vpW, vpH);
        } else if (vpX != 0 || vpY != 0) {
            device.SetViewport(0, 0, window.Width(), window.Height());
            device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            device.Clear();
        }
        if (!usePost) device.SetViewport(vpX, vpY, vpW, vpH);
        const glm::vec3 clear = sage::render::SceneClearColor(env);
        device.SetClearColor(clear.r, clear.g, clear.b, 1.0f);
        device.Clear();

        // Аппаратная гамма-коррекция нужна ТОЛЬКО когда сцена идёт прямо в
        // экран: иначе линейный цвет шейдеров уходит на монитор сырым и игра
        // выглядит неоправданно тёмной. С пост-обработкой гамму накладывает
        // composite-проход, и включать её здесь значило бы применить дважды.
        device.SetSRGBWrite(!usePost);

        // Съёмка сцены в именованные картинки (иконки, экраны, зеркала) — до
        // основного прохода: показать их собираются в этом же кадре.
        if (sage::render::RenderTextureViews(*m_scene, m_batch, env, m_sceneTime) > 0 && usePost) {
            m_sceneFbo->Bind();
            device.SetViewport(0, 0, vpW, vpH);
        }

        if (env.Skybox.Enabled) {
            sage::render::DrawSceneSky(*m_sky, env, view, proj, m_sceneTime);
        }

        // Статика — через RenderBatch: отсечение по фрустуму + инстансный батчинг.
        sage::render::SceneColorInput color;
        color.View = view;
        color.Proj = proj;
        color.ViewPos = viewPos;
        color.Env = &env;
        color.Shadows = FrameShadows(cfg.Shadows);
        color.OcclusionCulling = cfg.OcclusionCulling;
        color.Time = m_sceneTime;
        color.Reflection = m_reflections.Binding(
            vpW, vpH, wantPlanar ? m_planar.Texture() : sage::rhi::TextureHandle{});
        if (wantReflections) {
            // Зонд — каждому объекту по его положению (см. ReflectionProbeSet).
            m_probeSet = sage::render::CollectReflectionProbes(*m_scene);
            color.Reflection.Probes = &m_probeSet;
        }
        color.ShadingMode = (int)debugView;
        sage::render::RenderSceneColor(*m_scene, m_batch, color);
        // Скорости — сразу после геометрии (см. VelocityBuffer).
        sage::rhi::TextureHandle velocity;
        if (usePost && sage::render::ChainNeedsVelocity(cameraChain)) {
            velocity = m_velocity.Render(m_batch, view, proj, m_sceneFbo->Width(),
                                         m_sceneFbo->Height());
            m_sceneFbo->Bind();
            device.SetViewport(0, 0, m_sceneFbo->Width(), m_sceneFbo->Height());
        }

        // Частицы (billboard) — camRight/Up берём из матрицы вида.
        if (m_particles) m_particles->DrawFromView(view, proj);
        // Спрайты-маркеры — тем же способом и сразу после частиц: и те и
        // другие полупрозрачные и не пишут глубину, поэтому идут после всей
        // непрозрачной геометрии и до пост-обработки.
        if (m_billboards) m_billboards->DrawFromView(view, proj);

        // Отладочная графика игры (Debug:DrawLine из скриптов) — здесь же, в
        // буфере сцены: с тестом глубины, то есть заслоняется миром, как и
        // должна. В СОБРАННОЙ игре она нужна не меньше, чем в редакторе:
        // «в редакторе работает, в игре нет» разбирают именно ею.
        if (m_scripting && !m_scripting->Debug().Empty()) {
            if (!m_debugDraw) m_debugDraw.emplace();
            for (const sage::render::DebugLine& l : m_scripting->Debug().All())
                m_debugDraw->Line(l.A, l.B, l.Color);
            m_debugDraw->Flush(view, proj);
        }

        // Объём — после геометрии и ДО пост-обработки: лучи обязаны попасть в
        // bloom, иначе солнце светится, а его лучи нет. Работает только с
        // буфером сцены: без пост-обработки читать глубину неоткуда.
        if (usePost && cfg.Volumetrics) {
            m_sceneFbo->Resolve();
            if (!m_volumetrics) m_volumetrics.emplace();
            m_volumetrics->Render(*m_sceneFbo, m_sceneFbo->DepthTexture(), m_sceneFbo->Width(),
                                  m_sceneFbo->Height(), proj, view, viewPos, env,
                                  color.Shadows, sage::render::VolumetricsFromConfig(cfg),
                                  m_sceneTime);
        }

        if (usePost) {
            m_sceneFbo->Resolve(); // MSAA -> обычные текстуры (без MSAA — пустышка)
            device.BindDefaultFramebuffer();
            // Полосы letterbox чистим здесь: цепочка пишет только в свой
            // прямоугольник, и без этого по краям осталось бы содержимое
            // прошлого кадра.
            device.SetViewport(0, 0, window.Width(), window.Height());
            device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            device.Clear();
            // Солнце — звену «Lens Flare» (блик — часть тракта камеры).
            m_postfx->SetLighting(&env);
            m_postfx->SetGBuffer(m_sceneFbo->AlbedoTexture(), m_sceneFbo->NormalTexture());
            m_postfx->Render(m_sceneFbo->ColorTexture(), m_sceneFbo->DepthTexture(),
                             m_sceneFbo->Width(), m_sceneFbo->Height(), proj, view,
                             cameraChain,
                             /*output=*/nullptr, vpX, vpY, vpW, vpH, velocity);
        }
        };
        m_frame.AddPass(std::move(pass));
    }

    // Сборка и выполнение. Ошибка описания кадра — это ошибка программиста, и
    // сообщить о ней надо ГРОМКО: без графа она проявлялась бы чёрным экраном
    // или отражением прошлого кадра, то есть чем-то, что легко принять за
    // «шейдер барахлит».
    std::string frameError;
    if (!m_frame.Compile(rScreen, frameError)) {
        LOG_ERROR("Frame") << "Описание кадра неверно: " << frameError;
        return;
    }
    m_frame.Execute();

    // Описание кадра — при каждой смене состава. Ради этого граф во многом и
    // делается: «почему этот проход не выполнился» — вопрос, на который иначе
    // отвечать нечем, а гадать по чёрному экрану дороже, чем прочитать строку.
    const sage::render::FrameGraphStats& fgs = m_frame.Stats();
    if (fgs.PassesExecuted != m_lastFramePasses) {
        m_lastFramePasses = fgs.PassesExecuted;
        LOG_INFO("Frame") << "Состав кадра: " << fgs.PassesExecuted << " из "
                          << fgs.PassesDeclared << " проходов (отброшено "
                          << fgs.PassesCulled << ", выключено " << fgs.PassesDisabled << ")\n"
                          << m_frame.Describe();
    }

    device.SetSRGBWrite(false); // всё после сцены (UI/оверлеи) — уже в sRGB

    // UI сцены (компоненты интерфейса из .sage): худ/меню, собранные в редакторе.
    // Рисуется в letterbox-viewport с его размерами — якоря совпадают с панелью
    // Game редактора (WYSIWYG).
    auto uiView = m_scene->Registry().view<sage::ui::Element>();
    if (uiView.begin() != uiView.end()) {
        if (!m_ui) m_ui = std::make_unique<UIRenderer>();
        m_uiWidth = vpW;  // тот же прямоугольник, с которым сравнивается мышь
        m_uiHeight = vpH;
        m_uiOffsetX = vpX;
        m_uiOffsetY = vpY;
        m_ui->Begin(vpW, vpH);
        sage::ui::DrawSceneUI(*m_scene, *m_ui, vpW, vpH);
        m_ui->End();
    }

    TakeAutoScreenshot();
}

// Снимок кадра по SAGE_SCREENSHOT_AT_FRAME. Отдельной функцией, потому что
// кадр заканчивается в ДВУХ местах: обычный и экран отказа. Пока снимок жил
// только в обычном, экран отказа нельзя было проверить иначе как открыв его
// глазами — а это ровно тот кадр, который видит человек, у которого что-то не
// работает.
void PlayerLayer::TakeAutoScreenshot() {
    ++m_frameCounter;
    if (m_autoScreenshotFrame < 0 || m_frameCounter != m_autoScreenshotFrame) return;
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
    app.Close();
}
