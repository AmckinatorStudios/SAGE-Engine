// Система своего рендера без видеокарты: граф кадра со своими ресурсами,
// пул целей, вставка проходов в чужой рендерер, препроцессор шейдеров,
// разбор ответов драйверов, реестр рендереров. Всё на Null-устройстве —
// устройство кадра проверяется здесь, картинка — в tests/render/Checks_Pipeline.
#include "TestFramework.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rhi/null/NullDevice.h"
#include "sage/render/EffectChain.h"
#include "sage/render/RenderGraph.h"
#include "sage/render/RenderPipeline.h"
#include "sage/render/ShaderLibrary.h"
#include "sage/render/ShaderMaterial.h"
#include "sage/render/StandardPipeline.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::render;
namespace rhi = sage::rhi;

namespace {

// Null-устройство на время теста — и обязательно снятое после.
struct NullScope {
    std::unique_ptr<rhi::GraphicsDevice> Device = rhi::GraphicsDevice::Create(rhi::Backend::Null);
    NullScope() {
        Device->Init(nullptr);
        rhi::GraphicsDevice::SetCurrent(Device.get());
        rhi::NullDevice::ResetCounters();
    }
    ~NullScope() { rhi::GraphicsDevice::SetCurrent(nullptr); }
};

RGPass Pass(const std::string& name, std::vector<std::string>& log, std::vector<RGHandle> reads,
            std::vector<RGHandle> writes, RGPassType type = RGPassType::Graphics) {
    RGPass p;
    p.Name = name;
    p.Type = type;
    p.Reads = std::move(reads);
    p.Writes = std::move(writes);
    p.Execute = [&log, name](RenderGraphContext&) { log.push_back(name); };
    return p;
}

} // namespace

TEST(rendergraph_creates_targets_and_culls_unneeded) {
    NullScope s;
    RenderGraph g;
    g.Reset(320, 200);
    const RGHandle hdr = g.CreateTexture("HDR", {});
    RGTextureDesc half;
    half.Scale = 0.5f;
    const RGHandle blur = g.CreateTexture("Размытие", half);
    std::vector<std::string> log;
    RGPass sky = Pass("Небо", log, {}, {hdr});
    sky.ColorLoad = LoadOp::Clear;
    g.AddPass(std::move(sky));
    g.AddPass(Pass("Никому не нужен", log, {hdr}, {blur}));
    g.AddPass(Pass("Пост", log, {hdr}, {g.Output()}));
    std::string err;
    CHECK_TRUE(g.Compile(err));
    g.Execute();
    CHECK_EQ(log.size(), (size_t)2);
    CHECK_EQ(g.Stats().Culled, 1);
    // Цель для выполняемых — создана, для отброшенного — нет.
    CHECK_EQ(g.Stats().PooledTargets, 1);
}

TEST(rendergraph_draw_over_keeps_earlier_writer) {
    // «Небо» чистит HDR, «Сцена» дорисовывает поверх (LoadOp::Load) и не
    // объявляет чтения: без неявного чтения небо выбросилось бы из кадра.
    NullScope s;
    RenderGraph g;
    g.Reset(64, 64);
    const RGHandle hdr = g.CreateTexture("HDR", {});
    std::vector<std::string> log;
    RGPass sky = Pass("Небо", log, {}, {hdr});
    sky.ColorLoad = LoadOp::Clear;
    g.AddPass(std::move(sky));
    g.AddPass(Pass("Сцена", log, {}, {hdr}));
    g.AddPass(Pass("Пост", log, {hdr}, {g.Output()}));
    std::string err;
    CHECK_TRUE(g.Compile(err));
    g.Execute();
    CHECK_EQ(log.size(), (size_t)3);
    CHECK_TRUE(log[0] == "Небо");
}

TEST(rendergraph_pool_reuses_targets_between_frames) {
    NullScope s;
    RenderGraph g;
    std::string err;
    for (int frame = 0; frame < 3; ++frame) {
        g.Reset(128, 128);
        const RGHandle hdr = g.CreateTexture("HDR", {});
        std::vector<std::string> log;
        g.AddPass(Pass("Сцена", log, {}, {hdr}));
        g.AddPass(Pass("Пост", log, {hdr}, {g.Output()}));
        CHECK_TRUE(g.Compile(err));
        g.Execute();
        // Устоявшийся кадр видеопамять не выделяет.
        CHECK_EQ(g.Stats().CreatedThisFrame, frame == 0 ? 1 : 0);
    }
    const int shaders = rhi::NullDevice::Counters().RenderTargets;
    CHECK_EQ(shaders, 1);
}

