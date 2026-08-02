#include "sage/rhi/Conformance.h"

#include <string>
#include <vector>

#include "sage/rhi/GraphicsDevice.h"

namespace sage::rhi {
namespace {

struct Runner {
    ConformanceResult Result;

    void Check(bool ok, const char* name, const std::string& detail = {}) {
        ++Result.Checked;
        if (ok) return;
        Result.Failures.push_back(detail.empty() ? std::string(name)
                                                 : std::string(name) + ": " + detail);
    }
    void Skip() { ++Result.Skipped; }
};

// --- Уровень 1: структура ---------------------------------------------------

void CheckTextures(GraphicsDevice& device, Runner& r) {
    Texture2DDesc desc;
    desc.Width = 4;
    desc.Height = 4;
    desc.Channels = 4;
    desc.GenerateMipmaps = false;
    const unsigned char pixels[4 * 4 * 4] = {};

    auto a = device.CreateTexture2D(desc, pixels);
    auto b = device.CreateTexture2D(desc, pixels);
    r.Check(a != nullptr && b != nullptr, "CreateTexture2D возвращает объект");
    if (!a || !b) return;

    r.Check(a->Handle().Valid(), "хендл созданной текстуры валиден");
    // Два разных объекта — два разных хендла. Проверка выглядит очевидной ровно
    // до первой реализации с кэшем или пулом, где «одинаковое описание» легко
    // превращается в один и тот же объект — и тогда удаление одной текстуры
    // забирает с собой чужую.
    r.Check(a->Handle() != b->Handle(), "разные текстуры — разные хендлы");
    r.Check(a->NativeHandle() != 0, "нативный хендл текстуры ненулевой");
}

void CheckRenderTargets(GraphicsDevice& device, Runner& r) {
    RenderTargetDesc desc;
    desc.Width = 16;
    desc.Height = 8;
    desc.Kind = RenderTargetKind::ColorHDRWithDepth;

    auto rt = device.CreateRenderTarget(desc);
    r.Check(rt != nullptr, "CreateRenderTarget возвращает объект");
    if (!rt) return;

    r.Check(rt->Width() == 16 && rt->Height() == 8, "размер таргета совпадает с описанием",
            std::to_string(rt->Width()) + "x" + std::to_string(rt->Height()));
    r.Check(rt->Samples() >= 1, "Samples() не меньше единицы");
    r.Check(rt->ColorTextureHandle().Valid(), "у цветного таргета есть цветное вложение");

    // Resolve обязан быть безопасен ВСЕГДА — в том числе на таргете без MSAA.
    // Иначе каждый вызывающий обязан помнить, включён ли там MSAA, и однажды
    // забудет; ради этого он и объявлен пустышкой по умолчанию.
    rt->Resolve();
    r.Check(true, "Resolve() безопасен на таргете без MSAA");

    rt->Resize(32, 4);
    r.Check(rt->Width() == 32 && rt->Height() == 4, "Resize меняет размер",
            std::to_string(rt->Width()) + "x" + std::to_string(rt->Height()));
    // Повторный Resize тем же размером — no-op по контракту, и звать его каждый
    // кадр должно быть бесплатно и безвредно.
    rt->Resize(32, 4);
    r.Check(rt->Width() == 32 && rt->Height() == 4, "повторный Resize тем же размером безвреден");

    RenderTargetDesc depthOnly;
    depthOnly.Width = 8;
    depthOnly.Height = 8;
    depthOnly.Kind = RenderTargetKind::DepthOnly;
    auto shadow = device.CreateRenderTarget(depthOnly);
    r.Check(shadow != nullptr, "DepthOnly-таргет создаётся");
    if (shadow) {
        r.Check(shadow->DepthTextureHandle().Valid(),
                "у DepthOnly-таргета есть глубинное вложение");
    }
}

void CheckCubeTargets(GraphicsDevice& device, Runner& r) {
    CubeRenderTargetDesc desc;
    desc.Size = 16;
    desc.WithDepth = true;
    desc.MipLevels = 0; // полная цепочка

    auto cube = device.CreateCubeRenderTarget(desc);
    if (!cube) {
        // nullptr — ЛЕГАЛЬНЫЙ ответ: бэкенд без кубических таргетов просто
        // отключает отражения, а не роняет сборку (см. GraphicsDevice.h).
        r.Skip();
        return;
    }
    r.Check(cube->Size() == 16, "сторона кубического таргета совпадает с описанием");
    r.Check(cube->MipLevels() >= 1, "мип-уровней не меньше одного");
    // Полная цепочка от 16 — это пять уровней (16,8,4,2,1). Меньше означало бы,
    // что шероховатость материала выбирает мип, которого нет.
    r.Check(cube->MipLevels() == 5, "MipLevels=0 даёт полную цепочку",
            std::to_string(cube->MipLevels()));
    cube->Bind(0);
    r.Check(true, "Bind кубического таргета безопасен");
}

void CheckQueries(GraphicsDevice& device, Runner& r) {
    // Контракт запросов держится на одном правиле: бэкенд, который их НЕ умеет,
    // обязан оставаться безопасным. Иначе каждый вызывающий обязан спрашивать
    // Supports* перед каждым вызовом — и половина забудет.
    if (device.SupportsOcclusionQueries()) {
        QueryHandle q = device.CreateOcclusionQuery();
        r.Check(q.Valid(), "бэкенд с запросами перекрытия выдаёт валидный хендл");
        device.DestroyOcclusionQuery(q);
    } else {
        r.Skip();
    }

    // Невалидный хендл не должен ронять НИЧЕГО: так выглядит путь, где запрос
    // не удалось создать, и он обязан быть проходимым.
    const QueryHandle bad;
    device.DestroyOcclusionQuery(bad);
    device.BeginOcclusionQuery(bad);
    device.EndOcclusionQuery();
    r.Check(!device.OcclusionResultReady(bad), "невалидный запрос никогда не «готов»");
    r.Check(device.OcclusionVisible(bad),
            "невалидный запрос считается ВИДИМЫМ — безопасная сторона ошибки");

    if (device.SupportsGpuTimers()) {
        QueryHandle t = device.CreateTimestampQuery();
        r.Check(t.Valid(), "бэкенд с таймерами выдаёт валидный хендл метки");
        device.DestroyTimestampQuery(t);
    } else {
        r.Skip();
    }
    device.WriteTimestamp(bad);
    r.Check(!device.TimestampReady(bad), "невалидная метка никогда не «готова»");
    r.Check(device.TimestampNs(bad) == 0, "невалидная метка даёт нулевое время");
}

void CheckStateIsSafe(GraphicsDevice& device, Runner& r) {
    // Отвязка текстуры невалидным хендлом — документированное поведение, и
    // вызывается оно постоянно: выключенный эффект, отсутствующая карта.
    device.BindTexture2D(0, TextureHandle{});
    r.Check(true, "BindTexture2D с невалидным хендлом отвязывает, а не падает");

    device.SetScissor(false);
    device.SetBlend(false);
    device.SetDepthTest(true);
    device.SetDepthWrite(true);
    device.SetCullMode(CullMode::Back);
    device.SetPolygonMode(PolygonMode::Fill);
    device.SetSRGBWrite(false);
    r.Check(true, "состояние конвейера выставляется без исключений");

    r.Check(device.MaxAnisotropy() >= 1.0f, "MaxAnisotropy не меньше единицы",
            std::to_string(device.MaxAnisotropy()));
    r.Check(device.BackendName() != nullptr && device.BackendName()[0] != '\0',
            "бэкенд называет себя");
}

// --- Уровень 2: пиксели -----------------------------------------------------

// Соглашения о системе координат проверяются единственным способом — рисованием.
//
// Что здесь проверяется на самом деле: СОГЛАСОВАННОСТЬ начала отсчёта ножниц с
// порядком строк при чтении. Абсолютную ориентацию задают эталонные картинки
// рендер-тестов; здесь — то, что новый бэкенд не перевернёт одно относительно
// другого. Перепутанное по отдельности даёт скриншот вверх ногами или маску
// интерфейса, обрезающую не тот край, — дефекты, которые ищут долго именно
// потому, что «на старом бэкенде всё работало».
void CheckPixelConventions(GraphicsDevice& device, Runner& r) {
    constexpr int kW = 8;
    constexpr int kH = 8;

    RenderTargetDesc desc;
    desc.Width = kW;
    desc.Height = kH;
    desc.Kind = RenderTargetKind::ColorHDR;
    auto rt = device.CreateRenderTarget(desc);
    if (!rt) {
        r.Skip();
        return;
    }

    rt->Bind();
    device.SetScissor(false);
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, false);

