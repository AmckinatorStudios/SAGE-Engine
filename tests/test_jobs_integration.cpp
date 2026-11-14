// Интеграция подсистем с единой системой задач: физика Jolt идёт задачами
// движка (а не своим пулом), ресурсы декодируются на потоках ввода-вывода,
// задачи скриптов — в общей системе. Без GL.
#include "TestFramework.h"

#include "sage/jobs/JobSystem.h"
#include "sage/physics/PhysicsWorld.h"

#include <atomic>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace sage::physics;

namespace {
sage::JobSystem& IntegrationWorkers(int n = 4) {
    sage::JobSystem& js = sage::JobSystem::Get();
    if (!js.IsRunning() || js.WorkerCount() != n) {
        js.Shutdown();
        js.Initialize(n);
    }
    js.SetEnabled(true);
    return js;
}

uint64_t ExecutedIn(const sage::jobs::JobSystemStats& st, sage::jobs::Category c) {
    for (const auto& cs : st.Categories)
        if (cs.Cat == c) return cs.Executed;
    return 0;
}
} // namespace

TEST(JobsIntegration_jolt_steps_on_engine_workers) {
    if (!PhysicsWorld::HasJolt()) return;  // сборка без Jolt — проверять нечего
    sage::JobSystem& js = IntegrationWorkers(4);
    js.ResetStats();
    auto w = PhysicsWorld::Create(Backend::Jolt);
    w->SetGravity({0, -9.81f, 0});
    BodyDesc floor;
    floor.Type = BodyType::Static;
    floor.HalfExtents = {50.0f, 0.5f, 50.0f};
    w->CreateBody(floor);
    std::vector<BodyHandle> boxes;
    for (int i = 0; i < 300; ++i) {
        BodyDesc d;
        d.Type = BodyType::Dynamic;
        d.Position = {(float)(i % 15) * 1.2f - 9.0f, 1.0f + (float)(i / 15) * 1.1f, 0.0f};
        boxes.push_back(w->CreateBody(d));
    }
    for (int f = 0; f < 120; ++f) w->Step(1.0f / 60.0f);
    // Шаг дошёл до конца и тела легли, а не провалились сквозь пол.
    bool resting = true;
    for (BodyHandle b : boxes) {
        glm::vec3 p;
        glm::quat q;
        w->GetBodyTransform(b, p, q);
        resting &= p.y > 0.0f;
    }
    CHECK_TRUE(resting);
    // Задачи солвера прошли через систему задач движка.
    CHECK_TRUE(ExecutedIn(js.Stats(), sage::jobs::Category::Physics) > 0);
}

// --- Ресурсы: фоновое декодирование — задачи полосы ввода-вывода -------------

#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"

#include <chrono>
#include <thread>

TEST(JobsIntegration_async_texture_decodes_on_io_lane_and_uploads_on_main) {
    sage::JobSystem& js = IntegrationWorkers(4);
    js.ResetStats();
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    device->Init(nullptr);
    sage::rhi::GraphicsDevice::SetCurrent(device.get());
    {
        ResourceManager rm;
        const std::string file = std::string(SAGE_TEST_DATA_DIR) + "/blockbench_skin/05119a8b74f4a93d.png";
        auto tex = rm.GetTextureAsync(file, TextureFilter::Nearest, false);
        CHECK_TRUE(tex != nullptr);
        CHECK_EQ(tex->Width(), 1);  // пока — заглушка
        CHECK_EQ(rm.PendingAsyncTextures(), 1);
        int uploaded = 0;
        for (int i = 0; i < 500 && uploaded == 0; ++i) {
            uploaded = rm.PumpAsyncUploads();  // главный поток не ждёт — забирает готовое
            if (!uploaded) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK_EQ(uploaded, 1);
        CHECK_TRUE(tex->Width() > 1);  // тот же объект получил настоящие пиксели
        CHECK_EQ(rm.PendingAsyncTextures(), 0);
        CHECK_TRUE(ExecutedIn(js.Stats(), sage::jobs::Category::Resources) >= 1);

        // Очистка кэша посреди загрузки: задачи отменяются/дожидаются, а не
        // пишут в умершие записи.
        for (int i = 0; i < 20; ++i)
            rm.GetTextureAsync(file, i % 2 ? TextureFilter::Bilinear : TextureFilter::Trilinear, i % 3 == 0);
        rm.Clear();
        CHECK_EQ(rm.PendingAsyncTextures(), 0);
    }
    sage::rhi::GraphicsDevice::SetCurrent(nullptr);
}

// --- Рендер: устройство знает свой поток, фоновая задача идёт в очередь рендера

TEST(JobsIntegration_gpu_calls_only_from_device_thread) {
    sage::JobSystem& js = IntegrationWorkers(4);
    auto device = sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::Null);
    device->BindToCurrentThread();
    CHECK_TRUE(device->CheckThread("тест: свой поток"));
    const int before = sage::rhi::GraphicsDevice::ThreadViolations();
    // Прямой вызов из фоновой задачи — нарушение, пойманное и посчитанное.
    std::atomic<bool> fromIo{true};
    js.Run([&] { fromIo = device->CheckThread("тест: из потока ввода-вывода"); },
           sage::jobs::JobOptions().OnLane(sage::jobs::Lane::IO)).Wait();
    CHECK_FALSE(fromIo.load());
    CHECK_EQ(sage::rhi::GraphicsDevice::ThreadViolations(), before + 1);
    // Правильный путь: фоновая задача готовит данные, «заливку» выполняет
    // очередь рендера в потоке устройства.
    std::atomic<bool> uploadOk{false};
    sage::jobs::Future<int> prepared = js.Async([] { return 42; },
                                                sage::jobs::JobOptions().OnLane(sage::jobs::Lane::IO));
    sage::jobs::JobOptions renderLane("upload", sage::jobs::Category::Render, sage::jobs::Lane::Render);
    sage::jobs::JobHandle upload = prepared.Then([&](int& v) { uploadOk = v == 42 && device->CheckThread("заливка"); },
                                                 renderLane);
    prepared.Wait();
    for (int i = 0; i < 200 && !upload.IsDone(); ++i) {
        js.PumpRenderThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_TRUE(upload.IsDone());
    CHECK_TRUE(uploadOk.load());
    CHECK_EQ(sage::rhi::GraphicsDevice::ThreadViolations(), before + 1);
}