TEST(rendergraph_insert_and_disable_by_name) {
    NullScope s;
    RenderGraph g;
    g.Reset(64, 64);
    const RGHandle hdr = g.CreateTexture("HDR", {});
    std::vector<std::string> log;
    g.AddPass(Pass("Сцена", log, {}, {hdr}));
    g.AddPass(Pass("Пост", log, {hdr}, {g.Output()}));
    CHECK_TRUE(g.AddPassAfter("Сцена", Pass("Своя вода", log, {}, {hdr})));
    CHECK_TRUE(g.AddPassBefore("Сцена", Pass("Свой фон", log, {}, {hdr})));
    CHECK_FALSE(g.AddPassAfter("Нет такого", Pass("В конец", log, {}, {})));
    CHECK_TRUE(g.DisablePass("Пост"));
    g.AddPass(Pass("Свой пост", log, {hdr}, {g.Output()}));
    std::string err;
    CHECK_TRUE(g.Compile(err));
    g.Execute();
    const std::vector<std::string> expect = {"Свой фон", "Сцена", "Своя вода", "Свой пост"};
    CHECK_TRUE(log == expect);
}

TEST(rendergraph_compute_pass_gets_barrier_and_buffer) {
    NullScope s;
    RenderGraph g;
    g.Reset(64, 64);
    RGBufferDesc bd;
    bd.Bytes = 256;
    const RGHandle buf = g.CreateBuffer("Частицы", bd);
    rhi::GpuBuffer* seen = nullptr;
    RGPass sim;
    sim.Name = "Симуляция";
    sim.Type = RGPassType::Compute;
    sim.Writes = {buf};
    sim.Execute = [&](RenderGraphContext& ctx) {
        seen = ctx.Buffer(buf);
        ctx.Device().Dispatch(4);
    };
    g.AddPass(std::move(sim));
    std::vector<std::string> log;
    g.AddPass(Pass("Рисование", log, {buf}, {g.Output()}));
    std::string err;
    CHECK_TRUE(g.Compile(err));
    g.Execute();
    CHECK_TRUE(seen != nullptr && seen->Size() == 256);
    CHECK_EQ(rhi::NullDevice::Counters().Dispatches, 1);
}

TEST(rendergraph_reports_read_before_write) {
    NullScope s;
    RenderGraph g;
    g.Reset(64, 64);
    const RGHandle hdr = g.CreateTexture("HDR", {});
    std::vector<std::string> log;
    g.AddPass(Pass("Пост", log, {hdr}, {g.Output()}));
    std::string err;
    CHECK_FALSE(g.Compile(err));
    CHECK_TRUE(err.find("HDR") != std::string::npos);
    g.Execute(); // без успешной сборки — ничего
    CHECK_TRUE(log.empty());
}

TEST(shader_preprocess_expands_includes_with_line_markers) {
    std::map<std::string, std::string> files = {{"light.glsl", "#include \"util.glsl\"\nfloat light();\n"},
                                                {"util.glsl", "float util();\n"}};
    auto resolver = [&](const std::string& n, std::string& out) {
        auto it = files.find(n);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    };
    const std::string src = "#version 330 core\n#include \"light.glsl\"\n#include \"util.glsl\"\nvoid main(){}\n";
    PreprocessedShader p = PreprocessShader(src, "main.frag", resolver);
    CHECK_TRUE(p.Ok);
    CHECK_EQ(p.Files.size(), (size_t)3);
    CHECK_TRUE(p.Files[1] == "light.glsl" && p.Files[2] == "util.glsl");
    CHECK_TRUE(p.Code.find("#line 1 1\n") != std::string::npos);
    CHECK_TRUE(p.Code.find("#line 3 0\n") != std::string::npos); // назад в main.frag
    // util.glsl включён один раз, хоть и упомянут дважды.
    CHECK_EQ(p.Code.find("float util();"), p.Code.rfind("float util();"));
    // Встроенный блок движка доступен любому шейдеру.
    CHECK_TRUE(PreprocessShader("#include <sage/common.glsl>\n", "x", {}).Code.find("sage_aces") !=
               std::string::npos);
    PreprocessedShader missing = PreprocessShader("#version 330\n#include \"nope.glsl\"\n", "a.frag", resolver);
    CHECK_FALSE(missing.Ok);
    CHECK_TRUE(missing.Error.find("a.frag:2") != std::string::npos);
}

