#include "SelfTestCommon.h"

#include <fstream>

#include "NewShader.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostEffect.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ResourceManager.h"

// ---------------------------------------------------------------------------
// СВОИ ШЕЙДЕРЫ РАБОТАЮТ НА НАСТОЯЩЕМ ДРАЙВЕРЕ.
//
// Собраться мало: шейдер, который собрался, но ничего не рисует (не тот
// атрибут, альфа 0, текстура не привязана, звено не вызвано), выглядит как
// «не работает». Поэтому здесь рисуется кадр и проверяется, что в нём:
//   • пустой материал-шейдер из «Создать → Шейдер» виден на объекте;
//   • текстура, объявленная шейдером (uniform sampler2D), доходит до пикселя,
//     а общий код подключается «#include "файл"»;
//   • звено «Свой шейдер» камеры меняет кадр.
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
    namespace ns = sage::editor::newshader;
    fs::path made, again, screenMade;
    std::string err;
    if (!ns::Create(ns::Kind::Material, "selftest_shader", m_project.Dir(), m_project.AssetsDir(), made, err))
        fail("материал-шейдер не создан: " + err);
    if (ns::Create(ns::Kind::Material, "selftest_shader", m_project.Dir(), m_project.AssetsDir(), again, err))
        fail("повтор имени перезаписал существующие файлы");
    if (!ns::Create(ns::Kind::Screen, "selftest_screen", m_project.Dir(), m_project.AssetsDir(), screenMade, err))
        fail("шейдер экрана не создан: " + err);
    else if (!ResourceManager::Instance().GetScreenShader(screenMade.string()))
        fail("пустой шейдер экрана не собрался");

    const std::string matPath = "assets/materials/selftest_shader.sagemat";
    auto mat = ResourceManager::Instance().GetMaterial(matPath);
    if (!mat || !mat->HasCustomShader() || !mat->ShaderPtr) {
        fail("пустой материал-шейдер не загрузился или не собрался (ошибка — выше в логе)");
        return false;
    }
    if (!mat->ShaderPtr->Defaults.count("uColor")) fail("умолчания параметров не дошли до программы");

    // Кадр вьюпорта. Шаг пикселя — из размера: кадр читается без альфы (RGB).
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
    auto count = [&stride](const std::vector<unsigned char>& px, auto pred) {
        long long n = 0;
        for (size_t i = 0; i + 2 < px.size(); i += stride)
            if (pred(px[i], px[i + 1], px[i + 2])) ++n;
        return n;
    };

    // 1) Пустой материал-шейдер на плоскости — виден в кадре.
    std::vector<unsigned char> before, after;
    const bool hasBefore = frame(before);
    GameObject plane = CreatePrimitiveEntity("SelftestShaderPlane", MeshRef::Type::Plane);
    plane.GetTransform().Scale = glm::vec3(40.0f, 1.0f, 40.0f);
    auto& mr = m_scene->Registry().get<MeshRendererComponent>(plane.Entity());
    AssignMaterial(mr, matPath, mat);
    const bool hasAfter = frame(after);
    if (!hasBefore || !hasAfter) {
        fail("кадр вьюпорта не прочитался");
    } else {
        long long changed = 0;
        for (size_t i = 0; i + 2 < after.size() && i + 2 < before.size(); i += stride)
            if (std::abs(after[i] - before[i]) + std::abs(after[i + 1] - before[i + 1]) +
                    std::abs(after[i + 2] - before[i + 2]) > 24)
                ++changed;
        if (changed * 50 < (long long)(after.size() / stride))
            fail("плоскость с материалом-шейдером не видна в кадре (меньше 2% пикселей)");
    }

    // 2) Текстура шейдера и «#include»: зелёная картинка через параметр uPaint,
    //    функция выборки — из включённого файла.
    {
        const fs::path shaders = m_project.AssetsDir() / "shaders";
        const fs::path textures = m_project.AssetsDir() / "textures";
        std::error_code ec;
        fs::create_directories(textures, ec);
        const unsigned char green[4 * 4 * 3] = {
            0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0,
            0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0};
        stbi_write_png((textures / "selftest_green.png").string().c_str(), 4, 4, 3, green, 4 * 3);
        std::ofstream(shaders / "selftest_paint.glsl") << "vec3 Paint(sampler2D t, vec2 uv) { return texture(t, uv).rgb; }\n";
        std::ofstream(shaders / "selftest_shader.frag")
            << "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\n"
               "// @header Краска\nuniform sampler2D uPaint; // Картинка\n"
               "#include \"selftest_paint.glsl\"\n"
               "void main() { FragColor = vec4(Paint(uPaint, vUV), 1.0); }\n";
        // Правка файла — тем же путём, что в редакторе: пересборка на лету.
        ResourceManager::Instance().ReloadChangedShaders();
        mat->TextureParams["uPaint"].Path = "assets/textures/selftest_green.png";
        std::vector<unsigned char> px;
        if (!mat || !mat->ShaderPtr) {
            fail("шейдер с текстурой и #include не собрался (ошибка — выше в логе)");
        } else if (mat->ShaderPtr->Textures.empty() || mat->ShaderPtr->Textures[0].Name != "uPaint") {
            fail("текстура uPaint не попала в список текстур программы");
        } else if (!frame(px)) {
            fail("кадр вьюпорта не прочитался");
        } else {
            const long long g = count(px, [](int r, int gr, int b) { return gr > r + 60 && gr > b + 60; });
            if (g * 20 < (long long)(px.size() / stride)) {
                const size_t mid = (px.size() / stride / 2 + 40) * stride;
                fail("текстура шейдера не дошла до кадра: зелёных пикселей " + std::to_string(g) + ", пиксель ниже центра " +
                     std::to_string(px[mid]) + "," + std::to_string(px[mid + 1]) + "," + std::to_string(px[mid + 2]));
            }
        }
    }
    m_scene->RemoveObject(plane.Id());

    // 3) Шейдер экрана: звено «Свой шейдер», заливающее кадр красным.
    {
        const fs::path red = m_project.AssetsDir() / "shaders" / "selftest_red.frag";
        std::ofstream(red) << "#version 330 core\nin vec2 vUV;\nout vec4 FragColor;\nuniform sampler2D uScene;\n"
                              "uniform float uStrength;\n"
                              "void main() { FragColor = vec4(mix(texture(uScene, vUV).rgb, vec3(1, 0, 0), uStrength), 1.0); }\n";
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
        const long long redPixels = count(px, [](int r, int g, int b) { return r > 180 && g < 60 && b < 60; });
        if (!got) fail("кадр вьюпорта не прочитался");
        else if (redPixels * 10 < (long long)(px.size() / stride) * 9)
            fail("звено «Свой шейдер» не изменило кадр: красных пикселей " + std::to_string(redPixels) + " из " +
                 std::to_string(px.size() / stride));
        // Путь шейдера звена переживает сохранение тракта.
        const sage::render::PostChain back = sage::render::PostChainFromJson(sage::render::PostChainToJson(ppc.Chain));
        if (back.Effects.empty() || back.Effects.back().Source != "assets/shaders/selftest_red.frag")
            fail("путь шейдера звена «Свой шейдер» потерялся при сохранении");
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: shaders: OK — материал-шейдер виден, текстура и #include работают, "
                                  "шейдер экрана меняет кадр";
    return ok;
}
