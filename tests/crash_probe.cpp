// Отдельная программа, которая ПАДАЕТ НАРОЧНО.
//
// Проверить обработчик падений внутри обычного теста невозможно: он по замыслу
// доводит падение до конца и убивает процесс, а вместе с ним и весь прогон.
// Поэтому проверка вынесена в отдельный исполняемый файл: smoke-тест запускает
// его, ждёт падения и смотрит, появился ли отчёт и есть ли в нём то, ради чего
// он писался.
//
// Аргумент выбирает вид падения — их два, и они ловятся РАЗНЫМИ механизмами:
//   segv      — обращение по неверному адресу (сигнал);
//   terminate — необработанное исключение (std::terminate).
// Ловится только один из них — и половина отказов проходит мимо.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "sage/core/CrashHandler.h"
#include "sage/core/Log.h"

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "segv";
    const std::string dir = argc > 2 ? argv[2] : ".";

    sage::CrashHandler::Config cfg;
    cfg.AppName = "SAGE CrashProbe";
    cfg.Version = "test";
    cfg.ReportDir = dir;
    cfg.Context = [] { return std::string("МАРКЕР-КОНТЕКСТА"); };
    cfg.EmergencySave = []() -> std::string {
        // Ровно то, что делает редактор: сбросить работу в отдельный файл.
        FILE* f = std::fopen("crashprobe-recovered.txt", "w");
        if (!f) return {};
        std::fputs("сцена спасена\n", f);
        std::fclose(f);
        return "crashprobe-recovered.txt";
    };
    sage::CrashHandler::Install(cfg);

    LOG_INFO("Probe") << "МАРКЕР-ЛОГА перед падением";
    std::fflush(stdout);

    if (mode == "terminate") {
        throw std::runtime_error("МАРКЕР-ИСКЛЮЧЕНИЯ");
    }
    // Обращение по заведомо неверному адресу. volatile — чтобы оптимизатор не
    // выбросил запись как «неопределённое поведение, значит можно всё».
    volatile int* p = reinterpret_cast<int*>(1);
    *p = 42;
    return 0;
}