TEST(shader_log_parses_vendor_formats) {
    const std::vector<std::string> files = {"main.frag", "light.glsl"};
    auto mesa = ParseShaderLog("1:12(5): error: `foo' undeclared", rhi::ShaderStage::Fragment, false, files);
    CHECK_EQ(mesa.size(), (size_t)1);
    CHECK_TRUE(mesa[0].File == "light.glsl" && mesa[0].Line == 12 && mesa[0].Error);
    CHECK_TRUE(mesa[0].Message == "`foo' undeclared");
    auto nv = ParseShaderLog("0(7) : warning C7050: \"x\" might be used before being initialized",
                             rhi::ShaderStage::Vertex, false, files);
    CHECK_TRUE(nv.size() == 1 && nv[0].File == "main.frag" && nv[0].Line == 7 && !nv[0].Error);
    auto amd = ParseShaderLog("ERROR: 1:3: 'y' : undeclared identifier\nERROR: 1 compilation errors.",
                              rhi::ShaderStage::Fragment, false, files);
    CHECK_TRUE(amd.size() == 2 && amd[0].File == "light.glsl" && amd[0].Line == 3);
    CHECK_TRUE(mesa[0].ToString() == "light.glsl:12: error: `foo' undeclared");
}

TEST(pipeline_registry_lists_builtin_pbr_and_accepts_replacement) {
    CHECK_TRUE(RenderPipelineRegistry::Find(StandardPipeline::kId) != nullptr);
    struct Mine : RenderPipeline {
        std::string Id() const override { return "test.mine"; }
        void Build(RenderGraph& g, const RenderView&) override {
            RGPass p;
            p.Name = "Всё сразу";
            p.Writes = {g.Output()};
            g.AddPass(std::move(p));
        }
    };
    RenderPipelineRegistry::Register({"test.mine", "Мой", "", [] { return std::make_unique<Mine>(); }});
    RenderPipelineRegistry::Register({"test.mine", "Мой 2", "", [] { return std::make_unique<Mine>(); }});
    int count = 0;
    for (const PipelineInfo& i : RenderPipelineRegistry::All())
        if (i.Id == "test.mine") ++count;
    CHECK_EQ(count, 1);
    CHECK_TRUE(RenderPipelineRegistry::Find("test.mine")->Label == "Мой 2");

    NullScope s;
    PipelineRenderer r(RenderPipelineRegistry::Create("test.mine"));
    CHECK_TRUE(r.Ready());
    RenderView v;
    CHECK_TRUE(r.Render(v));
    CHECK_TRUE(r.Graph().HasPass("Всё сразу"));
}

TEST(effect_chain_skips_disabled_and_falls_back_to_copy) {
    NullScope s;
    ShaderLibrary lib;
    lib.LoadStages("fx", {{rhi::ShaderStage::Vertex, "", "v"}, {rhi::ShaderStage::Fragment, "", "f"}});
    ShaderMaterial a(lib, "fx"), b(lib, "fx");
    RenderGraph g;
    g.Reset(64, 64);
    const RGHandle hdr = g.CreateTexture("HDR", {});
    RGPass scene;
    scene.Name = "Сцена";
    scene.Writes = {hdr};
    g.AddPass(std::move(scene));
    CHECK_EQ(AddEffectChain(g, "Пост", hdr, g.Output(), {{"a", &a, false, {}}, {"b", &b, true, {}}}), 1);
    CHECK_TRUE(g.HasPass("Пост: b") && !g.HasPass("Пост: a"));
    std::string err;
    CHECK_TRUE(g.Compile(err));

    RenderGraph g2;
    g2.Reset(64, 64);
    const RGHandle h2 = g2.CreateTexture("HDR", {});
    RGPass s2;
    s2.Name = "Сцена";
    s2.Writes = {h2};
    g2.AddPass(std::move(s2));
    CHECK_EQ(AddEffectChain(g2, "Пост", h2, g2.Output(), {}), 1);
    CHECK_TRUE(g2.HasPass("Пост: копия"));
}

TEST(shader_material_needs_program_and_null_buffer_round_trips) {
    NullScope s;
    ShaderMaterial none;
    CHECK_FALSE(none.Apply(*s.Device));
    rhi::BufferDesc d;
    d.Bytes = 16;
    auto buf = s.Device->CreateBuffer(d, nullptr);
    const float in[4] = {1, 2, 3, 4};
    CHECK_TRUE(buf->Update(in, sizeof(in)));
    CHECK_FALSE(buf->Update(in, sizeof(in), 4)); // за край — отказ
    float out[4] = {};
    CHECK_TRUE(buf->Read(out, sizeof(out)));
    CHECK_TRUE(out[3] == 4.0f);
}
