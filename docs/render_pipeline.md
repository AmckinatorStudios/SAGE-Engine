# Свой рендер в SAGE Engine

Встроенный PBR — не основа движка, а **одна из реализаций** рендерера. Игра
может дополнить его своими проходами, заменить отдельный этап или написать
весь кадр сама: свои шейдеры, материалы, геометрию, свет и пост-обработку.
Движок даёт инструменты, а не готовые решения под жанр: блочного мира, света
по вокселям или готовой «воды» в нём нет и не будет — это пишет игра. Пример
игры целиком на своём рендерере — отдельная `games/voxelworld` (не часть
движка, собирается флагом `-DSAGE_BUILD_VOXELWORLD=ON`, описание — в её
`README.md`).

| Что | Где |
|---|---|
| Рендерер и реестр рендереров | `sage/render/RenderPipeline.h` |
| Граф кадра: проходы, ресурсы, зависимости | `sage/render/RenderGraph.h` |
| Встроенный PBR как рендерер (`"sage.pbr"`) | `sage/render/StandardPipeline.h` |
| Свои шейдеры: `#include`, ошибки по строкам, правка на лету | `sage/render/ShaderLibrary.h` |
| Материал своего шейдера | `sage/render/ShaderMaterial.h` |
| Свои эффекты и их цепочки | `sage/render/EffectChain.h` |
| Вычислительные шейдеры, буферы, запись в текстуры | `sage/rhi/GraphicsDevice.h` |

## Три степени свободы

1. **Дополнить** чужой рендерер — `PipelineRenderer::AddExtension`: вставить
   проход до или после штатного по имени, выключить или заменить штатный.
2. **Унаследовать** встроенный PBR и переопределить этап
   (`StandardPipeline::AddShadowPasses / AddScenePasses / AddPostPasses`).
3. **Написать свой** — наследник `RenderPipeline`.

```cpp
class MyPipeline : public sage::render::RenderPipeline {
public:
    std::string Id() const override { return "game.mine"; }
    bool Initialize(sage::rhi::GraphicsDevice& device, std::string& error) override {
        m_shaders.Load("ground", "assets/shaders/ground.vert", "assets/shaders/ground.frag");
        return m_shaders.Get("ground") != nullptr;   // false — рендерер не запустится, причина в error
    }
    void Build(sage::render::RenderGraph& g, const sage::render::RenderView& view) override {
        using namespace sage::render;
        const RGHandle hdr = g.CreateTexture("HDR", {});   // цель размером с выход; граф создаёт её сам
        RGPass scene{"Сцена"};
        scene.Writes = {hdr};
        scene.ColorLoad = LoadOp::Clear;                  // граф привяжет цель и очистит
        scene.DepthLoad = LoadOp::Clear;
        scene.Execute = [this, view](RenderGraphContext& ctx) { /* свои вызовы отрисовки */ };
        g.AddPass(std::move(scene));
        AddEffectChain(g, "Пост", hdr, g.Output(), {{"тон", &m_tone, true, {}}});
    }
private:
    sage::render::ShaderLibrary m_shaders;
    sage::render::ShaderMaterial m_tone{m_shaders, "tone"};
};

// Регистрация — тем же способом, что у встроенного PBR:
sage::render::RenderPipelineRegistry::Register(
    {"game.mine", "Мой рендер", "", [] { return std::make_unique<MyPipeline>(); }});

// Каждый кадр:
sage::render::PipelineRenderer renderer(sage::render::RenderPipelineRegistry::Create("game.mine"));
sage::render::RenderView view;            // камера, размер, время, сцена (может не быть), цель
renderer.Render(view);
```

## Граф кадра (RenderGraph)

Проход — данные: имя, тип, что читает, что пишет, тело. Граф:

- **создаёт ресурсы** по описанию (`CreateTexture`, `CreateBuffer`) и держит
  их между кадрами в пуле: устоявшийся кадр видеопамять не выделяет
  (`Stats().CreatedThisFrame == 0`), смена размера окна — `Resize`, а не
  пересоздание;
- принимает **чужие** ресурсы (`ImportTarget`, `ImportTexture`,
  `ImportBuffer`) и **виртуальные** (`DeclareVirtual` — только имя для
  зависимости, например «Меши чанков»);
- проверяет порядок и **выбрасывает** проходы, результат которых не нужен
  выходу; чтение того, во что никто не писал, — ошибка сборки кадра с
  причиной (`LastError()`), а не чёрный экран;
- для прохода `Graphics` **привязывает цель** (`Target` или первую из
  `Writes`) и выполняет `ColorLoad`/`DepthLoad`. Дорисовка поверх
  (`LoadOp::Load`) считается чтением прежнего содержимого: проход, очистивший
  цель раньше, не выбросится;
- после прохода `Compute` ставит **барьер памяти**;
- проход `Custom` граф ничего не привязывает (тени, тракт PostFX, загрузка
  данных). Что он читает, нужно перечислить в `Reads` явно;
- меряет каждый проход: CPU всегда, GPU — метками времени, где они есть
  (`Timings()`); `Describe()` — текстом весь кадр.

