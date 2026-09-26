#include "sage/core/Application.h"

#include <cctype>

#include "sage/core/CrashHandler.h"
#include "sage/core/Log.h"
#include "sage/core/Systems.h"
#include "sage/core/JobSystem.h"
#include "sage/core/Profiler.h"
#include "sage/render/ResourceManager.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace sage {

namespace {

// Какой бэкенд действительно поднимать. Настройка приходит СТРОКОЙ — из
// sage.cfg, из SAGE_BACKEND, в конечном счёте от человека, — и ошибиться в ней
// можно тремя способами: опечататься, попросить бэкенд, которого нет в этой
// сборке, и попросить тот, для которого на машине нет драйвера. Ни один из трёх
// не должен мешать запуску: движок берёт OpenGL и говорит почему.
//
// Отказ запускаться из-за строчки в конфиге — самый дорогой из возможных
// исходов. Человек, у которого игра перестала открываться после обновления
// драйвера, не станет читать лог: он удалит игру.
rhi::Backend ChooseBackend(const std::string& requested) {
    rhi::Backend backend = rhi::Backend::OpenGL;
    if (!rhi::GraphicsDevice::ParseBackend(requested, backend)) {
        std::string lower;
        for (char c : requested) lower += (char)std::tolower((unsigned char)c);
        if (lower.rfind("d3d", 0) == 0 || lower.rfind("directx", 0) == 0 || lower.rfind("dx", 0) == 0) {
            LOG_WARN("RHI") << "DirectX в движке пока нет (шейдеры написаны на GLSL) — беру OpenGL";
            return rhi::Backend::OpenGL;
        }
        LOG_WARN("RHI") << "неизвестный графический бэкенд '" << requested
                        << "' — беру OpenGL (доступны: opengl, vulkan, null)";
        return rhi::Backend::OpenGL;
    }
    if (backend == rhi::Backend::OpenGL) return backend;

    if (!rhi::GraphicsDevice::Available(backend)) {
        LOG_WARN("RHI") << "бэкенд " << rhi::GraphicsDevice::BackendId(backend)
                        << " на этой машине недоступен (нет драйвера или сборка без него)"
                           " — беру OpenGL";
        return rhi::Backend::OpenGL;
    }

    // Vulkan-бэкенд ЕЩЁ НЕ ДОДЕЛАН: устройство поднимается, ресурсы — нет.
    // Пока это так, он берётся только по явной просьбе, а обычный выбор в
    // настройках откатывается на OpenGL с объяснением.
    //
    // Гейт стоит здесь, а не в Available(): «Vulkan на машине есть» и «наш
    // Vulkan умеет рисовать» — разные утверждения, и подменять первое вторым
    // значило бы соврать в диагностике. Снимается вместе с последним этапом
    // бэкенда — тогда же уходит и эта ветка.
    if (backend == rhi::Backend::Vulkan && !std::getenv("SAGE_VULKAN_EXPERIMENTAL")) {
        LOG_WARN("RHI") << "бэкенд vulkan в этой сборке ещё не доделан (нет ресурсов и "
                           "конвейеров) — беру OpenGL. Принудительно: SAGE_VULKAN_EXPERIMENTAL=1";
        return rhi::Backend::OpenGL;
    }

    LOG_INFO("RHI") << "графический бэкенд: " << rhi::GraphicsDevice::BackendId(backend);
    return backend;
}

} // namespace

Application* Application::s_instance = nullptr;

Application::Application(const AppConfig& config) : m_config(config) {
    if (s_instance) {
        throw std::runtime_error("Application: уже существует экземпляр (Application — синглтон)");
    }
    s_instance = this;

    // Подсистемы движка. Обычно контекст уже поднят точкой входа (SAGE_MAIN):
    // игра настраивает EngineConfig до создания окна, то есть до этого
    // конструктора. Своим контекстом приложение обзаводится только если его
    // собрали в обход SAGE_MAIN — чтобы такой вызов не падал на пустом месте.
    m_context = EngineContext::CurrentOrNull();
    if (!m_context) {
        m_ownedContext = std::make_unique<EngineContext>();
        m_context = m_ownedContext.get();
        EngineContext::SetCurrent(m_context);
    }

    // Окно создаёт графический контекст (но НЕ трогает GL). Затем поднимаем
    // графическое устройство выбранного бэкенда: оно грузит драйвер и выставляет
    // дефолтное состояние конвейера. После этого конструкции слоёв (и их
    // OnAttach) уже могут создавать GPU-ресурсы.
    Window::Params wp;
    wp.Mode = config.Mode;
    wp.Maximized = config.Maximized;
    wp.Resizable = config.Resizable;
    wp.VSync = config.VSync;
    wp.Msaa = config.Msaa;
    m_window = std::make_unique<Window>(config.Width, config.Height, config.Title, wp);

    m_device = rhi::GraphicsDevice::Create(ChooseBackend(config.Backend));
    // Шаги запуска отмечаются в логе НЕ ради болтливости. Когда на чужой машине
    // «ничего не происходит», единственный доступный факт — докуда дошёл лог; без
    // отметок он обрывается на первой строке, и место отказа определить нечем.
    // Обработчик падений ставится в main ДО всего (см. SAGE_MAIN), но лог к
    // тому моменту ещё не открыт — сказать об этом можно только здесь. Строка
    // не декоративная: по ней видно, что отчёт о падении будет, а если её нет —
    // что искать его бессмысленно.
    LOG_INFO("Engine") << "Обработчик падений: "
                       << (CrashHandler::Installed() ? "установлен" : "НЕ УСТАНОВЛЕН");
    LOG_INFO("Engine") << "Запуск: окно готово, поднимаю графическое устройство";
    m_device->Init(reinterpret_cast<rhi::ProcLoader>(glfwGetProcAddress));
    // Реальный размер окна может отличаться от запрошенного (fullscreen/borderless
    // берут разрешение монитора) — берём фактический.
    m_device->SetViewport(0, 0, m_window->Width(), m_window->Height());
    rhi::GraphicsDevice::SetCurrent(m_device.get());
    m_context->SetDevice(m_device.get());

    // Пул задач: фоновые воркеры под параллельную подготовку кадра (отсечение/
    // батчи). Поднимается один раз на процесс; слои затем зовут ParallelFor.
    m_context->Jobs().Initialize(config.WorkerThreads);
    m_context->Jobs().SetEnabled(config.MultithreadedRender);

    // Состав и версии подсистем — в лог любой сборки (игра/редактор/рантайм).
    LogEngineSystems();
    LOG_INFO("Engine") << "Запуск: движок готов, передаю управление слоям";
}

