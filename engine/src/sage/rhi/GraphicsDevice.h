#pragma once
#include <memory>
#include <string>
#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// RHI (Render Hardware Interface) — абстракция графического «устройства».
//
// ЧТО ЗДЕСЬ ПРАВДА. Движок и игры не обращаются к OpenGL напрямую: за
// пределами engine/src/rhi/ во всём движке НОЛЬ вызовов gl*, единственное
// исключение — Window.cpp, которому нужен контекст от оконной системы. Эту
// границу держит проверка (см. scripts/check_rhi_boundary.py), она гоняется в
// CI, и Null-бэкенд доказывает её собой: он реализует тот же интерфейс, ничего
// не рисуя, и движок с ним собирается и работает.
//
// ЧТО ЗДЕСЬ БЫЛО НЕПРАВДОЙ. Раньше тут стояло: «чтобы добавить новый бэкенд,
// реализуют этот интерфейс — код движка и игр не переписывается». Для Vulkan,
// D3D12 и Metal это НЕ ТАК, и обещать такое нечестно.
//
// Форма интерфейса ниже — это машина состояний OpenGL: SetBlend, SetDepthTest,
// SetCullMode, BindTexture2D. Современные API устроены иначе: там команды
// пишутся в буферы, состояние
// собирается в объект конвейера заранее, ресурсы адресуются наборами
// дескрипторов, а синхронизация задаётся явно барьерами. Реализовать
// «SetDepthTest» поверх Vulkan можно, но это будет эмуляция состояния с
// пересборкой конвейеров на лету — то есть худшее из обоих миров.
//
// ЧЕСТНАЯ ОЦЕНКА: Vulkan-бэкенд потребует переписать МОДЕЛЬ этого интерфейса
// (команды, проходы, дескрипторы), а вместе с ней и места, где движок эту
// модель использует, — прежде всего порядок проходов кадра. Null-бэкенд
// доказывает изоляцию, а не переносимость: он выдержан в той же GL-подобной
// форме и потому не является свидетельством, что форма годится для Vulkan.
//
// СОГЛАШЕНИЯ RHI. Ниже — то, о чём интерфейс раньше молчал, а бэкенд решал по
// привычкам OpenGL. Молчание здесь опаснее любой протечки типа: несовпадение
// соглашений не ломает сборку, а даёт перевёрнутую картинку или расползшиеся
// тени на втором бэкенде, и ищется это неделями, потому что «на OpenGL всё
// работает». Каждый пункт — обязательство бэкенда пересчитать у себя, а не
// требование к вызывающему:
//
//   • Начало отсчёта прямоугольников (viewport, scissor, ReadPixels) — ЛЕВЫЙ
//     НИЖНИЙ угол. У D3D и Vulkan оно сверху; пересчёт — забота бэкенда.
//   • Строки ReadPixelsRGB идут СНИЗУ ВВЕРХ.
//   • Начало координат текстуры — левый нижний угол (V растёт вверх). Загрузчик
//     изображений переворачивает пиксели при загрузке, шейдеры это уже знают.
//   • Диапазон глубины в NDC — [-1, 1]. Это соглашение GL; у Vulkan/D3D он
//     [0, 1], и бэкенд обязан привести матрицы проекции к своему. Записано
//     здесь потому, что молчание об этом означало бы, что глубина «почти
//     работает»: близкие объекты правильно, дальние — со сдвигом.
//   • Матрицы — по столбцам (как в glm и GLSL).
//
// ЕЩЁ ДВА ОГРАНИЧЕНИЯ, о которых надо знать заранее:
//   • OpenGL 3.3 — это отсутствие вычислительных шейдеров (появились в 4.3).
//     Всё, что естественно ложится на compute (свёртки, отбор на GPU,
//     частицы), приходится делать через фрагментные проходы или на CPU.
//   • macOS объявил OpenGL устаревшим и остановился на 4.1. Порт на Apple
//     означает Metal или MoltenVK, то есть тот же разговор про модель.
//
// Интерфейс покрывает два уровня:
//   1) УСТРОЙСТВО: инициализация драйвера/контекста и состояние конвейера
//      (viewport, очистка, blend/depth/cull, экранный буфер, чтение пикселей).
//   2) РЕСУРСЫ: фабрики GPU-объектов (шейдеры, геометрия, текстуры,
//      рендер-таргеты) — см. sage/rhi/Resources.h.
// ---------------------------------------------------------------------------
namespace sage::rhi {

enum class Backend {
    OpenGL,
    // Null — устройство без графического API. Не заглушка «на будущее», а
    // рабочий бэкенд: он доказывает, что движок нигде за пределами rhi/ не
    // рассчитывает на OpenGL, и даёт тестам сцену без видеокарты.
    Null,
    // Vulkan, D3D12, Metal — см. честную оценку выше: это переписывание
    // модели, а не добавление реализации.
};

// Резолвер адресов функций графического API (даёт оконная система, напр. GLFW
// через glfwGetProcAddress). Бэкенд использует его в Init() для загрузки драйвера.
using ProcLoader = void* (*)(const char*);

class GraphicsDevice {
public:
    virtual ~GraphicsDevice() = default;

