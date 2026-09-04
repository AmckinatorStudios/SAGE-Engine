#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/PostFX.h"
#include "sage/render/LensFlare.h"
#include "sage/render/Volumetrics.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/ShadowAtlas.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Reflection.h"
#include "sage/render/ParticleSystem.h"
#include "sage/ui/UIRenderer.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Light.h"
#include "sage/core/Config.h"
#include "sage/ecs/RenderBatch.h"

#include "EditorHost.h" // EditorRenderMode

// ---------------------------------------------------------------------------
// EditorSceneRenderer — весь превью-рендер редактора, вынесенный из EditorLayer
// (разгрузка god-object): владеет рендер-ресурсами (карта теней, offscreen-FBO
// сцены/Game, PostFX, батч статики, скайбокс, частицы, DebugDraw, outline-шейдер)
// и рисует два кадра — Viewport (редакторская камера + гизмо/аутлайн/сетка) и
// Game (Primary-камера сцены, без гизмо) — с общим shadow-проходом и полной
// пост-обработкой. EditorLayer только оркестрирует: собирает освещение, зовёт
// RenderShadow → RenderViewport → RenderGame и показывает их текстуры в панелях.
// ---------------------------------------------------------------------------
// Явно заданные view/proj для вьюпорта. Нужны ортогональным видам
// (сверху/спереди/сбоку): у них нет свободной камеры, а есть плоскость, центр и
// масштаб — то есть матрицы, которые не выводятся из Camera.
struct EditorViewOverride {
    bool Use = false;
    glm::mat4 View{1.0f};
    glm::mat4 Proj{1.0f};
    glm::vec3 EyePos{0.0f};   // где «стоит» наблюдатель: блики и отражения
};

class EditorSceneRenderer {
public:
    // Время сцены для собственных шейдеров материалов (uTime). Идёт и в режиме
    // правки, а не только в Play: анимированный материал должен шевелиться во
    // вьюпорте — иначе его не настроить, не запуская игру.
    void Tick(float dt) { m_sceneTime += dt; }

    void Init();

    void SetViewportSize(int w, int h) { m_vpW = w; m_vpH = h; }
    void SetGameSize(int w, int h) { m_gameW = w; m_gameH = h; }
    // Размер игрового кадра: под него сверстан интерфейс игры, и с ним же
    // сравнивается курсор, переведённый панелью Game в его координаты.
    int GameWidth() const { return m_gameW; }
    int GameHeight() const { return m_gameH; }

    // Общий depth-проход солнца (одна карта теней на кадр для обоих окон).
    // camera — камера ВЬЮПОРТА: карта теней строится вокруг неё, а не вокруг
    // начала мира, иначе всё, что редактор отлистал в сторону, теней лишается.
    // Готовит отражения кадра (пересъёмка карты окружения при смене неба).
    // Зовётся ДО любых проходов, рисующих во вьюпорт.
    void PrepareReflections(Scene& scene, const LightingEnvironment& env);
    // env НЕ const: проход теней раздаёт локальным источникам места в атласе и
    // записывает номер места в сам источник (см. LocalShadowAtlas::Prepare).
    // Иначе связь «эта лампа — эта плитка» пришлось бы возвращать отдельным
    // списком и не потерять его по дороге до цветного прохода.
    void RenderShadow(Scene& scene, LightingEnvironment& env, const Camera& camera);
    // Подгоняет каскады солнца под заданный вид и перерисовывает глубину.
    // Видов два — вьюпорт и панель Game, — и карта, подогнанная под чужой, даёт
    // не «чуть хуже», а чёрную рябь по земле.
    void FitSunShadows(Scene& scene, const LightingEnvironment& env,
                       const ShadowMap::CameraView& view);
    // Работает ли цепочка пост-обработки на этой видеокарте (проверка один раз
    // за процесс). Отказ показывается кадром БЕЗ эффектов, а не чёрным экраном.
    bool PostWorks();
    bool m_postWarned = false;
    std::string m_renderWarning;
    // Тени кадра одной строкой: каскады солнца + атлас локальных источников.
    // Собирать ShadowBinding вручную на каждом месте отрисовки — значит однажды
    // забыть про атлас, и лампы перестанут отбрасывать тень ровно в том окне,
    // где забыли.
    ShadowBinding FrameShadows() const;

