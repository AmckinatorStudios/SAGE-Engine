// ============================================================================
//  sage_render_tests — проверка КАРТИНКИ по эталонным кадрам.
//
//  Обычные тесты движка проверяют математику, сериализацию и анимацию. Ошибку в
//  шейдере они не увидят: код собирается, значения считаются, а кадр выглядит
//  иначе. Здесь сцена рисуется в offscreen-буфер и сравнивается с образцом.
//
//  Тест ТРЕБУЕТ графического контекста, поэтому живёт отдельной целью и в CI
//  запускается под xvfb. Про допуски и переносимость эталонов — см. подробный
//  комментарий в RenderTestHarness.h.
//
//  ЗДЕСЬ ТОЛЬКО ПОРЯДОК ПРОГОНА. Сами проверки лежат по темам (Checks_*.cpp),
//  общая оснастка — в Fixture.h. Раньше всё это было одним файлом на две с
//  половиной тысячи строк, в котором рядом стояли отражения, кэш ассетов и
//  соответствие RHI; целым его держала не связность, а анонимное пространство
//  имён — MakeScene и RenderFrame просто не были видны никому снаружи.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <GLFW/glfw3.h>

#include "Fixture.h"

#include "sage/core/Log.h"
#include "sage/core/Window.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rendertest;

int main(int argc, char** argv) {
    std::string referenceDir = "references";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--update") == 0) SetUpdateMode(true);
        else if (std::strcmp(argv[i], "--references") == 0 && i + 1 < argc) referenceDir = argv[++i];
    }
    SetReferenceDir(referenceDir);

    Log::Init("sage_render_tests.log");
    std::printf("SAGE Engine — эталонные кадры\n");
    std::printf("=============================\n");
    if (UpdateMode()) std::printf("РЕЖИМ ОБНОВЛЕНИЯ: эталоны будут перезаписаны\n");
    std::printf("Каталог эталонов: %s\n\n", referenceDir.c_str());

    // Окно нужно только ради графического контекста — на экране ему делать
    // нечего. Размер минимальный: весь рендер идёт в offscreen-буферы.
    Window::Params params;
    params.Hidden = true;
    params.Resizable = false;
    params.VSync = false;
    Window window(64, 64, "sage_render_tests", params);

    // Девайс поднимаем сами: Application нам не нужен (нет ни слоёв, ни цикла),
    // а загрузчик функций GL берём у GLFW — как это делает Application.
    std::unique_ptr<sage::rhi::GraphicsDevice> device =
        sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::OpenGL);
    device->Init(reinterpret_cast<sage::rhi::ProcLoader>(glfwGetProcAddress));
    sage::rhi::GraphicsDevice::SetCurrent(device.get());

    {
        std::unique_ptr<Scene> scene = MakeScene();
        FrameRenderer renderer;
        RunFrameChecks(renderer, *scene);
        RunAnimationChecks(renderer);
        RunReflectionChecks(renderer);
        RunShadowChecks(renderer, *scene);
        RunSceneChecks(renderer);
        RunMaterialChecks(renderer);
        RunVolumetricChecks();
        RunUIChecks();
        RunPaintChecks();
    }

    // GPU-ресурсы освобождаем, пока контекст ещё жив: деструктор синглтона
    // сработал бы уже после разрушения окна.
    ResourceManager::Instance().Clear();
    sage::rhi::GraphicsDevice::SetCurrent(nullptr);

    std::printf("\n=============================\n");
    if (WrittenCount() > 0) {
        std::printf("Записано новых эталонов: %d — проверьте их глазами и закоммитьте.\n",
                    WrittenCount());
    }
    std::printf("Пройдено: %d, провалено: %d\n", PassedCount(), FailedCount());
    // Записанный впервые эталон — это не успех: сравнивать было не с чем.
    // В CI такое означает, что эталон забыли закоммитить.
    if (WrittenCount() > 0 && !UpdateMode()) {
        std::printf("ВНИМАНИЕ: часть эталонов отсутствовала — тест не считается пройденным.\n");
        return 2;
    }
    return FailedCount() == 0 ? 0 : 1;
}
