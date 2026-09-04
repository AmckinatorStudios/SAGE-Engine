#include "sage/core/GameModule.h"

#include <cstdio>
#include <string>

#include "sage/core/CrashHandler.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sage::detail {

void ReportFatal(const char* what) {
    const std::string text = what ? what : "(без описания)";

    // 1. В ЛОГ. Он остаётся на диске и его можно прислать — единственный
    // источник, доживающий до разбора на чужой машине. Раньше сюда не
    // попадало ничего: лог обрывался на строке «запускается», и по нему
    // выходило, что движок просто исчез.
    // Лог пишет каждую строку с flush (см. Log.cpp), поэтому она уже на диске
    // к моменту, когда процесс закончится.
    LOG_ERROR("Engine") << "ФАТАЛЬНАЯ ОШИБКА ПРИ ЗАПУСКЕ: " << text;

    // 2. В КОНСОЛЬ — для запуска из терминала и для CI.
    std::fprintf(stderr, "Фатальная ошибка: %s\n", text.c_str());
    std::fflush(stderr);

#ifdef _WIN32
    // 3. ОКНОМ. У GUI-приложения Windows консоли нет, и пункт 2 уходит в
    // никуда. Без этого окна запуск выглядит так: щёлкнул — ничего не
    // произошло. Ни ошибки, ни окна, ни намёка, куда смотреть.
    const std::string message =
        text + "\n\nПодробности записаны в лог рядом с программой (sage_editor.log "
               "или sage_player.log).";
    MessageBoxA(nullptr, message.c_str(), "SAGE — не удалось запуститься",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

void InstallEarlyCrashHandler(const char* appName) {
    if (CrashHandler::Installed()) return;   // слой уже поставил свой — не мешаем
    CrashHandler::Config cfg;
    cfg.AppName = appName ? appName : "SAGE";
    cfg.Version = kSageEngineVersion;
    // Аварийного сохранения здесь нет и быть не может: на этом этапе сцены ещё
    // не существует. Задача одна — чтобы падение ДО первого кадра оставило
    // отчёт, а не тишину.
    CrashHandler::Install(cfg);
}

} // namespace sage::detail
