// ---------------------------------------------------------------------------
// Свой рендер на настоящем драйвере: вычислительные шейдеры, буферы, запись в
// текстуры, граф кадра со своими проходами, ошибки шейдеров по файлам и
// строкам, правка на лету, встроенный PBR как один из рендереров.
//
// Модульные тесты (tests/test_render_pipeline.cpp) проверяют то же устройство
// кадра на пустышке; здесь — то, что пустышка подтвердить не может: что
// драйвер понял наши директивы #line, что барьер после вычислений делает
// записанное видимым, что встроенный PBR через граф даёт картинку.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/render/EffectChain.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/RenderGraph.h"
#include "sage/render/RenderPipeline.h"
#include "sage/render/ShaderLibrary.h"
#include "sage/render/ShaderMaterial.h"
#include "sage/render/StandardPipeline.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rendertest {
namespace {

using namespace sage::render;
namespace rhi = sage::rhi;

void ReadCenter(Framebuffer& fb, unsigned char rgb[3]) {
    fb.Bind();
    rhi::GraphicsDevice::Get().ReadPixelsRGB(fb.Width() / 2, fb.Height() / 2, 1, 1, rgb);
    rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
}

// Вычислительный шейдер удваивает числа в буфере — и они возвращаются.
void TestComputeBuffer() {
    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    if (!device.Capabilities().ComputeShaders) {
        std::printf("  [пропуск] вычислений нет у контекста — проверка буфера не применима\n");
        return;
    }
    const char* src = R"(#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 3) buffer Data { float v[]; };
void main() { v[gl_GlobalInvocationID.x] *= 2.0; })";
    rhi::ShaderBuildLog log;
    auto prog = device.CreateProgram({{{rhi::ShaderStage::Compute, src}}, "double"}, log);
    Check(prog != nullptr, "вычислительная программа собирается");
    if (!prog) return;
    std::vector<float> data(128);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (float)i;
    rhi::BufferDesc bd;
    bd.Bytes = data.size() * sizeof(float);
    auto buf = device.CreateBuffer(bd, data.data());
    Check(buf != nullptr, "буфер шейдера создаётся");
    if (!buf) return;
    prog->Use();
    device.BindBuffer(3, buf.get());
    device.Dispatch(2);
    device.ShaderBarrier(rhi::BarrierBits::All);
    std::vector<float> back(data.size(), -1.0f);
    Check(buf->Read(back.data(), bd.Bytes), "буфер читается обратно");
    bool ok = true;
    for (size_t i = 0; i < back.size(); ++i) ok = ok && back[i] == 2.0f * (float)i;
    Check(ok, "вычислительный шейдер записал в буфер то, что должен");
    device.BindBuffer(3, nullptr);
}

// Граф: вычисления пишут текстуру, графический проход читает её в цель.
// Проверяет и барьер после вычислений, и привязку цели графом.
void TestGraphComputeToTarget() {
    rhi::GraphicsDevice& device = rhi::GraphicsDevice::Get();
    if (!device.Capabilities().ComputeShaders) return;
    const char* comp = R"(#version 430
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 0) uniform writeonly image2D uOut;
void main() { imageStore(uOut, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 1.0, 0.0, 1.0)); })";
    ShaderLibrary lib;
    lib.LoadStages("fill", {{rhi::ShaderStage::Compute, "", comp}});
    Check(lib.Get("fill") != nullptr, "ShaderLibrary собирает вычислительный шейдер из текста");

    rhi::Texture2DDesc td;
    td.Width = td.Height = 8;
    td.FilterMode = rhi::Filter::Nearest;
    td.GenerateMipmaps = false;
    td.ShaderWrite = true;
    auto tex = device.CreateTexture2D(td, nullptr);

    Framebuffer out(64, 64);
    RenderGraph graph;
    graph.Reset(64, 64, &out.Target());
    const RGHandle img = graph.ImportTexture("Картинка", tex->Handle());
    RGPass fill;
    fill.Name = "Заливка";
    fill.Type = RGPassType::Compute;
    fill.Writes = {img};
    fill.Execute = [&](RenderGraphContext& ctx) {
        lib.Get("fill")->Use();
        ctx.Device().BindImage(0, ctx.Color(img), rhi::ImageAccess::Write, rhi::ImageFormat::RGBA8);
        ctx.Device().Dispatch(1, 1);
    };
    graph.AddPass(std::move(fill));
    AddEffectChain(graph, "Вывод", img, graph.Output(), {});
    std::string err;
    Check(graph.Compile(err), "граф «вычисления -> вывод» собирается");
    graph.Execute();
    unsigned char rgb[3] = {0, 0, 0};
    ReadCenter(out, rgb);
    Check(rgb[1] > 200 && rgb[0] < 40 && rgb[2] < 40, "записанное вычислениями видно следующему проходу");
}

