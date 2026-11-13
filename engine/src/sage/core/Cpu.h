#pragma once
#include <string>

// ---------------------------------------------------------------------------
// ПРОЦЕССОР: что он умеет и на что рассчитана сборка.
//
// ЗАЧЕМ. Движок собирается под БАЗОВЫЙ набор инструкций x86-64 — SSE4.2 и
// POPCNT (любой процессор с 2008–2011 года: Core i первого поколения,
// AMD FX, всё, где есть AVX). Ничего новее в сборку попадать не должно:
// прежде в неё через флаги физики попадали LZCNT и BMI1, и на процессорах с
// AVX, но без BMI1 (Sandy Bridge, Ivy Bridge, ранние AMD FX) игра падала с
// «недопустимой инструкцией» или считала неверно (LZCNT там исполняется как
// BSR — без ошибки, но с другим ответом).
//
// Если процессор слабее базового набора, запуск останавливается с понятным
// сообщением, а не падением. Набор, под который СОБРАНО, проверяет модульный
// тест (BuildBaseline) — случайный -march в чужом CMake его провалит.
// ---------------------------------------------------------------------------
namespace sage::cpu {

struct Features {
    std::string Brand;          // «Intel(R) Core(TM) i5-2500K CPU @ 3.30GHz»
    int LogicalCores = 0;
    bool X86 = false;           // false — не x86-64 (Apple Silicon): флаги ниже не о нём
    bool Sse42 = false;
    bool Popcnt = false;
    bool Avx = false;           // и процессор, и ОС (сохранение регистров YMM)
    bool Avx2 = false;
    bool Fma = false;
    bool Bmi1 = false;
    bool Bmi2 = false;
    bool Lzcnt = false;
    bool F16c = false;
};

// Что умеет процессор этой машины (считается один раз).
const Features& Detect();

// Чего сборка требует сверх простого x86-64 — по макросам компилятора, то
// есть по флагам, с которыми собран ЭТОТ файл (а они общие у всего движка).
struct Baseline {
    bool Sse42 = false, Popcnt = false, Avx = false, Avx2 = false, Fma = false;
    bool Bmi1 = false, Bmi2 = false, Lzcnt = false, F16c = false;
};
Baseline BuildBaseline();

// Пусто — процессор подходит. Иначе — чего не хватает, словами для человека.
std::string MissingForBuild(const Features& cpu, const Baseline& build);

// Строка для журнала: имя, ядра, набор инструкций.
std::string Describe(const Features& cpu);

// Останавливает запуск исключением, если процессор слабее сборки.
void RequireBaseline();

} // namespace sage::cpu