Порядок проходов задаёт автор: граф его не переставляет (модель версий —
как у `FrameGraph`, см. `FrameGraph.h`). Вставить в чужой кадр —
`AddPassBefore/AddPassAfter(имя, проход)`, выключить штатный — `DisablePass`.

Имена проходов встроенного PBR — константы `StandardPipeline::kShadowPass`
(«Тени»), `kSkyPass` («Небо»), `kScenePass` («Сцена»), `kPostPass` («Пост»),
ресурс `kHdr` («HDR»). Пример: своя вода после штатной сцены —

```cpp
renderer.AddExtension("water", [&](RenderGraph& g, const RenderView& v) {
    RGPass p{"Вода"};
    p.Writes = {g.Find(StandardPipeline::kHdr)};
    p.Execute = [&](RenderGraphContext& ctx) { if (water.Apply(ctx.Device())) DrawWater(); };
    g.AddPassAfter(StandardPipeline::kScenePass, std::move(p));
});
```

## Свои шейдеры (ShaderLibrary)

- Стадии: вершинная, фрагментная, геометрическая, вычислительная
  (`LoadStages`, `Load`, `LoadCompute`); исходник — файлом или текстом.
- `#include "файл.glsl"` — относительно шейдера и `AddIncludeDir`; каждый
  файл включается один раз. Встроенные блоки движка: `<sage/common.glsl>`
  (полноэкранный треугольник, sRGB, ACES, хэш) и `<sage/pbr.glsl>` (освещение
  штатного PBR — если нужно не своё). Свои встроенные — `RegisterShaderInclude`.
- **Ошибки — с файлом и строкой** исходного файла, в том числе внутри
  включённого: «assets/shaders/light.glsl:12: error: …». Понимаются ответы Mesa,
  NVIDIA и AMD/Intel. Ошибки — в `Find(имя)->Diagnostics`, в лог и в
  `ErrorCount()`.
- **Правка на лету**: `ReloadChanged()` пересобирает программы, у которых
  изменился любой файл, включая подключённые. Шейдер с ошибкой **не заменяет**
  рабочий: рисуется прежняя версия. У успешной пересборки растёт `Version`.

Сборка программы на любом бэкенде — `GraphicsDevice::CreateProgram(desc, log)`:
без исключений, ответ драйвера по стадиям.

## Материалы своих шейдеров (ShaderMaterial)

Не PBR: у материала нет «цвета» и «шероховатости», есть то, что объявил шейдер
— числа, векторы, матрицы (`Set`), текстуры (`SetTexture(имя, юнит, текстура)`),
буферы (`SetBuffer(binding, буфер)`) и состояние отрисовки (`State`: смешивание,
глубина, отсечение, запись цвета). Программа берётся из библиотеки по имени
при каждом `Apply`, поэтому правка шейдера на лету подхватывается без
пересоздания материалов. Штатные материалы (`.sagemat`, см.
[custom_shaders.md](custom_shaders.md)) работают как прежде.

Проход, поменявший состояние, возвращает умолчание — `RestoreDefaultState`.

## Своя пост-обработка (EffectChain)

Эффект — материал своего шейдера, нарисованный на весь экран. В шейдере —
`uSource` (юнит 0, результат предыдущего шага), `uDepth` (юнит 1, глубина
входа цепочки), `uTexel`. Вершинный шейдер — `FullscreenVertexSource()`.
`AddEffectChain(graph, "Пост", вход, выход, шаги)` объявляет проходы по
порядку списка; выключенный шаг выпадает, промежуточные цели граф заводит сам.
Штатный тракт `PostChain` можно оставить, заменить цепочкой или совместить.

## Свой свет, геометрия и GPU

Движок не навязывает модель освещения: свет — это то, что посчитали шейдеры и
буферы игры. Для этого RHI даёт:

| Вызов | Что |
|---|---|
| `Capabilities()` | что умеет устройство на этой машине (вычисления, буферы, запись в текстуры, лимиты) |
| `CreateBuffer`, `BindBuffer` | буферы шейдеров (std430 / std140), запись и чтение обратно |
| `BindImage` | текстура как изображение для `imageLoad/imageStore` (`Texture2DDesc::ShaderWrite`) |
| `Dispatch`, `ShaderBarrier` | запуск вычислительной программы и барьер памяти |
| `AttribType::UInt` | целые атрибуты вершин — упакованная геометрия |
| `Geometry` | свои вершинные и индексные буферы |

**Вычисления в OpenGL — это 4.3.** Драйверы Linux и Windows дают его почти
всегда, macOS — нет (OpenGL 4.1). Поэтому спрашивайте `Capabilities()` и
держите запасной путь. Удобный приём — два прохода, пишущих один ресурс
(вычислительный и на CPU), и флаг `Enabled`, выбирающий один из них.

**Vulkan-бэкенд** этих вызовов пока не реализует: `Capabilities()` честно
отвечает «нет», `CreateBuffer` — `nullptr`, `CreateProgram` собирает только
пару «вершинный + фрагментный». Null-бэкенд соглашается на всё (буферы —
настоящая память процесса), на нём проверяется устройство кадра без видеокарты.
