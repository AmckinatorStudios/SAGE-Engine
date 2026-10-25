#include "sage/render/PostFX.h"
#include "sage/render/PostFXDetail.h"

#include <chrono>
#include <cmath>

#include "sage/core/Log.h"
#include "sage/core/Profiler.h"

#include <algorithm>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/render/LensFlare.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

namespace sage::render {

bool SceneWantsGBuffer(const sage::EngineConfig& cfg) {
    // Сейчас G-буфер читает только экранный GI: без него платить за два
    // лишних вложения (и за их перенос при MSAA) незачем.
    return cfg.ScreenSpaceGI;
}

int SceneSamples(const sage::EngineConfig& cfg) {
    // Ступени растеризатора: всё, что между ними, округляем ВНИЗ — обещать
    // человеку 8x и молча дать 4x хуже, чем дать ровно то, что он выбрал из
    // доступного. Выше 8 не поднимаемся: выигрыш уже неразличим, а цена растёт
    // линейно по памяти буфера.
    const int m = cfg.Msaa;
    if (m >= 8) return 8;
    if (m >= 4) return 4;
    if (m >= 2) return 2;
    return 1;
}

using namespace postfx;

PostFX::PostFX() {
    m_scratch.Fullscreen = GraphicsDevice::Get().CreateGeometry(VertexLayout{});
}

void PostFX::EnsureTargets(int w, int h) {
    if (w == m_scratch.Width && h == m_scratch.Height && m_scratch.Ao) return;
    m_scratch.Width = w;
    m_scratch.Height = h;
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2); // свечение — половинное
    m_scratch.Ao = MakeColor(w, h);
    m_scratch.AoBlur = MakeColor(w, h);
    m_scratch.Bright = MakeColor(hw, hh);
    m_scratch.BloomA = MakeColor(hw, hh);
    m_scratch.BloomB = MakeColor(hw, hh);
    // Смена размера обесценивает рабочие буферы, пул целей и историю кадра:
    // репроецировать старую матрицу на другой кадр нельзя, а цель другого
    // размера не подходит ни одному звену.
    m_scratch.GiTrace.reset();
    m_scratch.GiBlur.reset();
    m_scratch.DofPrep.reset();
    m_scratch.DofBlur.reset();
    m_scratch.DofNearMaxA.reset();
    m_scratch.DofNearMaxB.reset();
    m_scratch.DofNear.reset();
    m_scratch.Pool.clear();
    m_scratch.HasPrevFrame = false;
}

RenderTarget* PostFX::EnsureAux(std::unique_ptr<RenderTarget>& slot, int w, int h) {
    return Keep(slot, w > 0 ? w : m_scratch.Width, h > 0 ? h : m_scratch.Height);
}

void PostFX::BlitToOutput(sage::rhi::TextureHandle source, Framebuffer* output, int outX, int outY,
                          int outW, int outH) {
    GraphicsDevice& device = GraphicsDevice::Get();
    // Состояние, от которого зависит проход, проход и выставляет: копия
    // вызывается и из тракта, и сама по себе — например хостом, у которого
    // пост-обработка выключена конфигом, или тестом «с обработкой и без».
    device.SetDepthTest(false);
    device.SetBlend(false);
    if (output) {
        output->Bind();
    } else {
        device.BindDefaultFramebuffer();
        device.SetViewport(outX, outY, outW, outH);
    }
    Shader& copy = CopyShader();
    copy.Use();
    copy.SetInt("uScene", 0);
    device.BindTexture2D(0, source);
    m_scratch.Fullscreen->DrawArrays(3);
}

