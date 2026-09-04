#include "sage/core/GameModule.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sage/core/CrashHandler.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace sage::detail {

#ifdef _WIN32
namespace {

// UTF-8 -> UTF-16. Весь текст в движке — UTF-8, а Windows принимает его только
// широкими функциями: MessageBoxA трактует байты как ANSI-кодировку системы, и
// русское сообщение превращается в «SAGE вЂ" PSPµ CѓPrP°P»PsCЃC'Њ» — то есть
// человек видит окно, но прочесть в нём ничего не может. Ради одного этого и
// заведено преобразование.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), need);
    return out;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out((size_t)need - 1, '\0');   // -1: без завершающего нуля
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

} // namespace
#endif

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
    //
    // MessageBoxW, а не MessageBoxA: узкая версия читает байты в ANSI-кодировке
    // системы, а у нас UTF-8 — и всё сообщение выходило нечитаемой кашей.
    // Окно с ошибкой, которую нельзя прочесть, немногим лучше отсутствия окна.
    const std::string message =
        text + "\n\nПодробности записаны в лог рядом с программой (sage_editor.log "
               "или sage_player.log).";
    const std::wstring wideMessage = Utf8ToWide(message);
    const std::wstring wideTitle = Utf8ToWide("SAGE — не удалось запуститься");
    ::MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(),
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

void NormalizeArgs(int& argc, char**& argv) {
#ifdef _WIN32
    // Настоящая командная строка процесса — широкая; узкий argv, который CRT
    // отдаёт main, получен из неё перекодировкой в ANSI. Путь с кириллицей там
    // либо испорчен, либо (в UTF-8-сборке) просто не является корректным UTF-8,
    // и первый же std::filesystem::path из такого аргумента бросает исключение.
    // Поэтому argv собирается заново из первоисточника.
    static bool done = false;
    if (done) return;   // зовётся один раз из main; второй вызов — точно ошибка
    done = true;

    int wideCount = 0;
    wchar_t** wide = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
    if (!wide || wideCount <= 0) return;   // не получилось — оставляем как было

    // Память живёт до конца процесса намеренно: argv читают когда угодно, в том
    // числе слои после запуска. Освобождать её было бы нечем и незачем.
    static std::vector<std::string> storage;
    static std::vector<char*> pointers;
    storage.reserve((size_t)wideCount);
    for (int i = 0; i < wideCount; ++i) storage.push_back(WideToUtf8(wide[i]));
    ::LocalFree(wide);
    pointers.reserve(storage.size() + 1);
    for (std::string& s : storage) pointers.push_back(s.data());
    pointers.push_back(nullptr);

    argc = wideCount;
    argv = pointers.data();

    // Заодно консоль: если программу запустили из терминала (или в CI), вывод
    // stderr тоже UTF-8, и без этой строки он читается так же плохо, как читался
    // MessageBoxA.
    ::SetConsoleOutputCP(CP_UTF8);
#else
    (void)argc;
    (void)argv;   // на Unix argv — байты, перекодировать нечего
#endif
}

} // namespace sage::detail
