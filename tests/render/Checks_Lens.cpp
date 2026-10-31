// ---------------------------------------------------------------------------
// Хроматическая аберрация: кайма по радиусу объектива, без двоения, без
// размазывания края кадра, с чистым центром и силой без потолка.
//
// Старое звено брало три смещённые копии кадра. При заметной силе контур
// распадался на три силуэта с чёрными щелями между ними, красный читался из-за
// края кадра, а сила упиралась в 1% кадра. Проверки ниже на старом коде падают.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cstdio>
#include <cstdlib>

#include "sage/render/Framebuffer.h"
#include "sage/render/PostEffect.h"
#include "sage/render/PostFX.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rendertest {
namespace {

// Чёрный кадр с белыми полосками: вертикальная у правого края и в центре.
void PaintBars(Framebuffer& fb) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    fb.Bind();
    device.SetViewport(0, 0, fb.Width(), fb.Height());
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    device.SetClearColor(4.0f, 4.0f, 4.0f, 1.0f);
    device.SetScissor(true, fb.Width() * 9 / 10, 0, 3, fb.Height()); // у края
    device.Clear(true, false);
    device.SetScissor(true, fb.Width() / 2 - 1, 0, 3, fb.Height()); // в центре
    device.Clear(true, false);
    device.SetScissor(false);
}

Image RunAberration(float amount, float start) {
    Framebuffer src(kW, kH), out(kW, kH);
    PaintBars(src);
    sage::render::PostChain chain;
    AddEffect(chain, "chromatic");
    SetParam(chain, "chromatic", "amount", amount);
    SetParam(chain, "chromatic", "start", start);
    sage::render::PostFX fx;
    fx.Render(src.ColorTexture(), src.DepthTexture(), kW, kH, glm::mat4(1.0f), glm::mat4(1.0f), chain,
              &out, 0, 0, kW, kH);
    out.Bind();
    Image img = Capture(kW, kH);
    sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
    return img;
}

const unsigned char* Px(const Image& img, int x, int y) { return &img.Pixels[((size_t)y * img.Width + x) * 3]; }

} // namespace

void RunLensChecks() {
    std::printf("=== Хроматическая аберрация ===\n");
    const int row = kH / 2;
    const Image strong = RunAberration(3.0f, 0.0f);

    // Кайма у края: синий конец спектра увеличен меньше красного, поэтому от
    // полоски НАРУЖУ (к краю кадра) тянется цветной след — сплошной, без
    // чёрных щелей между «копиями».
    const int bar = kW * 9 / 10;
    int lit = 0, holes = 0;
    bool seen = false, ended = false;
    for (int x = bar; x < kW; ++x) {
        const unsigned char* p = Px(strong, x, row);
        const bool on = p[0] + p[1] + p[2] > 24;
        if (on && ended) ++holes;
        if (on) {
            seen = true;
            ++lit;
        } else if (seen) {
            ended = true;
        }
    }
    const unsigned char* fringe = Px(strong, bar + 6, row);
    std::printf("    у края: светлых пикселей %d, разрывов %d, цвет каймы %d %d %d\n", lit, holes,
                fringe[0], fringe[1], fringe[2]);
    Check(lit > 6, "сила выше 1 даёт заметную кайму (раньше потолок — 1% кадра)");
    Check(holes == 0, "кайма сплошная: контур не распадается на три копии");
    Check(fringe[0] + 20 < fringe[2] || fringe[2] + 20 < fringe[0], "кайма цветная, а не серая");

    // Центр: полоска в центре кадра остаётся белой (каналы не расходятся).
    const Image clean = RunAberration(3.0f, 0.3f);
    const unsigned char* c = Px(clean, kW / 2, row);
    const unsigned char* l = Px(clean, kW / 2 - 3, row);
    std::printf("    центр: полоска %d %d %d, рядом %d %d %d\n", c[0], c[1], c[2], l[0], l[1], l[2]);
    Check(std::abs(c[0] - c[2]) < 10 && l[0] + l[1] + l[2] < 24, "центр кадра без аберрации");

    // Край кадра не размазывается: крайний столбец остаётся тёмным.
    const unsigned char* e = Px(strong, kW - 1, row);
    Check(e[0] + e[1] + e[2] < 24, "выборка не выходит за кадр — край не размазан");
}

} // namespace sage::rendertest