    // Загружает драйвер (через loader) и выставляет дефолтное состояние
    // конвейера, общее для всего движка (тест глубины, отсечение задних граней,
    // бесшовные cubemap). Вызывается один раз после создания окна/контекста.
    virtual void Init(ProcLoader loader) = 0;

    virtual const char* BackendName() const = 0;
    virtual std::string ApiVersion() const = 0;

    // --- Состояние кадра / конвейера ---
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetClearColor(float r, float g, float b, float a) = 0;
    virtual void Clear(bool color = true, bool depth = true) = 0;

    // Привязать экранный (default) кадровый буфер как цель отрисовки.
    virtual void BindDefaultFramebuffer() = 0;

    // Стандартный alpha-blending (src_alpha, 1 - src_alpha) вкл/выкл.
    virtual void SetBlend(bool enabled) = 0;
    virtual void SetDepthTest(bool enabled) = 0;
    virtual void SetDepthWrite(bool enabled) = 0;
    virtual void SetDepthFunc(DepthFunc func) = 0;      // LessEqual нужен skybox'у
    virtual void SetCullMode(CullMode mode) = 0;        // Front — depth-проход теней
    virtual void SetPolygonMode(PolygonMode mode) = 0;  // Line — каркасный рендер (wireframe)

    // Ножницы (scissor): пиксели вне прямоугольника не пишутся. Используется UI
    // для клип-масок (обрезка детей контейнера).
    //
    // (x, y) — ЛЕВЫЙ НИЖНИЙ угол в пикселях фреймбуфера. Это соглашение RHI, а
    // не «как в glScissor»: формулировка через один API означала бы, что второй
    // бэкенд волен понимать её иначе, и клип-маски интерфейса тихо перевернулись
    // бы по вертикали. У D3D и Vulkan начало отсчёта сверху — бэкенд обязан
    // пересчитать сам, зная высоту цели, а не переложить это на вызывающего.
    virtual void SetScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) = 0;

    // Аппаратное sRGB-кодирование при записи в текущий фреймбуфер (гамма-
    // коррекция «бесплатно» на выводе). Включается на время рендера СЦЕНЫ
    // напрямую в экран, когда HDR-пост-процесса нет (он делал гамму сам):
    // без этого линейный цвет шейдеров уходит на монитор сырым и картинка
    // выглядит неоправданно тёмной. Выключать перед UI/текстом — их цвета
    // уже в sRGB и повторное кодирование их пересветит.
    virtual void SetSRGBWrite(bool enabled) = 0;

    // Привязать 2D-текстуру по хендлу (их отдают Texture2D::Handle и
    // RenderTarget::ColorTextureHandle/DepthTextureHandle) — для сэмплирования
    // вложений рендер-таргетов (карта теней, HDR-сцена) обычными шейдерами.
    // Невалидный хендл означает «отвязать»: юнит остаётся без текстуры.
    virtual void BindTexture2D(int unit, TextureHandle texture) = 0;

