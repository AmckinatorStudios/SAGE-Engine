// Во что обходится ВЫЗОВ скрипта. Не порог, а цифра: тест печатает время и
// никогда не падает от него — «медленно» на машине сборщика значит слишком
// многое, чтобы делать из этого проверку. Число нужно, чтобы правка скриптовой
// части не удорожала кадр незаметно: 200 скриптов это уже заметная сцена.
//
// Здесь же ответ на вопрос «сколько стоят включённые проверки типов sol2»
// (см. SOL_ALL_SAFETIES_ON в CMakeLists.txt): проверка типа на аргумент, то
// есть единицы наносекунд на вызов. Померить конфигурацию БЕЗ них не выйдет —
// без проверок соседний тест с неверными типами роняет процесс, что само по
// себе исчерпывающий ответ на вопрос, нужны ли они.
#include "TestFramework.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include "sage/scripting/ScriptEngine.h"
#include "sage/scene/Scene.h"
TEST(Bench_script_call_cost) {
    ScriptEngine se; Scene sc("B"); se.BindScene(sc);
    { std::ofstream f("bench.lua");
      f << "function OnUpdate(e, dt)\n  e.Transform.Position.x = e.Transform.Position.x + dt\n"
           "  e.Transform.Rotation.y = e.Transform.Rotation.y + dt * 30.0\n end\n"; }
    for (int i = 0; i < 200; ++i) se.AttachScript(sc.CreateObject("o"), "bench.lua");
    std::remove("bench.lua");
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 500; ++f) se.UpdateAll(0.016f);
    auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("       200 скриптов x 500 кадров: %.1f мс (%.3f мс/кадр)\n", ms, ms / 500.0);
    CHECK_TRUE(true);
}
