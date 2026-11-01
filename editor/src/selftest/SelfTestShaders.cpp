#include "SelfTestCommon.h"

#include <fstream>

#include "ShaderTemplates.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostEffect.h"
#include "sage/render/ResourceManager.h"

// ---------------------------------------------------------------------------
// ШАБЛОНЫ ШЕЙДЕРОВ СОБИРАЮТСЯ НА НАСТОЯЩЕМ ДРАЙВЕРЕ.
//
// Свой шейдер материала был в движке, но ни одного рабочего примера в
// репозитории — и проверить, что контракт (атрибуты, инстансы, <sage_pbr>)
// вообще сходится, было не на чем. Здесь каждый шаблон окна «Создать →
// Шейдер» создаётся в проекте и собирается: материал — через загрузку
// .sagemat, экран — как звено «Свой шейдер». Умолчания параметров обязаны
// дойти до программы (иначе вода с нулевой длиной волны делит на ноль).
// ---------------------------------------------------------------------------
bool EditorLayer::SelfTestShaders() {
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: shaders: " << why;
        ok = false;
    };
    if (!m_project.Loaded()) {
        fail("нет проекта");
        return false;
    }
    namespace st = sage::editor::shadertemplates;
    for (const st::Template& t : st::All()) {
        fs::path made;
        std::string err;
        const std::string name = std::string("selftest_") + t.Id;
        if (!st::Create(t, name, m_project.Dir(), m_project.AssetsDir(), made, err)) {
            fail(std::string("шаблон ") + t.Id + " не создан: " + err);
            continue;
        }
        if (t.Kind == st::Target::Screen) {
            auto sh = ResourceManager::Instance().GetScreenShader(made.string());
            if (!sh) fail("шейдер экрана не собрался");
            // Звено тракта хранит путь и переживает сохранение.
            sage::render::PostChain chain;
            sage::render::AddPostEffect(chain, "shader");
            chain.Effects.back().Source = "assets/shaders/" + name + ".frag";
            const sage::render::PostChain back =
                sage::render::PostChainFromJson(sage::render::PostChainToJson(chain));
            if (back.Effects.empty() || back.Effects.back().Source != chain.Effects.back().Source)
                fail("путь шейдера звена «Свой шейдер» потерялся при сохранении");
            continue;
        }
        // Повтор имени — отказ, а не перезапись чужих файлов.
        fs::path again;
        if (st::Create(t, name, m_project.Dir(), m_project.AssetsDir(), again, err))
            fail(std::string("шаблон ") + t.Id + " перезаписал существующие файлы");
        auto mat = ResourceManager::Instance().GetMaterial(made.string());
        if (!mat || !mat->HasCustomShader()) {
            fail(std::string("материал шаблона ") + t.Id + " не загрузился");
            continue;
        }
        if (mat->Params.empty()) fail(std::string("материал шаблона ") + t.Id + " без параметров");
        if (!mat->ShaderPtr) {
            fail(std::string("шейдер шаблона ") + t.Id + " не собрался (ошибка компиляции — выше в логе)");
            continue;
        }
        if (std::string(t.Id) == "water" && !mat->ShaderPtr->Defaults.count("uWaveLength"))
            fail("умолчания параметров воды не дошли до программы");
    }
    // --- Вживую: кадр вьюпорта ---------------------------------------------
    // Собраться мало: шейдер, который собрался, но ничего не рисует (не тот
    // атрибут, альфа 0, звено не вызвано), выглядит как «не работает». Поэтому
    // кадр рисуется и сравнивается.
    // Шаг пикселя — из размера: кадр читается без альфы (RGB).
    size_t stride = 3;
    auto frame = [this, &stride](std::vector<unsigned char>& out) {
        glm::mat4 v(1.0f), p(1.0f);
        m_renderer.RenderViewport(*m_scene, m_camera, m_scene->Lighting, -1, {}, EditorRenderMode::Shaded,
                                  /*grid=*/false, m_settings, v, p);
        int w = 0, h = 0;
        if (!m_renderer.ReadViewportPixels(out, w, h) || out.empty() || w <= 0 || h <= 0) return false;
        stride = std::max<size_t>(3, out.size() / ((size_t)w * (size_t)h));
        return true;
    };
    auto diff = [&stride](const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
        long long changed = 0;
        for (size_t i = 0; i + 2 < a.size() && i + 2 < b.size(); i += stride) {
            const int d = std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]);
            if (d > 24) ++changed;
        }
        return changed;
    };

    // Вода: плоскость с материалом шаблона обязана появиться в кадре.
    {
        std::vector<unsigned char> before, after;
        const bool hasBefore = frame(before);
        GameObject water = CreatePrimitiveEntity("SelftestWater", MeshRef::Type::Plane);
        water.GetTransform().Scale = glm::vec3(40.0f, 1.0f, 40.0f);
        auto& mr = m_scene->Registry().get<MeshRendererComponent>(water.Entity());
        const std::string matPath = "assets/materials/selftest_water.sagemat";
        AssignMaterial(mr, matPath, ResourceManager::Instance().GetMaterial(matPath));
        const bool hasAfter = frame(after);
        m_scene->RemoveObject(water.Id());
        if (!hasBefore || !hasAfter) fail("кадр вьюпорта не прочитался");
        else if (diff(before, after) * 50 < (long long)(after.size() / stride))
            fail("плоскость с материалом воды не видна в кадре (меньше 2% пикселей)");
    }

    // Шейдер экрана: звено «Свой шейдер», заливающее кадр красным, обязано
    // дойти до экрана — иначе звено молча пропускается.
    {
        const fs::path red = m_project.AssetsDir() / "shaders" / "selftest_red.frag";
        {
            std::ofstream f(red);
            f << "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\nuniform sampler2D uScene;\n"
                 "uniform float uStrength;\n"
                 "void main() { FragColor = vec4(mix(texture(uScene, vUV).rgb, vec3(1, 0, 0), uStrength), 1.0); }\n";
        }
        GameObject cam = m_scene->CreateEmptyObject("SelftestScreenShaderCamera");
        m_scene->Registry().emplace<CameraComponent>(cam.Entity());
        sage::render::PostProcessComponent ppc;
        // Тракт только из этого звена: виньетка и зерно штатного тракта
        // перекрасили бы края кадра и спутали подсчёт.
        sage::render::AddPostEffect(ppc.Chain, "shader").Source = "assets/shaders/selftest_red.frag";
        m_scene->Registry().emplace<sage::render::PostProcessComponent>(cam.Entity(), ppc);
        std::vector<unsigned char> px;
        const bool got = frame(px);
        m_scene->RemoveObject(cam.Id());
        long long redPixels = 0;
        for (size_t i = 0; i + 2 < px.size(); i += stride)
            if (px[i] > 180 && px[i + 1] < 60 && px[i + 2] < 60) ++redPixels;
        if (!got) fail("кадр вьюпорта не прочитался");
        else if (redPixels * 10 < (long long)(px.size() / stride) * 9)
            fail("звено «Свой шейдер» не изменило кадр: красных пикселей " + std::to_string(redPixels) +
                 " из " + std::to_string(px.size() / stride));
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: shaders: OK — шаблоны собраны, вода видна, шейдер экрана работает";
    return ok;
}