    // Дождаться завершения всех команд GPU и прочитать прямоугольник пикселей
    // текущего буфера как плотный RGB8 (для скриншотов).
    //
    // Строки идут СНИЗУ ВВЕРХ: первая строка out — нижняя строка изображения.
    // Соглашение RHI, и записано оно здесь потому, что молчание о нём стоило бы
    // перевёрнутых скриншотов на втором бэкенде — дефекта, который не воспроизво-
    // дится на первом и потому ищется долго.
    virtual void ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) = 0;

    // Максимальная поддерживаемая анизотропия фильтрации (>= 1.0).
    virtual float MaxAnisotropy() = 0;

    // --- Таймеры GPU -------------------------------------------------------
    //
    // Метки времени, а НЕ «замер интервала». Интервальный запрос в OpenGL
    // может быть активен только один за раз, то есть вложенные проходы им не
    // измерить: тень внутри кадра, отдельный эффект внутри пост-обработки.
    // Метки вкладываются как угодно — длительность считается вычитанием.
    //
    // Результат забирается ЧЕРЕЗ НЕСКОЛЬКО КАДРОВ. Спросить его сразу значит
    // дождаться, пока видеокарта закончит, — то есть своим измерением испортить
    // то, что измеряешь, и заодно убить параллельность CPU и GPU.
    //
    // Реализация не обязательна: бэкенд без таймеров возвращает 0 и не мешает
    // работать. Профилировщик в этом случае показывает только время CPU.
    // Запись цвета. Выключается на проходах, которым нужна только глубина:
    // проверка перекрытия рисует коробки-заменители, и попадать в кадр они не
    // должны — нужен только факт «прошли ли пиксели тест глубины».
    virtual void SetColorWrite(bool) {}

    // --- Запросы перекрытия (occlusion queries) ----------------------------
    //
    // Считают, сколько пикселей нарисованного прошло тест глубины. Ноль
    // означает, что объект целиком закрыт уже нарисованной геометрией, и в
    // следующем кадре его можно не рисовать.
    //
    // Результат, как и у таймеров, забирается ЧЕРЕЗ КАДР. Спросить сразу —
    // значит дождаться видеокарты и убить весь смысл: экономия на отрисовке
    // обошлась бы дороже простоя на ожидании.
    //
    // GL_ANY_SAMPLES_PASSED, а не точный счёт пикселей: нам нужен только факт
    // «видно/не видно», а приблизительный вариант драйверу дешевле.
    virtual QueryHandle CreateOcclusionQuery() { return {}; }
    virtual void DestroyOcclusionQuery(QueryHandle) {}
    virtual void BeginOcclusionQuery(QueryHandle) {}
    virtual void EndOcclusionQuery() {}
    virtual bool OcclusionResultReady(QueryHandle) { return false; }
    virtual bool OcclusionVisible(QueryHandle) { return true; }
    virtual bool SupportsOcclusionQueries() const { return false; }

    virtual QueryHandle CreateTimestampQuery() { return {}; }
    virtual void DestroyTimestampQuery(QueryHandle) {}
    virtual void WriteTimestamp(QueryHandle) {}
    virtual bool TimestampReady(QueryHandle) { return false; }
    virtual unsigned long long TimestampNs(QueryHandle) { return 0; }
    virtual bool SupportsGpuTimers() const { return false; }

    // --- Фабрики GPU-ресурсов (см. sage/rhi/Resources.h) ---
    virtual std::unique_ptr<ShaderProgram> CreateShaderProgram(const std::string& vertexSrc,
                                                               const std::string& fragmentSrc) = 0;
    virtual std::unique_ptr<Geometry> CreateGeometry(const VertexLayout& layout) = 0;
    virtual std::unique_ptr<Texture2D> CreateTexture2D(const Texture2DDesc& desc, const void* pixels) = 0;
    // Объёмная float-текстура (GI-объём проб). pixels — плотный массив
    // Width*Height*Depth*Channels float'ов (может быть nullptr).
    virtual std::unique_ptr<Texture3D> CreateTexture3D(const Texture3DDesc& desc, const float* pixels) = 0;
    virtual std::unique_ptr<TextureCube> CreateTextureCube(const CubeFacePixels faces[6]) = 0;
    virtual std::unique_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) = 0;
    // Кубический таргет для отражений (см. CubeRenderTarget). Не чисто
    // виртуальный: бэкенд, который его не умеет, вернёт nullptr, и отражения
    // просто отключатся, а не уронят сборку.
    virtual std::unique_ptr<CubeRenderTarget> CreateCubeRenderTarget(const CubeRenderTargetDesc&) {
        return nullptr;
    }

    // Фабрика бэкенда. Возвращает готовый (но ещё не Init) девайс.
    static std::unique_ptr<GraphicsDevice> Create(Backend backend);

    // Текущий девайс процесса — ставится Application после создания. Даёт
    // подсистемам доступ к устройству без протаскивания указателя повсюду.
    static GraphicsDevice& Get();
    static void SetCurrent(GraphicsDevice* device);
};

} // namespace sage::rhi
