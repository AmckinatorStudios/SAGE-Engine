#pragma once
#include <cstdio>
#include <exception>
#include "sage/core/Application.h"

// ---------------------------------------------------------------------------
// GameModule — контракт точки входа между движком и игрой.
//
// Точку входа (main) даёт ДВИЖОК через макрос SAGE_MAIN(): он оборачивает
// запуск в try/catch и владеет жизненным циклом Application. Игра лишь
// реализует одну функцию — sage::CreateApplication(argc, argv) — которая
// конфигурирует Application и добавляет свои слои. Так игра не дублирует
// цикл/обработку ошибок и остаётся тонкой надстройкой над движком.
//
// Пример (games/<name>/src/main.cpp):
//     #include "sage/core/GameModule.h"
//     sage::Application* sage::CreateApplication(int, char**) {
//         auto* app = new sage::Application({.Title = "My Game"});
//         app->PushLayer(std::make_unique<MyGameLayer>());
//         return app;
//     }
//     SAGE_MAIN()
// ---------------------------------------------------------------------------

namespace sage {
// Реализуется КАЖДОЙ игрой/редактором. Возвращает готовый к запуску
// Application (владение передаётся вызывающему — движку).
Application* CreateApplication(int argc, char** argv);
} // namespace sage

// Куда уходит фатальная ошибка запуска.
//
// БЫЛО: std::fprintf(stderr). У окна Windows-приложения консоли НЕТ (сборка
// GUI-подсистемы), и stderr уходит в никуда: человек запускает редактор,
// ничего не происходит, ошибки не видно, а в логе — только строка «запускается»
// и тишина. Ровно так это и описывают: «просто запускаю, ничего не
// происходит».
//
// СТАЛО: три адреса сразу, потому что у каждого своя дыра. Лог — чтобы причина
// осталась на диске и её можно было прислать. Окно с сообщением — чтобы
// человек увидел её без всякого лога. stderr — чтобы её было видно при запуске
// из консоли и в CI.
namespace sage::detail {
void ReportFatal(const char* what);

// Обработчик падений — ДО всего остального.
//
// Он ставился в OnAttach слоя редактора, то есть уже ПОСЛЕ создания окна и
// загрузки драйвера, — мимо самого опасного участка запуска. Падение там (а
// «ничего не происходит при запуске» обычно именно там) не оставляло ни
// отчёта, ни строки: процесс исчезал молча. Здесь ставится минимальный
// обработчик; слой при желании переустановит свой, с аварийным сохранением.
void InstallEarlyCrashHandler(const char* appName);
} // namespace sage::detail

#define SAGE_MAIN()                                                        \
    int main(int argc, char** argv) {                                      \
        sage::detail::InstallEarlyCrashHandler(argv && argv[0] ? argv[0] : "SAGE"); \
        try {                                                              \
            sage::Application* app = sage::CreateApplication(argc, argv);  \
            app->Run();                                                    \
            delete app;                                                    \
        } catch (const std::exception& e) {                                \
            sage::detail::ReportFatal(e.what());                           \
            return -1;                                                     \
        } catch (...) {                                                    \
            /* Исключение не от std::exception — редкость, но молчать о нём \
               нельзя: «ничего не произошло» одинаково для обоих. */        \
            sage::detail::ReportFatal("неизвестное исключение");           \
            return -1;                                                     \
        }                                                                  \
        return 0;                                                          \
    }