void PostFX::Render(sage::rhi::TextureHandle sceneColor, sage::rhi::TextureHandle sceneDepth,
                    int w, int h, const glm::mat4& proj, const glm::mat4& view,
                    const PostChain& chain, Framebuffer* output, int outX, int outY, int outW,
                    int outH, sage::rhi::TextureHandle velocityTexture) {
    SAGE_PROFILE("Пост-обработка");
    GraphicsDevice& device = GraphicsDevice::Get();
    EnsureTargets(w, h);
    device.SetDepthTest(false); // все проходы — полноэкранные, глубина не нужна
    // СМЕШИВАНИЕ ВЫКЛЮЧАЕТСЯ ЯВНО, а не предполагается выключенным.
    //
    // Каждый проход этой цепочки обязан ЗАМЕНИТЬ содержимое своей цели: он
    // читает предыдущий буфер и пишет следующий. Со включённым смешиванием он
    // вместо этого подмешивается к тому, что в цели уже лежит, — а лежит там
    // результат ПРОШЛОГО КАДРА: буферы затенения, свечения и промежуточные
    // цели живут между кадрами и намеренно не очищаются (их всё равно
    // перекрывает полноэкранный треугольник).
    //
    // Получалась положительная обратная связь: кадр темнел на несколько
    // процентов, следующий брал уже потемневший результат и темнел ещё, и так
    // без конца. Замер на неподвижной сцене: яркость 105 -> 87 -> 78 -> 75...
    // Снаружи это и есть жалоба «сначала рендер работает нормально, а потом без
    // всякой причины начинает деградировать»; отладочные режимы вида при этом
    // остаются правильными, потому что идут мимо пост-обработки, — и поломка
    // выглядит необъяснимой.
    //
    // Кто именно оставил смешивание включённым, значения не имеет: этой цепочке
    // нельзя зависеть от порядка чужих проходов. Состояние, от которого зависит
    // проход, проход и выставляет — ровно как строкой выше с глубиной.
    device.SetBlend(false);

    // ТРАКТ ДОПОЛНЯЕТСЯ И ПРОВЕРЯЕТСЯ ДО ОТРИСОВКИ.
    //
    // Порядок звеньев — данные, и он может быть неверным: свечение после
    // тон-маппинга, сглаживание до него, два тон-маппинга. Заметить это «по
    // картинке» нельзя: картинка будет просто неверной, без единого сообщения.
    // Поэтому тракт сперва дополняется до рабочего (нет тон-маппинга — он
    // вставляется, см. PostChain::Completed), затем компилируется и только потом
    // исполняется.
    // ПО ЭТАПАМ, а не по порядку списка: что человек видит разделами в
    // инспекторе, то и выполняется здесь — см. PostChain::Ordered.
    PostChain run = chain.Completed().Ordered();
    const PostChainReport report = run.Compile();
    if (!report.Ok) {
        // Неверный тракт НЕ оставляет чёрный экран: кадр показывается с одним
        // тон-маппингом, а причина уходит в лог. Отказ показать картинку
        // из-за опечатки в настройке — самый дорогой исход: человек увидит
        // чёрное и решит, что сломан рендер, а не тракт.
        OnceLog("тракт пост-обработки неверен: " + report.Error +
                " — кадр показан без эффектов");
        run.Effects.clear();
        run.Effects.push_back(MakePostEffect("tonemap"));
    }

    // Контекст кадра — ВСЁ, что звено вправе знать. Ни очереди эффектов, ни
    // числа проходов, ни внутренностей исполнителя здесь нет: звено должно
    // уметь работать, ничего не зная о том, кто стоит до и после него.
    PostContext ctx;
    ctx.Device = &device;
    ctx.Width = w;
    ctx.Height = h;
    ctx.Proj = proj;
    ctx.View = view;
    ctx.SceneColor = sceneColor;
    ctx.SceneDepth = sceneDepth;
    ctx.SceneAlbedo = m_gAlbedo;
    ctx.SceneNormal = m_gNormal;
    ctx.Velocity = velocityTexture;
    ctx.Color = sceneColor;
    ctx.ColorIsLdr = false;
    ctx.Scratch = &m_scratch;
    ctx.Lighting = m_lighting;
    // Время — от первого кадра процесса, а не от начала эпохи: у секунд с 1970
    // года не хватает точности float, и зерно плёнки застывало бы на месте.
    {
        static const auto start = std::chrono::steady_clock::now();
        ctx.Time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    }

    const glm::mat4 viewProj = proj * view;

    for (const PostEffect& effect : run.Effects) {
        if (!effect.Enabled) continue;
        const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
        if (!kind || !kind->Run) continue;

        // Чего не хватает КАДРУ, а не тракту: глубины может не быть вовсе, если
        // сцену нарисовали без буфера глубины. Это не ошибка автора тракта, и
        // звено пропускается — но с причиной, а не молча.
        if (Has(kind->Needs, PostNeeds::Depth) && !ctx.SceneDepth.Valid()) {
            OnceLog("звено «" + kind->Label + "» пропущено: у кадра нет текстуры глубины");
            continue;
        }
        // Эти два случая компилятор уже отверг; здесь они лишь страховка от
        // расхождения между проверкой и исполнением.
        if (Has(kind->Needs, PostNeeds::HdrColor) && ctx.ColorIsLdr) continue;
        if (Has(kind->Needs, PostNeeds::LdrColor) && !ctx.ColorIsLdr) continue;

        kind->Run(ctx, effect);
    }

    // ФИНАЛЬНАЯ КОПИЯ. Звено пишет в СВОЮ цель, а не в выход: оно не обязано
    // знать, куда смотрит камера. Копия стоит один проход и одно чтение
    // текстуры, зато одно и то же звено годится и для превью в редакторе, и для
    // экрана игры, и для второго вьюпорта другого размера.
    BlitToOutput(ctx.Color, output, outX, outY, outW, outH);

    device.SetDepthTest(true);

    // История для смаза следующего кадра. Пишется ВСЕГДА, даже когда смаз
    // выключен: иначе после его включения первый кадр сравнивался бы с давно
    // устаревшей матрицей и размазал бы весь экран.
    m_scratch.PrevViewProj = viewProj;
    m_scratch.HasPrevFrame = true;
}


