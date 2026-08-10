// ---------------------------------------------------------------------------
//  sage_bench — сколько стоят горячие места движка.
//
//  ЗАЧЕМ ОТДЕЛЬНАЯ ПРОГРАММА, А НЕ ТЕСТ. Тест отвечает «работает/нет», и
//  добавить в него измерение легко — но тогда набор тестов начинает падать от
//  загрузки машины сборщика, и ему перестают верить. Здесь наоборот: программа
//  ничего не утверждает, она печатает числа. Утверждение делает человек,
//  сравнивая два запуска.
//
//  ЧТО МЕРЯЕТСЯ. Ровно то, что происходит КАЖДЫЙ КАДР и растёт вместе со
//  сценой: мировые матрицы иерархии, отсечение по пирамиде, сбор освещения,
//  раскладка интерфейса, шаг физики, вызов скриптов. Загрузку моделей и
//  компиляцию шейдеров не меряем — они случаются один раз, и оптимизировать их
//  за счёт читаемости невыгодно.
//
//  КАК МЕРЯЕТСЯ. Медиана из нескольких прогонов, а не среднее: одиночный
//  выброс от планировщика ОС сдвигает среднее на десятки процентов, а медиану
//  не двигает вовсе. Рядом печатается разброс — по нему видно, можно ли верить
//  разнице между двумя запусками. Разница меньше разброса — это шум, а не
//  ускорение, и объявлять её улучшением нельзя.
//
//  ПРО ЭТУ МАШИНУ. Абсолютные числа зависят от неё целиком. Сравнивать имеет
//  смысл ДВА ЗАПУСКА НА ОДНОЙ машине — до правки и после; переносить цифры с
//  машины на машину бессмысленно.
//
//  Запуск:  ./sage_bench            все замеры
//           ./sage_bench culling     только те, в чьём имени есть «culling»
// ---------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Cases.h"

namespace sage::bench {
namespace {

std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

} // namespace

void Register(const char* name, const char* unit, long long items, Body body) {
    Registry().push_back(Case{name, unit, items, std::move(body)});
}

namespace {

struct Result {
    double MedianMs = 0.0;
    double MinMs = 0.0;
    double SpreadPct = 0.0;   // (max-min)/median: можно ли верить разнице
};

// Прогоняем тело несколько раз и берём медиану. Перед замером — один холостой
// прогон: он греет кэши и аллокатор, и без него первый результат всегда хуже
// остальных, из-за чего «первое измерение» врало бы в любую сторону.
Result Measure(const Case& c, int runs) {
    c.Run();   // прогрев

    std::vector<double> samples;
    samples.reserve(runs);
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        c.Run();
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());

    Result r;
    r.MedianMs = samples[samples.size() / 2];
    r.MinMs = samples.front();
    r.SpreadPct = r.MedianMs > 0.0 ? (samples.back() - samples.front()) / r.MedianMs * 100.0 : 0.0;
    return r;
}

// Ширина строки В СИМВОЛАХ, а не в байтах. printf считает байты, кириллица в
// UTF-8 занимает два, и «%-42s» разъезжает колонки ровно на длину русских слов
// — таблица становится нечитаемой именно там, где имён больше всего.
int DisplayWidth(const char* s) {
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;   // продолжения UTF-8 не считаем
    return n;
}

void Pad(const char* s, int width, bool left) {
    const int gap = std::max(0, width - DisplayWidth(s));
    if (!left) for (int i = 0; i < gap; ++i) std::putchar(' ');
    std::fputs(s, stdout);
    if (left) for (int i = 0; i < gap; ++i) std::putchar(' ');
}

void PrintRow(const char* name, const char* median, const char* best, const char* spread,
              const char* per) {
    Pad(name, 44, true);
    Pad(median, 12, false);
    Pad(best, 12, false);
    Pad(spread, 9, false);
    std::fputs("   ", stdout);
    std::fputs(per, stdout);
    std::putchar('\n');
}

} // namespace
} // namespace sage::bench

int main(int argc, char** argv) {
    using namespace sage::bench;

    const char* filter = argc > 1 ? argv[1] : nullptr;
    int runs = 7;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--runs=", 7) == 0) {
            runs = std::max(3, std::atoi(argv[i] + 7));
            if (argv[i] == filter) filter = nullptr;
        }
    }

    RegisterAllCases();

    std::printf("SAGE Engine — замеры горячих мест\n");
    std::printf("=================================\n");
    std::printf("прогонов на замер: %d (берётся медиана)\n\n", runs);
    PrintRow("замер", "медиана", "лучший", "разброс", "на единицу");
    PrintRow("-----", "-------", "------", "-------", "----------");

    int shown = 0;
    for (const Case& c : Registry()) {
        if (filter && std::string(c.Name).find(filter) == std::string::npos) continue;
        const Result r = Measure(c, runs);
        ++shown;

        // Стоимость единицы — то, ради чего замер и делается: «12 мс» ничего не
        // говорит, а «84 нс на сущность» сравнимо и с прошлым запуском, и со
        // здравым смыслом.
        char per[64] = "";
        if (c.Items > 0) {
            const double ns = r.MedianMs * 1e6 / (double)c.Items;
            std::snprintf(per, sizeof(per), "%.1f нс / %s", ns, c.Unit);
        }
        char median[32], best[32], spread[32];
        std::snprintf(median, sizeof(median), "%.3f мс", r.MedianMs);
        std::snprintf(best, sizeof(best), "%.3f мс", r.MinMs);
        std::snprintf(spread, sizeof(spread), "%.1f%%", r.SpreadPct);
        PrintRow(c.Name, median, best, spread, per);
    }

    if (shown == 0) {
        std::printf("\nни один замер не подошёл под «%s»\n", filter ? filter : "");
        return 1;
    }
    std::printf("\nРазница между двумя запусками меньше разброса — это шум, а не ускорение.\n");
    return 0;
}
