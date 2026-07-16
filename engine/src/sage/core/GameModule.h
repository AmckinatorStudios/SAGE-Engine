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

#define SAGE_MAIN()                                                        \
    int main(int argc, char** argv) {                                      \
        try {                                                              \
            sage::Application* app = sage::CreateApplication(argc, argv);  \
            app->Run();                                                    \
            delete app;                                                    \
        } catch (const std::exception& e) {                               \
            std::fprintf(stderr, "Фатальная ошибка: %s\n", e.what());     \
            return -1;                                                     \
        }                                                                  \
        return 0;                                                          \
    }