    // Превью сцены редакторской камерой. Возвращает использованные view/proj
    // (нужны вызывающему для гизмо/пикинга). mode/showGrid — из тулбара.
    //
    // slot — какой из вьюпортов рисуем (0..kMaxViews-1). У каждого свой буфер:
    // мультивьюпорт показывает несколько видов ОДНОВРЕМЕННО, и рисовать их в
    // один буфер значило бы, что видно только последний.
    // Рисовать габаритную коробку выделенного. Флагом, а не ещё одним
    // параметром RenderViewport: тот и так принимает девять аргументов и
    // вызывается в цикле по всем видам раскладки.
    void SetShowBounds(bool show) { m_showBounds = show; }

    // Игрового интерфейса во вьюпорте БОЛЬШЕ НЕТ.
    //
    // Он рисовался здесь ради «режима вёрстки»: вьюпорт на время превращался в
    // холст интерфейса — с подложкой поверх сцены, своим масштабом и своей
    // панорамой. Отсюда же брались две копии одной формулы (здесь и в
    // UICanvas) и вечный вопрос, в каком положении сейчас режим.
    //
    // Интерфейс верстается в своём окне (UIEditorPanel), где показан ИГРОВОЙ
    // кадр в разрешении игры — тот же, что рисует RenderGame, вместе со своим
    // UI. Вьюпорту остаётся сцена.

    void RenderViewport(Scene& scene, Camera& camera, const LightingEnvironment& env,
                        int selectedId, const std::vector<int>& selection,
                        EditorRenderMode mode, bool showGrid,
                        const sage::EngineConfig& cfg, glm::mat4& outView, glm::mat4& outProj,
                        int slot = 0, const EditorViewOverride& viewOverride = EditorViewOverride());

    // Сколько видов может показываться одновременно. Четыре — классическая
    // раскладка «перспектива + сверху + спереди + сбоку»; больше на практике не
    // используют, а каждый вид стоит полного прохода сцены.
    static constexpr int kMaxViews = 4;
    void SetViewportSize(int slot, int w, int h);

    // Игровое окно от первой Primary-камеры сцены (нет камеры — кадр не рисуется,
    // GameApplied() остаётся false). Всегда Shaded + пост-обработка.
    void RenderGame(Scene& scene, const LightingEnvironment& env, const sage::EngineConfig& cfg);

    // Текстуры для ImGui-панелей: после PostFX — LDR-выход, иначе HDR-цвет FBO.
    uint64_t ViewportTexture() const;
    // Текстура конкретного вида раскладки.
    uint64_t ViewportTexture(int slot) const;
    uint64_t GameTexture() const;

    // Записать последний игровой кадр в PNG. Именно ИГРОВОЙ: он снят
    // Primary-камерой сцены и уже прошёл пост-обработку и интерфейс — то есть
    // это ровно то, что увидит игрок, а не вид редакторской камеры с сеткой и
    // гизмо. Нужен обложкам шаблонов проекта: рисованная картинка обещает то,
    // чего в проекте может уже не быть, а снимок обещать не умеет.
    //
    // false — кадра ещё нет (RenderGame не звали): молча писать пустой файл
    // значило бы получить чёрную обложку и гадать, откуда она.
    bool SaveGameFrame(const std::string& path);

    // Что с рендером не так — одной строкой для показа В КАДРЕ. Пусто — всё в
    // порядке. Лог для этого не годится: человек, у которого чёрный вьюпорт,
    // смотрит во вьюпорт, а не в консоль, и «не понятно из-за чего» — это
    // ровно то, что он говорит.
    const std::string& Warning() const { return m_renderWarning; }

    // Есть ли в игровом буфере кадр ЭТОЙ сцены. Без Primary-камеры RenderGame
    // не рисует ничего, и в буфере остаётся кадр ПРЕДЫДУЩЕЙ сцены — панель Game
    // прячет его подсказкой, а снимок обложки взял бы чужую картинку и выдал за
    // свою (так «пустой шаблон» и снялся с демо-объектами).
    bool HasGameFrame() const { return m_gameFrameValid; }

    const sage::ecs::RenderStats& LastStats() const { return m_lastStats; }
    ParticleSystem& Particles() { return *m_particles; } // UpdateEmitters + self-test

    // Для self-test редактора: прогнать инстансный батч с заданной камерой.
    sage::ecs::RenderStats RenderColorForTest(Scene& scene, const glm::mat4& view,
                                              const glm::mat4& proj, const glm::vec3& viewPos,
                                              const LightingEnvironment& env);

private:
    float m_sceneTime = 0.0f;