    // Красим ТОЛЬКО левый нижний квадрант. Ножницы, а не отдельная геометрия:
    // очистка обязана их уважать, и это тоже часть контракта.
    device.SetScissor(true, 0, 0, kW / 2, kH / 2);
    device.SetClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, false);
    device.SetScissor(false);

    std::vector<unsigned char> pixels((size_t)kW * kH * 3, 0);
    device.ReadPixelsRGB(0, 0, kW, kH, pixels.data());

    auto red = [&](int col, int row) {
        const size_t i = ((size_t)row * kW + (size_t)col) * 3;
        return pixels[i] > 128 && pixels[i + 1] < 128 && pixels[i + 2] < 128;
    };

    // Первая строка буфера — НИЖНЯЯ строка изображения, значит закрашенный
    // квадрант лежит именно в ней и именно слева.
    r.Check(red(0, 0), "ножницы и чтение сходятся в левом нижнем углу");
    r.Check(!red(kW - 1, 0), "правый нижний угол не закрашен");
    r.Check(!red(0, kH - 1), "левый верхний угол не закрашен");
    r.Check(!red(kW - 1, kH - 1), "правый верхний угол не закрашен");

    device.BindDefaultFramebuffer();
}

} // namespace

ConformanceResult RunConformance(GraphicsDevice& device, const ConformanceOptions& options) {
    Runner r;
    CheckStateIsSafe(device, r);
    CheckTextures(device, r);
    CheckRenderTargets(device, r);
    CheckCubeTargets(device, r);
    CheckQueries(device, r);
    if (options.Rasterizes) CheckPixelConventions(device, r);
    else r.Skip();
    return r.Result;
}

} // namespace sage::rhi
