#include "VoxelLayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/app/Application.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/render/Screenshot.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/UIRenderer.h"

#include "BlockAtlas.h"

using namespace voxel;

namespace {

// Панель быстрого выбора: что ставится клавишами 1..9.
const uint8_t kHotbar[9] = {Turf, Granite, Boards, Clay, Crystal, Lumen, Trunk, Foliage, Ember};

} // namespace

VoxelLayer::VoxelLayer() : sage::Layer("VoxelWorld") {}
VoxelLayer::~VoxelLayer() = default;

void VoxelLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();

    if (const std::string p = sage::EnvString("SAGE_SCREENSHOT_PATH"); !p.empty()) m_screenshotPath = p;
    if (const char* f = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(f);
    if (const char* t = std::getenv("SAGE_VOXEL_TIME")) m_timeOfDay = (float)std::atof(t);
    m_autopilot = m_autoScreenshotFrame >= 0;

    int chunks = 12;
    if (const char* c = std::getenv("SAGE_VOXEL_CHUNKS")) chunks = std::clamp(std::atoi(c), 2, 48);
    m_world = std::make_unique<World>(chunks, chunks, 1337u);
    LOG_INFO("Voxel") << "Мир " << m_world->SizeX() << "×" << m_world->SizeZ() << ", освещено блоков "
                      << m_world->LitBlocks();

    // Рендерер игры — в реестр движка рядом со встроенным PBR: тот же
    // интерфейс, тот же способ подключения.
    World* world = m_world.get();
    sage::render::RenderPipelineRegistry::Register(
        {VoxelPipeline::kId, "Воксели", "Блочный мир: свет по уровням блоков, своя пост-обработка.",
         [world] { return std::make_unique<VoxelPipeline>(*world); }});
    auto pipeline = std::make_unique<VoxelPipeline>(*m_world);
    m_pipeline = pipeline.get();
    if (std::getenv("SAGE_VOXEL_CPU_LIGHTMAP")) m_pipeline->ForceCpuLightmap = true;
    m_renderer = std::make_unique<sage::render::PipelineRenderer>(std::move(pipeline));
    if (!m_renderer->Ready()) LOG_ERROR("Voxel") << "рендерер не готов: " << m_renderer->LastError();

    // Интерфейс — расширением графа: проход после поста, рисует поверх кадра.
    m_renderer->AddExtension("hud", [this](sage::render::RenderGraph& g, const sage::render::RenderView&) {
        sage::render::RGPass p;
        p.Name = "Интерфейс";
        p.Writes = {g.Output()};
        p.Execute = [this](sage::render::RenderGraphContext&) { DrawHud(); };
        g.AddPassAfter("Пост: тон и виньетка", std::move(p));
    });

    m_ui = std::make_unique<UIRenderer>();
    int aw = 0, ah = 0;
    const std::vector<uint8_t> px = BuildAtlasPixels(aw, ah);
    m_atlasUi = std::make_unique<Texture>(px.data(), aw, ah, TextureFilter::Nearest, false, true);

    const int cx = m_world->SizeX() / 2, cz = m_world->SizeZ() / 2;
    m_player.Position = glm::vec3(cx + 0.5f, (float)m_world->SurfaceHeight(cx, cz) + 1.0f, cz + 0.5f);
    if (m_autopilot) {
        // Вид для проверочного снимка: над миром, взгляд на рельеф.
        m_player.Flying = true;
        m_player.Position += glm::vec3(-20.0f, 14.0f, -20.0f);
        m_player.Yaw = 45.0f;
        m_player.Pitch = -24.0f;
    }

    m_bridge.Attach(window, m_input);
    m_input.Register("Move", sage::input::ActionType::Vector).BindVector("W", "S", "A", "D");
    m_input.Register("Jump").Bind("SPACE");
    m_input.Register("Down").Bind("LEFT_SHIFT");
    m_input.Register("Fly").Bind("F");
    m_input.Register("Break").Bind("MOUSE_LEFT");
    m_input.Register("Place").Bind("MOUSE_RIGHT");
    m_input.Register("Debug").Bind("F3");
    m_input.Register("Reload").Bind("F5");
    m_input.Register("FastTime").Bind("T");
    m_input.Register("Quit").Bind("ESCAPE");
    for (int i = 0; i < 9; ++i) m_input.Register("Slot" + std::to_string(i + 1)).Bind(std::to_string(i + 1));
    if (!m_autopilot) {
        m_input.SetCursorCaptured(true);
        m_captured = true;
    }
}