// --- Проверка цепочки на этой видеокарте -------------------------------------
//
// Прогоняем известную картинку и смотрим, что вышло. Ровный средний серый —
// вход, у которого правильный ответ известен без спора: тон-маппинг его
// подвинет, но чёрным и белым он не станет ни при каких настройках.
//
// Проверяется ровно то, что ломается: не «совпал ли пиксель», а «не пропала ли
// картинка». Сравнивать с эталоном тут нельзя — у каждой видеокарты своя
// фильтрация и свой порядок операций, и любой такой эталон дал бы ложную
// тревогу на первой же чужой машине.
const PostFX::SelfCheck& PostFX::CheckPipeline() {
    if (m_selfCheck.Ran) return m_selfCheck;
    m_selfCheck.Ran = true;

    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    constexpr int kSize = 32;

    // Вход: ровный серый, непрозрачный.
    std::vector<unsigned char> gray((size_t)kSize * kSize * 4, 128);
    for (size_t i = 3; i < gray.size(); i += 4) gray[i] = 255;
    sage::rhi::Texture2DDesc desc;
    desc.Width = desc.Height = kSize;
    desc.Channels = 4;
    desc.FilterMode = sage::rhi::Filter::Bilinear;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = false;
    std::unique_ptr<sage::rhi::Texture2D> input = dev.CreateTexture2D(desc, gray.data());
    if (!input) {
        m_selfCheck.Reason = "не удалось создать пробную текстуру";
        return m_selfCheck;
    }

    Framebuffer target(kSize, kSize);
    // Тракт для проверки — ПУСТОЙ: он означает «только тон-маппинг», а все
    // эффекты, читающие глубину, в этой пробе и не нужны (глубины тут нет).
    // Дополнение до рабочего делает сам исполнитель (PostChain::Completed).
    const PostChain chain;
    Render(input->Handle(), sage::rhi::TextureHandle{}, kSize, kSize, glm::mat4(1.0f),
           glm::mat4(1.0f), chain, &target, 0, 0, kSize, kSize);

    target.Resolve();
    target.Bind();
    std::vector<unsigned char> out((size_t)kSize * kSize * 3, 0);
    dev.ReadPixelsRGB(0, 0, kSize, kSize, out.data());
    dev.BindDefaultFramebuffer();

    double sum = 0.0;
    for (unsigned char c : out) sum += c;
    const double mean = sum / (double)out.size();

    // Порог низкий намеренно: ловим ПРОПАЖУ картинки, а не отличие в оттенке.
    // Серый после любого разумного тон-маппинга остаётся заметно светлее нуля.
    if (mean < 4.0) {
        m_selfCheck.Reason = "цепочка вернула чёрный кадр на заведомо сером входе";
        return m_selfCheck;
    }
    m_selfCheck.Ok = true;
    return m_selfCheck;
}

} // namespace sage::render
