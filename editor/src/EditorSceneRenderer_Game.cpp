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



// Кадр игры (панель Game) и превью камеры: тот же путь, что у собранной игры.
// Кадр ВЫБРАННОЙ камеры — тем же хелпером, что и панель Game и собранная игра
// (sage::ecs::CameraFrameFor). Второй способ посчитать «куда смотрит камера»
// разошёлся бы с игрой на первом же повёрнутом родителе, а превью для того и
// нужно, чтобы ему верили.
void EditorSceneRenderer::RenderCameraPreview(Scene& scene, const LightingEnvironment& env,
                                              const sage::EngineConfig& cfg,
                                              entt::entity camera) {
    m_previewValid = false;
    m_previewPostApplied = false;
    if (m_previewW < 8 || m_previewH < 8) return;

    const float aspect = (float)m_previewW / (float)std::max(m_previewH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::CameraFrameFor(scene, camera, aspect);
    if (!frame.HasPrimary) return;   // не камера (или сущности уже нет)

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    EnsureFramebuffer(m_previewFbo, m_previewW, m_previewH, sage::render::SceneSamples(cfg));
    m_previewFbo->Bind();
    const glm::vec3 clear = sage::render::SceneClearColor(env);
    device.SetClearColor(clear.r, clear.g, clear.b, 1.0f);
    device.Clear();

    // ТЕНИ БЕРУТСЯ ТЕ ЖЕ, что построил вьюпорт этим кадром, и заново под эту
    // камеру не подгоняются. Подгонка — общая для кадра (солнце одно на сцену),
    // и превью размером с почтовую марку забрало бы карту у вьюпорта, где
    // работают: тени поехали бы в главном окне ради теней в углу. Ошибка
    // выборки в превью стоит куда меньше.
    DrawSky(env, frame.View, frame.Proj);
    DrawLit(scene, env, frame.View, frame.Proj, frame.Position, /*shadingMode=*/0,
            /*wireframe=*/false, /*viewId=*/kPreviewViewId);
    m_particles->DrawFromView(frame.View, frame.Proj);
    m_previewFbo->Resolve();

    // Пост-обработка — как в игре: превью обещает игровой кадр, а не «сцену без
    // эффектов». Объёмного света и блика здесь нет намеренно: два лишних
    // полноэкранных прохода ради картинки в углу.
    sage::render::PostChain previewChain;
    if (sage::render::ResolvePostChain(scene, camera, previewChain) && PostWorks()) {
        EnsureFramebuffer(m_previewPostFbo, m_previewW, m_previewH);
        m_previewPostFbo->Resize(m_previewW, m_previewH);
        m_previewPostfx->SetLighting(&env);
        m_previewPostfx->Render(m_previewFbo->ColorTexture(), m_previewFbo->DepthTexture(),
                         m_previewFbo->Width(), m_previewFbo->Height(), frame.Proj, frame.View,
                         previewChain,
                         /*output=*/&*m_previewPostFbo, 0, 0, m_previewW,
                         m_previewH);
        m_previewPostApplied = true;
    }

    device.BindDefaultFramebuffer();
    m_previewValid = true;
}


void EditorSceneRenderer::RenderGame(Scene& scene, const LightingEnvironment& env, const sage::EngineConfig& cfg) {
    // Кадр от Primary-камеры сцены — ЧЕРЕЗ ТОТ ЖЕ ХЕЛПЕР, что и собранная игра
    // (sage::ecs::PrimaryCameraFrame): превью Game-панели = реальный вид в игре.
    // Нет Primary-камеры — кадр не рисуем (панель Game покажет подсказку).
    float aspect = (float)m_gameW / (float)std::max(m_gameH, 1);
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(scene, aspect);
    if (!frame.HasPrimary) {
        // КАМЕРЫ НЕТ, А ИНТЕРФЕЙС ЕСТЬ. Редактор интерфейса показывает игровой
        // кадр — и на сцене без Primary-камеры показывал пустое место: верстать
        // было не на чем, хотя к камере интерфейс никакого отношения не имеет
        // (он считается в пикселях кадра, а не в мире). Непрозрачная подложка
        // сама по себе годится в фон: под ней сцены всё равно не видно.
        if (m_uiBackdropAlpha >= 0.999f && !DrawGameUI(scene, cfg, /*onlyBackdrop=*/true)) {
            m_gamePostApplied = false;
            m_gameFrameValid = false;
        }
        return;
    }

    const glm::mat4& view = frame.View;
    const glm::mat4& proj = frame.Proj;
    glm::vec3 camPos = frame.Position;

    // ТЕНИ — ПОД ИГРОВУЮ КАМЕРУ. Карта, оставшаяся от вьюпорта, натянута на
    // другую пирамиду: выборки уходят за каскад, и по земле идёт чёрная рябь.
    // Панель Game обещает «как в собранной игре», а та считает тени под свою
    // камеру — здесь делаем то же самое.
    if (cfg.Shadows && m_shadows) {
        ShadowMap::CameraView gv;
        gv.Position = camPos;
        // Направление и «верх» — из матрицы вида: у игровой камеры нет объекта
        // Camera, она задана компонентом, и восстанавливать углы обратно значило
        // бы завести второй способ считать то же самое.
        const glm::mat3 rot = glm::mat3(glm::transpose(view));
        gv.Forward = -glm::vec3(rot[2]);
        gv.Up = glm::vec3(rot[1]);
        gv.FovY = 2.0f * std::atan(1.0f / proj[1][1]);
        gv.Aspect = aspect;
        gv.Near = 0.1f;
        gv.ShadowDistance = env.Shadows.Distance > 0.0f ? env.Shadows.Distance
                                                        : cfg.ShadowDistance;
        FitSunShadows(scene, const_cast<LightingEnvironment&>(env), gv);
    }

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    EnsureFramebuffer(m_gameFbo, m_gameW, m_gameH, sage::render::SceneSamples(cfg));
    m_gameFrameValid = true;
    m_gameFbo->Bind();
    // Заливка — общим правилом (SkyDraw.h): выключенное небо чёрное, а не
    // голубое.
    const glm::vec3 clear = sage::render::SceneClearColor(env);
    device.SetClearColor(clear.r, clear.g, clear.b, 1.0f);
    device.Clear();

    DrawSky(env, view, proj);
    // Игровое окно — всегда Shaded, без гизмо (как увидит игрок).
    DrawLit(scene, env, view, proj, camPos, /*shadingMode=*/0, /*wireframe=*/false,
            /*viewId=*/kGameViewId);
    sage::rhi::TextureHandle gameVelocity;
    sage::render::PostChain gameChain;
    const bool gameWantsPost =
        sage::render::ResolvePostChain(scene, sage::ecs::PrimaryCameraEntity(scene), gameChain);
    if (gameWantsPost && sage::render::ChainNeedsVelocity(gameChain)) {
        gameVelocity = m_gameVelocity.Render(m_batch, view, proj, m_gameW, m_gameH);
        m_gameFbo->Bind();
        device.SetViewport(0, 0, m_gameW, m_gameH);
    }
    m_particles->DrawFromView(view, proj);

    // Отладочная графика игры (Debug:DrawLine) — и в панели Game тоже: она
    // показывает игру, а отладочные линии рисуют, чтобы понять игру.
    if (m_scriptDebug && !m_scriptDebug->Empty()) {
        DrawScriptDebug();
        m_debugDraw->Flush(view, proj);
    }

    // Объём и блик — ТЕ ЖЕ ДВА ПРОХОДА, что у собранной игры (см. PlayerLayer),
    // и в том же порядке: объём после геометрии, блик после объёма, оба до
    // пост-обработки.
    //
    // Их здесь не было вовсе, и это ломало ровно то, ради чего панель Game
    // существует. Включив объёмный свет, человек видел туман во вьюпорте и
    // чистый кадр в окне игры — при том что в собранной игре туман есть. Панель,
    // которая обещает показать игру, показывала третью картинку, не совпадающую
    // ни с редактором, ни с игрой; и «в редакторе тёмно, а в игре нормально»
    // начиналось именно с этого расхождения.
    if (cfg.Volumetrics) {
        m_gameFbo->Resolve();
        if (!m_volumetrics) m_volumetrics.emplace();
        m_volumetrics->Render(*m_gameFbo, m_gameFbo->DepthTexture(), m_gameW, m_gameH, proj,
                              view, camPos, env, FrameShadows(),
                              sage::render::VolumetricsFromConfig(cfg), m_sceneTime,
                              /*viewId=*/kGameViewId);
    }

    m_gameFbo->Resolve();   // MSAA -> обычные текстуры (без MSAA — пустышка)

    m_gamePostApplied = false;
    if (gameWantsPost && PostWorks()) {
        m_gamePostFbo->Resize(m_gameW, m_gameH);
        m_gamePostfx->SetLighting(&env);
        m_gamePostfx->Render(m_gameFbo->ColorTexture(), m_gameFbo->DepthTexture(),
                             m_gameFbo->Width(), m_gameFbo->Height(), proj, view, gameChain,
                             /*output=*/&*m_gamePostFbo, 0, 0, m_gameW, m_gameH, gameVelocity);
        m_gamePostApplied = true;
    }

    // UI сцены (компоненты интерфейса) — поверх ИТОГОВОЙ картинки (после поста),
    // ровно как его увидит игрок в собранной игре (WYSIWYG панели Game).
    DrawGameUI(scene, cfg, /*onlyBackdrop=*/false);
    device.BindDefaultFramebuffer();
}


// Проход интерфейса: подложка редактора и сам интерфейс сцены — ОДНИМ кадром
// UIRenderer, в правильном порядке.
//
// Порядок здесь и есть всё содержание: подложка первой, интерфейс поверх неё.
// Пока подложку рисовала панель редактора поверх готового кадра, она ложилась
// и на интерфейс тоже — то есть «приглушить сцену за меню» приглушало и меню.
//
// onlyBackdrop — кадр без сцены (нет Primary-камеры): заливка вместо картинки
// игры. Возвращает false, если рисовать было нечего, — вызывающий по этому
// признаку объявляет игровой кадр недействительным.
bool EditorSceneRenderer::DrawGameUI(Scene& scene, const sage::EngineConfig& cfg,
                                     bool onlyBackdrop) {
    // ЕСТЬ ЛИ ЧТО РИСОВАТЬ — в текущей области, а не вообще в сцене. Иначе в
    // пространстве вёрстки пустой интерфейс считался бы непустым из-за чужого,
    // и кадр собирался бы ради ничего.
    const bool hasUI = !sage::ui::SolveSceneRects(scene, m_gameW, m_gameH,
                                                  /*includeHidden=*/false, m_uiScope).empty();
    const bool hasBackdrop = m_uiBackdropAlpha > 0.001f;
    if (!hasUI && !hasBackdrop) return false;

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    if (onlyBackdrop) {
        // Своего кадра ещё нет: собираем его с нуля из одной заливки.
        EnsureFramebuffer(m_gameFbo, m_gameW, m_gameH, sage::render::SceneSamples(cfg));
        m_gameFrameValid = true;
        m_gamePostApplied = false;
        m_gameFbo->Bind();
        device.SetClearColor(m_uiBackdropColor.r, m_uiBackdropColor.g, m_uiBackdropColor.b, 1.0f);
        device.Clear();
        m_gameFbo->Resolve();
    }

    if (!m_ui) m_ui = std::make_unique<UIRenderer>();
    Framebuffer& target = m_gamePostApplied ? *m_gamePostFbo : *m_gameFbo;
    target.Bind();
    device.SetViewport(0, 0, m_gameW, m_gameH);
    m_ui->Begin(m_gameW, m_gameH);
    if (hasBackdrop && !onlyBackdrop) {
        m_ui->Rect(0.0f, 0.0f, (float)m_gameW, (float)m_gameH, m_uiBackdropColor,
                   std::min(m_uiBackdropAlpha, 1.0f));
    }
    if (hasUI) sage::ui::DrawSceneUI(scene, *m_ui, m_gameW, m_gameH, m_uiScope);
    m_ui->End();
    // РЕЗОЛВ ПОСЛЕ ПРОХОДА. При включённом MSAA Bind() кладёт рисование в
    // многосэмпловый буфер, а показывается обычная текстура — то есть без
    // этой строки интерфейс (и подложка) просто не доезжали до экрана, и
    // сглаживание выглядело как «интерфейс в игровом окне пропал».
    target.Resolve();
    if (onlyBackdrop) device.BindDefaultFramebuffer();
    return true;
}
