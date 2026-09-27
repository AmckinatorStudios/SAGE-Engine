#pragma once
#include <string>

// ---------------------------------------------------------------------------
// ТЕСТЫ НА ЯЗЫКЕ СКРИПТОВ — результат одного теста.
//
// Тест-скрипт — обычный скрипт объекта, у которого есть методы test_*:
//
//     local T = {}
//     function T:test_button_click()
//         local b = UI.create("Button", "Play")
//         local n = 0
//         b.clicked:connect(function() n = n + 1 end)
//         Test.click(b)
//         Test.eq(n, 1)
//     end
//     return T
//
// Каждый тест идёт КОРУТИНОЙ в настоящем кадре игры: `Test.wait(0.5)` и
// `Test.frames(3)` отдают кадр движку (физика шагает, UI обновляется,
// Update других скриптов идёт), и тест продолжается с того же места. Так
// проверяется то, что проверить вызовом функции нельзя: падение тела,
// срабатывание таймера, сигнал, пришедший через кадр.
// ---------------------------------------------------------------------------
namespace sage::scripting {

struct TestResult {
    std::string Script;   // путь файла теста
    std::string Name;     // имя метода: test_button_click
    bool Passed = false;
    std::string Message;  // почему упал (пусто у прошедшего)
    std::string File;     // где упал (файл:строка из ошибки языка)
    int Line = 0;
    float Seconds = 0.0f; // игрового времени
    int Frames = 0;
};

} // namespace sage::scripting