Application::~Application() {
    // Слои отсоединяются в конструкторно-обратном порядке, пока окно (и, значит,
    // GL-контекст) ещё живо — иначе GPU-ресурсы слоёв удалялись бы уже без
    // контекста. Само окно разрушится ниже, после того как стек слоёв пуст.
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        (*it)->OnDetach();
    }
    m_layers.clear();

    // РЕСУРСЫ ВИДЕОКАРТЫ ОСВОБОЖДАЮТСЯ ЗДЕСЬ — пока GL-контекст ещё жив.
    //
    // Это одна строка, но за ней стоит вся причина существования EngineContext.
    // Раньше кэш ресурсов был функцией-статиком и разрушался обработчиком
    // выхода процесса — то есть УЖЕ ПОСЛЕ того, как окно и устройство
    // уничтожены (см. m_window.reset() ниже). Освобождение буфера видеокарты в
    // этот момент — вызов по указателю на функцию, которой больше нет: процесс
    // падал на выходе, когда в логе уже всё написано, отчёт напечатан и причина
    // не видна. Снаружи это «игра отработала и упала» с кодом 139
    // (см. tests/exit_probe.cpp и docs/history.md).
    //
    // Обходной путь — звать Clear() у себя в OnDetach — знали все четыре
    // потребителя движка, и НИ ОДИН из них не был обязан: знание жило в памяти
    // авторов, а не в движке. Теперь порядок — не соглашение, а строка кода:
    // временем жизни устройства владеет Application, значит он и закрывает
    // подсистемы, пока устройство живо. Вызов идемпотентен, явные Clear() у
    // потребителей от него не ломаются.
    m_context->ReleaseGpuResources();

    // Останавливаем фоновые воркеры ДО разрушения GL/окна: их задачи могли бы
    // ещё держать ссылки на данные слоёв. Джойн гарантирует чистое завершение.
    m_context->Jobs().Shutdown();
    m_context->SetDevice(nullptr);
    rhi::GraphicsDevice::SetCurrent(nullptr);
    m_device.reset();
    m_window.reset();
    // Свой контекст (если он свой) уходит последним — после окна и устройства,
    // но разрушать в нём уже нечего: GPU-объекты закрыты выше.
    if (m_ownedContext) {
        EngineContext::SetCurrent(nullptr);
        m_ownedContext.reset();
        m_context = nullptr;
    }
    s_instance = nullptr;
}

Layer* Application::PushLayer(std::unique_ptr<Layer> layer) {
    Layer* raw = layer.get();
    const std::string name = raw->Name();
    m_layers.push_back(std::move(layer));
    // Отметки вокруг OnAttach: именно там живёт самая долгая и самая хрупкая
    // часть запуска (шейдеры, шрифты, проект, ассеты). Если лог обрывается
    // между этими двумя строками — известно и КТО упал, и что это случилось не
    // в движке, а в слое.
    LOG_INFO("Engine") << "Запуск слоя '" << name << "'";
    raw->OnAttach();
    LOG_INFO("Engine") << "Слой '" << name << "' запущен";
    return raw;
}

void Application::Close() {
    m_running = false;
}