void VoxelLayer::OnDetach() {
    m_renderer.reset();
    m_ui.reset();
    m_atlasUi.reset();
}

void VoxelLayer::OnUpdate(float dt) {
    sage::Application& app = sage::Application::Get();
    m_bridge.PollGamepads();
    m_input.Update(dt);
    m_time += dt;
    m_fps = m_fps * 0.95f + (dt > 0.0f ? 1.0f / dt : 0.0f) * 0.05f;

    if (m_input.WasPressed("Quit")) {
        // Первый Esc отпускает мышь, второй — выход.
        if (m_captured) {
            m_input.SetCursorCaptured(false);
            m_captured = false;
        } else {
            app.Close();
        }
    }
    if (!m_captured && !m_autopilot && m_input.WasPressed("Break")) {
        m_input.SetCursorCaptured(true);
        m_captured = true;
        return;
    }
    if (m_input.WasPressed("Debug")) m_debug = !m_debug;
    if (m_input.WasPressed("Fly")) m_player.Flying = !m_player.Flying;
    for (int i = 0; i < 9; ++i)
        if (m_input.WasPressed("Slot" + std::to_string(i + 1))) m_slot = i;
    const float wheel = m_input.Wheel();
    if (wheel != 0.0f) m_slot = (m_slot + (wheel < 0.0f ? 1 : 8)) % 9;

    m_timeOfDay = std::fmod(m_timeOfDay + dt / (m_input.IsDown("FastTime") ? 15.0f : 600.0f), 1.0f);

    // Правка шейдеров на лету: раз в секунду сверяем время правки файлов.
    m_reloadTimer += dt;
    if (m_input.WasPressed("Reload") || m_reloadTimer > 1.0f) {
        m_reloadTimer = 0.0f;
        if (m_pipeline) m_pipeline->Shaders().ReloadChanged();
    }

    if (!m_autopilot) {
        if (m_captured) {
            const glm::vec2 look = m_input.MouseDelta();
            m_player.Look(look.x, look.y);
        }
        PlayerInput in;
        in.Move = m_input.Vector("Move");
        in.Jump = m_input.IsDown("Jump");
        in.Down = m_input.IsDown("Down");
        m_player.Update(*m_world, in, dt);
    }

    m_target = m_world->Raycast(m_player.Eye(), m_player.Forward(), 6.0f);
    if (m_captured && m_target.Hit) {
        if (m_input.WasPressed("Break")) m_world->Set(m_target.Block.x, m_target.Block.y, m_target.Block.z, Air);
        if (m_input.WasPressed("Place")) {
            const glm::ivec3 p = m_target.Block + m_target.Normal;
            const uint8_t at = m_world->Get(p.x, p.y, p.z);
            if ((at == Air || at == Water) && !m_player.Overlaps(p)) m_world->Set(p.x, p.y, p.z, kHotbar[m_slot]);
        }
    }
}

void VoxelLayer::OnRender() {
    sage::Application& app = sage::Application::Get();
    Window& window = app.GetWindow();
    if (!m_renderer || !m_renderer->Ready()) return;

    const float aspect = (float)window.Width() / (float)std::max(window.Height(), 1);
    const glm::vec3 eye = m_player.Eye();
    sage::render::RenderView view;
    view.View = glm::lookAt(eye, eye + m_player.Forward(), glm::vec3(0.0f, 1.0f, 0.0f));
    view.Proj = glm::perspective(glm::radians(70.0f), aspect, 0.05f,
                                 (float)(m_pipeline->RenderDistance * kChunkSize) + 64.0f);
    view.Position = eye;
    view.Width = window.Width();
    view.Height = window.Height();
    view.Time = m_time;
    view.Frame = ++m_frame;

    VoxelFrame& f = m_pipeline->Frame;
    f.TimeOfDay = m_timeOfDay;
    f.Underwater = m_world->Get((int)std::floor(eye.x), (int)std::floor(eye.y), (int)std::floor(eye.z)) == Water;
    f.HasSelection = m_target.Hit;
    f.Selection = m_target.Block;
    m_renderer->Render(view);

    if (m_autopilot && m_frame == 3) SelfTest();
    if (m_autoScreenshotFrame >= 0 && m_frame == m_autoScreenshotFrame) {
        LOG_INFO("Voxel") << "Кадр:\n" << m_renderer->Graph().Describe();
        SaveScreenshot(m_screenshotPath, window.Width(), window.Height());
        app.Close();
    }
}

