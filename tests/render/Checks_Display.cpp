// ---------------------------------------------------------------------------
// Режим экрана на ходу: «Проверить на этом экране» в настройках игры.
//
// Здесь, а не в sage_tests: проверке нужно НАСТОЯЩЕЕ окно GLFW (размер
// меняет оконная система), а оно есть только у этого прогона.
//
// Обещание окна настроек — «не подтвердишь, режим вернётся сам». Если откат
// сломается, человек, выбравший режим, который его монитор не тянет, останется
// с чёрным экраном и без кнопки «Отмена». Поэтому проверяется именно откат по
// времени, а не только то, что режим применяется.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>

#include <GLFW/glfw3.h>

#include "Fixture.h"

#include "sage/core/Display.h"
#include "sage/core/Window.h"

namespace sage::rendertest {

namespace {
// Оконная система меняет размер не мгновенно: событие приходит позже.
void Settle() {
    for (int i = 0; i < 20; ++i) glfwWaitEventsTimeout(0.01);
}
} // namespace

void RunDisplayChecks(Window& window) {
    std::printf("\n--- Режим экрана: проба и откат ---\n");
    const Window::Display before = window.CurrentDisplay();

    sage::display::DisplayRequest next;
    next.Mode = sage::WindowMode::Windowed;
    next.Width = 320;
    next.Height = 200;

    // Проба без ответа — по истечении времени окно возвращается само.
    sage::display::DisplayTrial trial;
    std::string err;
    const bool began = trial.Begin(window, next, 100.0, 15.0, &err);
    Settle();
    Check(began, "проба режима экрана применилась");
    const Window::Display during = window.CurrentDisplay();
    Check(during.Width == 320 && during.Height == 200, "на время пробы окно получило выбранный размер");
    Check(trial.Active() && trial.SecondsLeft(105.0) > 9.9, "до конца пробы идёт обратный отсчёт");
    Check(!trial.Tick(window, 114.9), "раньше срока проба не откатывается");
    Check(trial.Tick(window, 115.0), "по истечении срока проба откатывается сама");
    Settle();
    const Window::Display after = window.CurrentDisplay();
    Check(after.Width == before.Width && after.Height == before.Height && after.Mode == before.Mode,
          "после отката окно — как до пробы");
    Check(!trial.Active(), "после отката проба закрыта");

    // «Оставить» — режим остаётся и таймер его больше не трогает.
    sage::display::DisplayTrial kept;
    kept.Begin(window, next, 0.0, 15.0, nullptr);
    Settle();
    kept.Confirm();
    Check(!kept.Tick(window, 100.0), "подтверждённый режим не откатывается по таймеру");
    Settle();
    Check(window.CurrentDisplay().Width == 320, "подтверждённый режим остался");

    // Вернуть окно прогона как было: наборы после этого не должны зависеть
    // от того, что здесь проверялось.
    window.ApplyDisplay(before, nullptr);
    Settle();
}

} // namespace sage::rendertest
