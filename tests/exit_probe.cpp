// Отдельная программа, которая ПРОСТО ЗАВЕРШАЕТСЯ.
//
// Звучит так, будто проверять нечего, — и ровно поэтому проверка нужна.
//
// ЧТО ОНА ЛОВИТ. Кэш ресурсов (ResourceManager) — глобальный синглтон, то есть
// функция-статик. Такие разрушаются обработчиком выхода из процесса, ПОСЛЕ
// того как Application уничтожил окно и GL-контекст. Освобождение буфера
// видеокарты в этот момент — вызов по указателю на функцию, которого больше
// нет: процесс падает уже после последней строки игры, а стек падения ведёт
// в движок. Со стороны это выглядит как «игра отработала и почему-то упала».
//
// Обход был известен и записан в каждом потребителе движка: звать
// ResourceManager::Instance().Clear() у себя в OnDetach. Четыре одинаковые
// строки в редакторе, плеере, sandbox и testgame — обязательные и никем не
// проверяемые. Новая игра на движке (Война Наций) о них не знала и получила
// падение на выходе с кодом 139.
//
// Поэтому здесь ЭТОЙ СТРОКИ НАМЕРЕННО НЕТ. Программа ведёт себя как игра,
// написанная по документации, а не по памяти сопровождающего: берёт меш из
// кэша, рисует пару кадров и выходит. Проверка одна — КОД ВОЗВРАТА (см.
// scripts/ci_smoke_test.sh). На движке без освобождения кэша в ~Application
// она падает; с ним — возвращает ноль.
//
// Обычным юнит-тестом это не проверить дважды: нужен настоящий GL-контекст, и
// падение случается в обработчике выхода, то есть уже после main.
#include <memory>

#include "sage/core/Application.h"
#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Scene.h"

namespace {

class ExitProbeLayer : public sage::Layer {
public:
    ExitProbeLayer() : sage::Layer("ExitProbe") {}

    void OnAttach() override {
        // Ровно то, что делает любая игра: берёт примитивы из общего кэша.
        // Каждый из них — буфер на видеокарте, который кто-то обязан вернуть.
        m_cube = ResourceManager::Instance().GetCube();
        m_sphere = ResourceManager::Instance().GetSphere();
        m_cylinder = ResourceManager::Instance().GetCylinder();

        GameObject obj = m_scene.CreateObject("Куб");
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = m_cube;

        LOG_INFO("ExitProbe") << "EXITPROBE: кэш ресурсов заполнен";
    }

    // Clear() ЗДЕСЬ НЕТ — см. комментарий в начале файла. Это и есть суть
    // проверки: игра, не знающая про обходной путь, обязана выходить чисто.
    void OnDetach() override { LOG_INFO("ExitProbe") << "EXITPROBE: слой отсоединён"; }

    void OnUpdate(float) override {
        if (++m_frames >= 3) sage::Application::Get().Close();
    }

private:
    Scene m_scene{"ExitProbe"};
    std::shared_ptr<Mesh> m_cube, m_sphere, m_cylinder;
    int m_frames = 0;
};

} // namespace

sage::Application* sage::CreateApplication(int, char**) {
    Log::Init("sage_exit_probe.log");
    sage::AppConfig cfg;
    cfg.Width = 320;
    cfg.Height = 240;
    cfg.Title = "SAGE ExitProbe";
    cfg.VSync = false;
    auto* app = new sage::Application(cfg);
    app->PushLayer(std::make_unique<ExitProbeLayer>());
    return app;
}

SAGE_MAIN()