void VoxelLayer::DrawHud() {
    Window& window = sage::Application::Get().GetWindow();
    const float w = (float)window.Width(), h = (float)window.Height();
    UIRenderer& ui = *m_ui;
    ui.Begin(window.Width(), window.Height());

    // Прицел.
    ui.Rect(w * 0.5f - 9.0f, h * 0.5f - 1.0f, 18.0f, 2.0f, glm::vec3(0.95f), 0.85f);
    ui.Rect(w * 0.5f - 1.0f, h * 0.5f - 9.0f, 2.0f, 18.0f, glm::vec3(0.95f), 0.85f);

    // Панель блоков.
    const float slot = 44.0f, gap = 4.0f, total = 9 * slot + 8 * gap;
    const float x0 = (w - total) * 0.5f, y0 = h - slot - 14.0f;
    for (int i = 0; i < 9; ++i) {
        const float x = x0 + i * (slot + gap);
        ui.Rect(x, y0, slot, slot, glm::vec3(0.05f), 0.55f);
        const BlockInfo& bi = Info(kHotbar[i]);
        const int tile = bi.TileSide;
        UIRenderer::Sprite s{(float)(tile % kAtlasGrid * kTilePixels), (float)(tile / kAtlasGrid * kTilePixels),
                             (float)kTilePixels, (float)kTilePixels};
        ui.ImageSprite(x + 6.0f, y0 + 6.0f, slot - 12.0f, slot - 12.0f, m_atlasUi.get(), s);
        if (i == m_slot) ui.RectOutline(x - 2.0f, y0 - 2.0f, slot + 4.0f, slot + 4.0f, 3.0f, glm::vec3(1.0f));
    }
    ui.TextCentered(w * 0.5f, y0 - 26.0f, 1.4f, glm::vec3(1.0f), Info(kHotbar[m_slot]).Name);

    char buf[256];
    if (m_debug || m_autopilot) {
        const auto& st = m_renderer->Graph().Stats();
        std::snprintf(buf, sizeof(buf), "%.0f FPS  чанков %d  граней %d  проходов %d/%d  карта света: %s",
                      m_fps, m_pipeline->ChunksDrawn(), m_pipeline->FacesDrawn(), st.Executed, st.Passes,
                      m_pipeline->UsesComputeLightmap() ? "compute" : "CPU");
        ui.Text(12.0f, 12.0f, 1.2f, glm::vec3(1.0f), buf);
        float y = 34.0f;
        for (const sage::render::RGPassTiming& t : m_renderer->Graph().Timings()) {
            if (!t.Executed) continue;
            if (t.GpuMs >= 0.0) std::snprintf(buf, sizeof(buf), "%-24s CPU %.2f  GPU %.2f мс", t.Name.c_str(), t.CpuMs, t.GpuMs);
            else std::snprintf(buf, sizeof(buf), "%-24s CPU %.2f мс", t.Name.c_str(), t.CpuMs);
            ui.Text(12.0f, y, 1.0f, glm::vec3(0.85f, 0.9f, 1.0f), buf);
            y += 18.0f;
        }
        if (const int errors = m_pipeline->Shaders().ErrorCount())
            ui.Text(12.0f, y + 6.0f, 1.1f, glm::vec3(1.0f, 0.4f, 0.35f),
                    "Ошибок в шейдерах: " + std::to_string(errors) + " (см. лог)");
    } else {
        ui.Text(12.0f, 12.0f, 1.1f, glm::vec3(1.0f),
                "WASD, пробел — ходьба; F — полёт; ЛКМ/ПКМ — сломать/поставить; 1-9 — блок; T — время; F3 — отладка");
    }
    ui.End();
}