// Ошибка в шейдере называет ФАЙЛ И СТРОКУ — и в главном файле, и во
// включённом через #include: директивы #line поняты драйвером.
void TestDiagnosticsMapLines() {
    RegisterShaderInclude("test/helpers.glsl", "float ok_fn() { return 1.0; }\nfloat bad_fn() { return undefined_in_include; }\n");
    const std::string frag = "#version 330 core\n"
                             "#include <test/helpers.glsl>\n"
                             "out vec4 C;\n"
                             "void main() {\n"
                             "    C = vec4(undefined_in_main);\n" // строка 5
                             "}\n";
    ShaderLibrary lib;
    lib.LoadStages("broken", {{rhi::ShaderStage::Vertex, "", FullscreenVertexSource()},
                              {rhi::ShaderStage::Fragment, "", frag}});
    const ShaderEntry* e = lib.Find("broken");
    Check(e && !e->Program && !e->LastBuildOk, "шейдер с ошибкой не собирается и не роняет игру");
    bool inMain = false, inInclude = false;
    if (e)
        for (const ShaderDiagnostic& d : e->Diagnostics) {
            if (!d.Error) continue;
            if (d.File == "broken.fragment" && d.Line == 5) inMain = true;
            if (d.File == "test/helpers.glsl" && d.Line == 2) inInclude = true;
        }
    if (!inMain || !inInclude)
        for (const ShaderDiagnostic& d : e->Diagnostics) std::printf("    %s\n", d.ToString().c_str());
    Check(inMain, "ошибка в главном файле — с его именем и строкой 5");
    Check(inInclude, "ошибка во включённом файле — с его именем и строкой 2");
}

// Правка на лету: сломанная правка не заменяет рабочую программу.
void TestHotReloadKeepsWorkingProgram() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "sage_hot_reload";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "fx.frag").string();
    auto write = [&](const char* body) {
        std::ofstream f(path, std::ios::trunc);
        f << body;
    };
    write("#version 330 core\nout vec4 C;\nvoid main() { C = vec4(1.0); }\n");
    ShaderLibrary lib;
    lib.LoadStages("fx", {{rhi::ShaderStage::Vertex, "", FullscreenVertexSource()},
                          {rhi::ShaderStage::Fragment, path, ""}});
    rhi::ShaderProgram* first = lib.Get("fx");
    Check(first != nullptr, "шейдер из файла собирается");
    // Время правки файловой системы грубое — сдвигаем его явно.
    write("#version 330 core\nout vec4 C;\nvoid main() { C = vec4(oops); }\n");
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
    Check(lib.ReloadChanged() == 0, "сломанная правка не считается пересборкой");
    Check(lib.Get("fx") == first, "после сломанной правки рисует прежняя программа");
    Check(lib.ErrorCount() > 0, "ошибка правки видна в библиотеке");
    write("#version 330 core\nout vec4 C;\nvoid main() { C = vec4(0.5); }\n");
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(4));
    Check(lib.ReloadChanged() == 1 && lib.Find("fx")->Version == 2, "исправленная правка подхватывается");
    std::filesystem::remove_all(dir);
}

// Встроенный PBR — обычный рендерер из реестра; расширение дорисовывает
// своё после штатного прохода сцены.
void TestStandardPipelineWithExtension(Scene& scene) {
    auto pipeline = RenderPipelineRegistry::Create(StandardPipeline::kId);
    Check(pipeline != nullptr, "встроенный PBR есть в реестре");
    if (!pipeline) return;
    PipelineRenderer renderer(std::move(pipeline));
    Check(renderer.Ready(), "встроенный PBR инициализируется");

    Framebuffer out(kW, kH);
    RenderView view;
    view.View = TestView();
    view.Proj = glm::perspective(glm::radians(50.0f), (float)kW / (float)kH, 0.1f, 100.0f);
    view.Position = kEye;
    view.Width = kW;
    view.Height = kH;
    view.World = &scene;
    view.Output = &out;
    Check(renderer.Render(view), "кадр встроенного PBR собирается и выполняется");
    unsigned char rgb[3] = {0, 0, 0};
    ReadCenter(out, rgb);
    Check(rgb[0] + rgb[1] + rgb[2] > 30, "встроенный PBR через граф рисует сцену");

    // Расширение: после «Сцена» залить HDR красным — свой проход в чужом рендере.
    ShaderLibrary lib;
    lib.LoadStages("red", {{rhi::ShaderStage::Vertex, "", FullscreenVertexSource()},
                           {rhi::ShaderStage::Fragment, "", "#version 330 core\nout vec4 C;\nvoid main(){ C = vec4(0.6, 0.0, 0.0, 1.0); }\n"}});
    ShaderMaterial red(lib, "red");
    red.State.DepthTest = false;
    renderer.AddExtension("red", [&](RenderGraph& g, const RenderView&) {
        RGPass p;
        p.Name = "Красный";
        p.Writes = {g.Find(StandardPipeline::kHdr)};
        p.Execute = [&](RenderGraphContext& ctx) {
            if (red.Apply(ctx.Device())) DrawFullscreenTriangle();
            RestoreDefaultState(ctx.Device());
        };
        g.AddPassAfter(StandardPipeline::kScenePass, std::move(p));
    });
    Check(renderer.Render(view), "кадр с расширением собирается");
    ReadCenter(out, rgb);
    if (!(rgb[0] > 90 && rgb[0] > rgb[1] + 60 && rgb[0] > rgb[2] + 60))
        std::printf("    цвет %d %d %d\n%s", rgb[0], rgb[1], rgb[2], renderer.Graph().Describe().c_str());
    Check(rgb[0] > 90 && rgb[0] > rgb[1] + 60 && rgb[0] > rgb[2] + 60, "проход расширения попал в кадр между сценой и постом");
}

} // namespace

void RunPipelineChecks(Scene& scene) {
    std::printf("=== Свой рендер: вычисления, граф, шейдеры, PBR как рендерер ===\n");
    TestComputeBuffer();
    TestGraphComputeToTarget();
    TestDiagnosticsMapLines();
    TestHotReloadKeepsWorkingProgram();
    TestStandardPipelineWithExtension(scene);
}

} // namespace sage::rendertest