    // Небо кадра (кубическая текстура или процедурный градиент).
    void DrawSky(const LightingEnvironment& env, const glm::mat4& view, const glm::mat4& proj);
    // viewId — какое окно рисуется. Уходит в проверку перекрытия: ответ «этот
    // объект закрыт» принадлежит ТОЧКЕ, из которой смотрят, и вьюпорт с панелью
    // Game обязаны вести его раздельно (см. RenderBatch::SetOcclusionCulling).
    void DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                 const glm::mat4& proj, glm::vec3 viewPos, int shadingMode, bool wireframe,
                 int viewId);
    // Номер вида для панели Game: сразу за слотами вьюпорта, чтобы ни с одним
    // из них не совпасть.
    static constexpr int kGameViewId = kMaxViews;
    // Аутлайн выделения — робастный пост-проход: силуэт объекта в масочный
    // буфер, затем краевая дилатация ПОСТОЯННОЙ ширины в пикселях поверх кадра.
    // Работает для любых мешей (модели/плоскости/невыпуклые), в отличие от
    // прежней «раздутой оболочки» (толщина зависела от размера, только выпуклые).
    // Силуэты ВСЕХ выбранных сущностей в масочный буфер (одна очистка, потом все).
    void RenderOutlineMask(Scene& scene, const std::vector<int>& selection,
                           const glm::mat4& view, const glm::mat4& proj, int w, int h);
    // Приводит карту теней к настройкам конфига (разрешение и число каскадов
    // задаются при создании — поменять их у живой карты нельзя).
    void EnsureShadowMap();
    void CompositeOutline(Framebuffer& target);
    void DrawEntityGizmos(Scene& scene, const std::vector<int>& selection, float gameAspect);
    static sage::render::PostFXSettings FxFromConfig(const sage::EngineConfig& cfg);

    std::optional<Shader> m_outlineShader;   // lit-шейдер как flat-цвет каймы выделения
    std::optional<ShadowMap> m_shadows;
    std::optional<sage::render::LocalShadowAtlas> m_localShadows;
    int m_localShadowRes = 0;
    // С какими настройками создана карта теней — по ним видно, что пора
    // пересоздать (разрешение и число каскадов задаются при создании).
    int m_shadowRes = 0;
    int m_shadowCascades = 0;
    std::optional<Framebuffer> m_sceneFbo, m_gameFbo;
    bool m_gameFrameValid = false;   // в m_gameFbo кадр ТЕКУЩЕЙ сцены
    std::optional<Framebuffer> m_postFbo, m_gamePostFbo; // LDR-выходы PostFX
    // Дополнительные виды (слоты 1..3). Слот 0 — это m_sceneFbo/m_postFbo:
    // одиночный вьюпорт, самый частый случай, не должен платить за раскладку,
    // которой не пользуются.
    std::optional<Framebuffer> m_extraFbo[kMaxViews], m_extraPostFbo[kMaxViews];
    bool m_extraPostApplied[kMaxViews] = {false, false, false, false};
    int m_extraW[kMaxViews] = {0, 0, 0, 0};
    int m_extraH[kMaxViews] = {0, 0, 0, 0};
    std::optional<Framebuffer> m_outlineMask;            // силуэт выделенного объекта (аутлайн)
    std::unique_ptr<sage::rhi::Geometry> m_outlineTri;   // полноэкранный треугольник для краевого прохода
    // UI сцены (компоненты sage::ui) — оверлей в панели Game (WYSIWYG: как в
    // собранной игре). Лениво: создаётся при первом кадре с UI-сущностями.
    std::unique_ptr<UIRenderer> m_ui;
    std::optional<sage::render::PostFX> m_postfx, m_gamePostfx;
    std::optional<sage::render::Volumetrics> m_volumetrics;
    std::optional<sage::render::LensFlare> m_lensFlare;
    bool m_postApplied = false, m_gamePostApplied = false;
    std::optional<DebugDraw> m_debugDraw;
    std::optional<SkyRenderer> m_sky;
    // Отражения вьюпорта. Свои, а не общие с рантаймом: карта окружения
    // снимается из точки и принадлежит виду.
    sage::render::ReflectionSystem m_reflections;
    std::optional<ParticleSystem> m_particles;
    sage::ecs::RenderBatch m_batch;
    sage::ecs::RenderStats m_lastStats;

    int m_vpW = 1280, m_vpH = 720;
    // Размер, под который последний раз строилась маска выделения: кайма
    // накладывается по нему, а у разных видов раскладки размеры разные.
    int m_outlineMaskW = 1280, m_outlineMaskH = 720;
    bool m_showBounds = false;
    int m_gameW = 1280, m_gameH = 720;
};