void VoxelLayer::SelfTest() {
    std::string fail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok && fail.empty()) fail = what;
    };
    // Открытый столб: над поверхностью только воздух (под кроной небо и
    // должно быть неполным — там проверять нечего).
    int x = m_world->SizeX() / 2, z = m_world->SizeZ() / 2;
    auto open = [&](int cx, int cz) {
        const int t = m_world->SurfaceHeight(cx, cz);
        if (m_world->Get(cx, t, cz) == Water) return false;
        for (int y = t + 1; y < kWorldHeight; ++y)
            if (m_world->Get(cx, y, cz) != Air) return false;
        return true;
    };
    for (int r = 0; r < 32 && !open(x, z); ++r) {
        x = m_world->SizeX() / 2 + (r % 8) * 3 - 12;
        z = m_world->SizeZ() / 2 + (r / 8) * 3 - 6;
    }
    const int top = m_world->SurfaceHeight(x, z);
    check(m_world->SkyLight(x, top + 1, z) == 15, "над поверхностью нет полного неба");

    // Источник света: ставим, проверяем соседа, убираем. Сравниваем с уровнем
    // ДО установки: свет пещерных светокамней может доходить и до поверхности.
    const uint8_t before = m_world->BlockLight(x, top + 2, z);
    m_world->Set(x, top + 1, z, Lumen);
    check(m_world->BlockLight(x, top + 2, z) == 14, "светокамень не осветил соседа");
    m_world->Set(x, top + 1, z, Air);
    check(m_world->BlockLight(x, top + 2, z) == before, "свет не вернулся к прежнему после снятия источника");

    // Тень: блок над клеткой гасит прямое небо до рассеянного.
    m_world->Set(x, top + 3, z, Granite);
    check(m_world->SkyLight(x, top + 2, z) == 14, "под блоком осталось прямое небо");
    m_world->Set(x, top + 3, z, Air);
    check(m_world->SkyLight(x, top + 2, z) == 15, "небо не вернулось после снятия блока");

    // Свет после сотен случайных правок совпадает с полным пересчётом: на
    // отдельном маленьком мире, чтобы не трогать тот, в котором играют.
    {
        World probe(2, 2, 4242u);
        uint32_t r = 12345u;
        auto next = [&] { return r = r * 1664525u + 1013904223u; };
        const uint8_t pal[] = {Air, Air, Granite, Lumen, Ember, Foliage, Water, Crystal};
        for (int i = 0; i < 300; ++i) {
            const int px = (int)(next() >> 8) % probe.SizeX(), pz = (int)(next() >> 8) % probe.SizeZ();
            const int py = std::max(1, probe.SurfaceHeight(px, pz) - 6 + (int)((next() >> 8) % 10));
            probe.Set(px, py, pz, pal[(next() >> 8) % 8]);
        }
        const int mismatches = probe.LightMismatches();
        if (mismatches) LOG_WARN("Voxel") << "свет после правок разошёлся с пересчётом в " << mismatches << " клетках";
        check(mismatches == 0, "свет после правок расходится с полным пересчётом");
    }

    const sage::render::RenderGraph& g = m_renderer->Graph();
    bool chunksRan = false;
    for (const auto& t : g.Timings())
        if (t.Name == "Чанки" && t.Executed) chunksRan = true;
    check(chunksRan, "проход «Чанки» не выполнился");
    check(m_pipeline->ChunksDrawn() > 0, "не нарисовано ни одного чанка");
    check(m_pipeline->Shaders().ErrorCount() == 0, "ошибки в шейдерах");
    check(g.HasPass("Интерфейс"), "расширение интерфейса не вставило проход");

    if (fail.empty())
        LOG_INFO("Voxel") << "VOXEL: SELFTEST PASS (карта света: "
                          << (m_pipeline->UsesComputeLightmap() ? "compute" : "CPU") << ", чанков "
                          << m_pipeline->ChunksDrawn() << ")";
    else
        LOG_ERROR("Voxel") << "VOXEL: SELFTEST FAIL: " << fail;
}
