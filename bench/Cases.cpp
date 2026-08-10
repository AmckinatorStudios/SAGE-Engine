// ---------------------------------------------------------------------------
// Сами замеры. Каждый строит своё окружение ОДИН РАЗ (в лямбде-захвате), а в
// теле остаётся только измеряемая работа: иначе половина времени уходила бы на
// постройку сцены, и правка горячего места не была бы видна за этим шумом.
//
// Ничего из графики здесь нет и быть не может: sage_bench запускается без окна
// и без видеокарты, в том числе на машине сборки. Всё, что упирается в GPU,
// меряется своим способом (см. tests/render и профилировщик кадра).
// ---------------------------------------------------------------------------
#include "Cases.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "physics/builtin/BuiltinWorld.h"
#include "sage/core/JobSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/Frustum.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/ui/UI.h"
#include "sage/ui/UILegacy.h"
#include "sage/ui/UISceneSystem.h"

namespace sage::bench {
namespace {

// --- Сцены под замер ---------------------------------------------------------

// Плоская сцена: n сущностей без родителей. Основной случай — так выглядит
// уровень из расставленных объектов.
std::unique_ptr<Scene> MakeFlatScene(int n) {
    auto scene = std::make_unique<Scene>("bench");
    for (int i = 0; i < n; ++i) {
        GameObject o = scene->CreateObject("o");
        Transform& t = o.GetTransform();
        t.Position = {(float)(i % 64) * 2.0f, (float)((i / 64) % 16), (float)(i / 1024) * 2.0f};
        t.Rotation = {0.0f, (float)(i % 360), 0.0f};
    }
    return scene;
}

// Глубокая иерархия: цепочки по depth звеньев. Здесь и проверяется мемоизация
// мировых матриц — без неё цена растёт как n*depth, а не как n.
std::unique_ptr<Scene> MakeHierarchyScene(int chains, int depth) {
    auto scene = std::make_unique<Scene>("bench-tree");
    for (int c = 0; c < chains; ++c) {
        GameObject parent = scene->CreateObject("root");
        parent.GetTransform().Position = {(float)c, 0.0f, 0.0f};
        for (int d = 1; d < depth; ++d) {
            GameObject child = scene->CreateObject("link");
            child.GetTransform().Position = {0.0f, 1.0f, 0.0f};
            child.GetTransform().Rotation = {0.0f, 7.0f, 0.0f};
            scene->SetParent(child.Entity(), parent.Entity());
            parent = child;
        }
    }
    return scene;
}

glm::mat4 BenchViewProj() {
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 30.0f, 90.0f), glm::vec3(0.0f),
                                       glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    return proj * view;
}

// --- Замеры -------------------------------------------------------------------

void RegisterSceneCases() {
    // Мировые матрицы плоской сцены. Самый частый проход кадра: его делает
    // каждый проход отрисовки (тени, отражения, сцена).
    {
        constexpr int kN = 20000;
        auto scene = std::make_shared<std::unique_ptr<Scene>>(MakeFlatScene(kN));
        auto cache = std::make_shared<sage::WorldMatrices>();
        Register("scene: мировые матрицы, 20k плоско", "сущность", kN, [scene, cache] {
            (*scene)->ComputeWorldMatrices(*cache);
        });
    }

    // То же на глубокой иерархии. Отдельно, потому что цена здесь совсем другая:
    // у каждой сущности есть родитель, и без мемоизации проход квадратичен.
    {
        constexpr int kChains = 500, kDepth = 20;
        auto scene = std::make_shared<std::unique_ptr<Scene>>(MakeHierarchyScene(kChains, kDepth));
        auto cache = std::make_shared<sage::WorldMatrices>();
        Register("scene: мировые матрицы, 10k иерархия x20", "сущность", kChains * kDepth,
                 [scene, cache] { (*scene)->ComputeWorldMatrices(*cache); });
    }

    // Сбор освещения кадра: обход всех светов сцены. Зовётся раз в кадр, но
    // раскладывает точечные и прожекторные источники в векторы, и на сцене с
    // сотней ламп это уже не бесплатно.
    {
        auto scene = std::make_shared<std::unique_ptr<Scene>>(MakeFlatScene(4000));
        for (int i = 0; i < 120; ++i) {
            GameObject l = (*scene)->CreateObject("lamp");
            LightComponent& lc = l.Registry()->emplace<LightComponent>(l.Entity());
            lc.Kind = (i % 3 == 0) ? LightComponent::Type::Spot : LightComponent::Type::Point;
            l.GetTransform().Position = {(float)i, 3.0f, 0.0f};
        }
        Register("scene: сбор освещения, 120 ламп", "лампа", 120,
                 [scene] {
                     const LightingEnvironment env = sage::ecs::CollectLighting(**scene);
                     if (env.PointLights.size() > 9999) std::printf("!");
                 });
    }
}

void RegisterCullingCases() {
    // Математика отсечения в чистом виде: сфера против шести плоскостей. Это
    // самый горячий цикл кадра — он идёт по каждому объекту в каждом проходе.
    {
        constexpr int kN = 200000;
        auto centers = std::make_shared<std::vector<glm::vec4>>();
        centers->reserve(kN);
        for (int i = 0; i < kN; ++i) {
            centers->push_back({(float)((i * 37) % 400) - 200.0f, (float)((i * 11) % 40),
                                (float)((i * 53) % 400) - 200.0f, 1.5f});
        }
        auto frustum = std::make_shared<sage::render::Frustum>(
            sage::render::Frustum::FromViewProj(BenchViewProj()));

        Register("cull: сфера против пирамиды, 200k", "сфера", kN, [centers, frustum] {
            int visible = 0;
            for (const glm::vec4& c : *centers) {
                if (frustum->IntersectsSphere(glm::vec3(c), c.w)) ++visible;
            }
            // Результат обязан быть использован, иначе оптимизатор имеет полное
            // право выбросить весь цикл — и замер покажет ноль наносекунд.
            if (visible < 0) std::printf("%d", visible);
        });
    }
}

void RegisterJobCases() {
    // Накладные расходы самого ParallelFor. Меряются на ПУСТОЙ работе: если они
    // сравнимы с полезной нагрузкой, распараллеливать нечего, и это надо знать
    // числом, а не на глаз.
    {
        constexpr int kCalls = 2000;
        Register("jobs: ParallelFor вхолостую, 2000 вызовов", "вызов", kCalls, [] {
            volatile int sink = 0;
            for (int i = 0; i < kCalls; ++i) {
                sage::JobSystem::Get().ParallelFor(64, [&](std::size_t b, std::size_t e) {
                    (void)b; (void)e;
                });
                sink = sink + 1;
            }
            (void)sink;
        });
    }
}

void RegisterPhysicsCases() {
    // Шаг встроенной физики на куче ящиков: широкая фаза, узкая фаза, решатель.
    //
    // МИР СБРАСЫВАЕТСЯ ПЕРЕД КАЖДЫМ ПРОГОНОМ, и без этого замер врал вдвое.
    // Тела за шестьдесят шагов успевают упасть, улечься и заснуть — то есть
    // второй прогон начинается из другого мира, чем первый, и стоит вдвое
    // дешевле. Разброс выходил под шестьдесят процентов, и по такому числу
    // нельзя сказать вообще ничего: любая правка тонет в нём целиком.
    //
    // Сброс тоже внутри замера — он честно входит в цену, но триста вызовов
    // SetBodyTransform на фоне шестидесяти шагов это доли процента.
    {
        auto world = std::make_shared<sage::physics::BuiltinWorld>();
        world->SetGravity({0.0f, -9.81f, 0.0f});
        sage::physics::BodyDesc ground;
        ground.Shape = sage::physics::ShapeType::Box;
        ground.HalfExtents = {40.0f, 1.0f, 40.0f};
        ground.Position = {0.0f, -1.0f, 0.0f};
        ground.Type = sage::physics::BodyType::Static;
        world->CreateBody(ground);

        // Стопка, а не россыпь: контактов больше всего именно в стопке, и
        // решатель — самое дорогое место шага — нагружается ею честно.
        auto boxes = std::make_shared<std::vector<sage::physics::BodyHandle>>();
        auto starts = std::make_shared<std::vector<glm::vec3>>();
        for (int i = 0; i < 300; ++i) {
            sage::physics::BodyDesc d;
            d.Shape = sage::physics::ShapeType::Box;
            d.HalfExtents = {0.5f, 0.5f, 0.5f};
            d.Position = {(float)((i % 15) - 7) * 1.2f, 1.0f + (float)(i / 15) * 1.3f,
                          (float)((i / 15) % 5) * 1.2f};
            d.Type = sage::physics::BodyType::Dynamic;
            boxes->push_back(world->CreateBody(d));
            starts->push_back(d.Position);
        }

        Register("physics: 300 ящиков, 60 шагов", "шаг тела", 300 * 60,
                 [world, boxes, starts] {
                     for (std::size_t i = 0; i < boxes->size(); ++i) {
                         world->SetBodyTransform((*boxes)[i], (*starts)[i],
                                                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                         world->SetLinearVelocity((*boxes)[i], glm::vec3(0.0f));
                     }
                     for (int i = 0; i < 60; ++i) world->Step(1.0f / 60.0f);
                 });
    }
}

void RegisterScriptCases() {
    // Стоимость вызова скрипта: 200 сущностей со скриптом, 200 кадров. Меряется
    // не Lua сам по себе, а ГРАНИЦА — переход в скрипт и доступ к компонентам
    // через usertype'ы, потому что дорого именно это.
    {
        auto se = std::make_shared<ScriptEngine>();
        auto scene = std::make_shared<Scene>("bench-lua");
        se->BindScene(*scene);
        {
            std::ofstream f("sage_bench_script.lua");
            f << "function OnUpdate(e, dt)\n"
                 "  local t = e.Transform\n"
                 "  t.Position.x = t.Position.x + dt\n"
                 "  t.Rotation.y = t.Rotation.y + dt * 30.0\n"
                 "end\n";
        }
        for (int i = 0; i < 200; ++i)
            se->AttachScript(scene->CreateObject("o"), "sage_bench_script.lua");
        std::remove("sage_bench_script.lua");

        Register("script: 200 OnUpdate x 200 кадров", "вызов", 200 * 200, [se] {
            for (int i = 0; i < 200; ++i) se->UpdateAll(1.0f / 60.0f);
        });
    }
}

void RegisterUiCases() {
    // Раскладка интерфейса: якоря, растяжения, сетки, обход иерархии. Зовётся
    // каждый кадр, а экран инвентаря — это полторы сотни элементов.
    {
        auto scene = std::make_shared<Scene>("bench-ui");
        auto put = [&](GameObject obj, glm::vec2 pos, glm::vec2 size, bool interactive) {
            sage::ui::LegacyElement e;
            e.Type = sage::ui::LegacyElement::Kind::Panel;
            e.Anchor = UIAnchor::TopLeft;
            e.Offset = pos;
            e.Size = size;
            e.Interactive = interactive;
            sage::ui::Decompose(e, scene->Registry(), obj.Entity());
        };
        GameObject root = scene->CreateObject("Canvas");
        put(root, {0.0f, 0.0f}, {1920.0f, 1080.0f}, false);
        // Ячейки ИНТЕРАКТИВНЫЕ: раскладка ищет, кто под курсором, и на пассивных
        // элементах этот поиск не делается вовсе — замер мерил бы половину
        // работы, которую делает настоящий экран инвентаря.
        for (int i = 0; i < 240; ++i) {
            GameObject slot = scene->CreateObject("slot");
            put(slot, {(float)((i % 16) * 56), (float)((i / 16) * 56)}, {52.0f, 52.0f}, true);
            scene->SetParent(slot.Entity(), root.Entity());
        }
        sage::ui::UIInputState input;
        Register("ui: раскладка 240 элементов x 200 кадров", "элемент", 240 * 200,
                 [scene, input] {
                     for (int i = 0; i < 200; ++i)
                         sage::ui::UpdateSceneUI(*scene, input, 1920, 1080);
                 });
    }
}

void RegisterSerializationCases() {
    // Сохранение и загрузка сцены. Не каждый кадр, но именно этим измеряется
    // «долго грузится»: две тысячи объектов — обычный уровень.
    {
        auto scene = std::make_shared<std::unique_ptr<Scene>>(MakeFlatScene(2000));
        Register("scene: сохранить 2000 объектов", "объект", 2000, [scene] {
            const std::string text = SceneSerializer::SaveToString(**scene);
            if (text.empty()) std::printf("!");
        });

        auto text = std::make_shared<std::string>(SceneSerializer::SaveToString(**scene));
        Register("scene: загрузить 2000 объектов", "объект", 2000, [text] {
            std::unique_ptr<Scene> dst = SceneSerializer::LoadFromString(*text);
            if (!dst) std::printf("!");
        });
    }
}

} // namespace

void RegisterAllCases() {
    RegisterSceneCases();
    RegisterCullingCases();
    RegisterJobCases();
    RegisterPhysicsCases();
    RegisterScriptCases();
    RegisterUiCases();
    RegisterSerializationCases();
}

} // namespace sage::bench