void Application::Run() {
    float lastFrame = (float)glfwGetTime();
    float fpsTimer = 0.0f;
    int fpsFrames = 0;

    // Профилирование в собранной игре включается переменной окружения, а не
    // сборкой: разбираться, почему у ИГРОКА просело, приходится на его машине
    // и на его драйвере, и требовать ради этого отладочную сборку значит не
    // разобраться никогда. SAGE_PROFILE_LOG=<сек> вдобавок печатает таблицу в
    // лог — единственный способ увидеть замеры там, где окна нет вовсе:
    // headless-прогон, CI, сервер сборки.
    float profileLogEvery = 0.0f;
    float profileLogTimer = 0.0f;
    if (std::getenv("SAGE_PROFILE")) profile::SetEnabled(true);
    if (const char* s = std::getenv("SAGE_PROFILE_LOG")) {
        profileLogEvery = std::max(0.5f, (float)std::atof(s));
        profile::SetEnabled(true);
    }

    while (m_running) {
        // ЗАКРЫТИЕ — ВОПРОС, А НЕ ПРИКАЗ. Крестик и Alt+F4 ставят флаг GLFW;
        // прежде чем выйти, спрашиваем слои. Любой ответивший «нет» отменяет
        // закрытие, и цикл продолжается — слой показывает свой вопрос
        // («сцена не сохранена») и закроет приложение сам, когда ответят.
        //
        // Спрашиваем ЗДЕСЬ, в начале кадра, а не внутри кадра: флаг ставится в
        // PollEvents в конце предыдущего, и любая проверка внутри OnRender
        // просто не успевала бы выполниться — цикл уже вышел бы по условию.
        if (m_window->ShouldClose()) {
            bool allow = true;
            for (auto& layer : m_layers) {
                if (!layer->OnCloseRequest()) allow = false;
            }
            if (!allow) m_window->CancelClose();
            else break;
        }

        float now = (float)glfwGetTime();
        // Ограничитель dt: защита от «рывка» симуляции после паузы/лага/точки
        // останова (тот же приём, что был в исходном игровом цикле).
        m_deltaTime = std::min(now - lastFrame, m_config.MaxDeltaTime);
        lastFrame = now;

        // Простой счётчик FPS (обновляется дважды в секунду) — движок отдаёт
        // его через Fps(), чтобы отладочные оверлеи не считали его сами.
        fpsTimer += m_deltaTime;
        ++fpsFrames;
        if (fpsTimer >= 0.5f) { m_fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

        // Заливаем в VRAM текстуры, декодированные фоновым потоком (стриминг
        // ассетов). Только здесь — у главного потока единственного GL-контекст.
        profile::BeginFrame();
        {
            SAGE_PROFILE("Загрузка ресурсов");
            ResourceManager::Instance().PumpAsyncUploads();
        }

        {
            SAGE_PROFILE("Обновление");
            for (auto& layer : m_layers) layer->OnUpdate(m_deltaTime);
        }

        // Viewport экранного буфера держим в размер окна каждый кадр (раньше
        // это делал Window::OnResize напрямую через glViewport; теперь окно к
        // GL не обращается — за viewport отвечает графический слой).
        m_device->SetViewport(0, 0, m_window->Width(), m_window->Height());
        {
            SAGE_PROFILE("Отрисовка");
            for (auto& layer : m_layers) layer->OnRender();
        }

        // Обмен буферов меряем отдельно и НЕ считаем «нашим» временем: при
        // включённой вертикальной синхронизации здесь стоит ожидание развёртки,
        // и большое число тут означает «мы успеваем», а не «мы тормозим».
        {
            SAGE_PROFILE("Обмен буферов");
            m_window->SwapBuffers();
        }
        m_window->PollEvents();

        // Кадр закрываем ДО ограничителя частоты: досып до 1/cap — это
        // намеренное безделье, и включать его в стоимость кадра значило бы
        // показывать 16 мс там, где работы было на 3.
        profile::EndFrame();

        if (profileLogEvery > 0.0f) {
            profileLogTimer += m_deltaTime;
            if (profileLogTimer >= profileLogEvery) {
                profileLogTimer = 0.0f;
                LOG_INFO("Profile") << profile::Summary();
                for (const profile::Entry& e : profile::Average()) {
                    // Выравнивание считаем по СИМВОЛАМ, а не по байтам: имена
                    // участков кириллические, в UTF-8 это два байта на букву, и
                    // ширина поля в printf разъезжает колонки ровно там, где
                    // таблицу и читают.
                    size_t glyphs = 0;
                    for (unsigned char c : e.Name)
                        if ((c & 0xC0) != 0x80) ++glyphs;
                    const size_t indent = (size_t)e.Depth * 2;
                    const size_t width = 26;
                    std::string pad(indent + glyphs < width ? width - indent - glyphs : 1, ' ');
                    char tail[96];
                    std::snprintf(tail, sizeof(tail), "CPU %7.3f мс   GPU %7.3f мс", e.CpuMs,
                                  e.GpuMs);
                    LOG_INFO("Profile") << std::string(indent, ' ') << e.Name << pad << tail;
                }
            }
        }

        // Ограничитель кадров (если задан и без VSync): досыпаем до 1/cap секунды.
        if (m_config.FrameCap > 0) {
            float target = 1.0f / (float)m_config.FrameCap;
            float frameTime = (float)glfwGetTime() - now;
            if (frameTime < target) {
                glfwWaitEventsTimeout(target - frameTime); // спит, но не игнорирует события
            }
        }
    }

    LOG_INFO("Engine") << "Завершение работы";
}

} // namespace sage
