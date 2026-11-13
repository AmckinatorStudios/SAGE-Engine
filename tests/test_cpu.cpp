#include "TestFramework.h"

#include "sage/core/Cpu.h"

// Сборка рассчитана на старые процессоры: не новее SSE4.2 + POPCNT. Любой
// флаг новее (AVX, BMI1, LZCNT — прежде приходили от физики) — и на
// процессоре 2011–2012 годов игра падает или считает неверно.
TEST(Cpu_build_targets_old_processors) {
    const sage::cpu::Baseline b = sage::cpu::BuildBaseline();
    CHECK_FALSE(b.Avx);
    CHECK_FALSE(b.Avx2);
    CHECK_FALSE(b.Fma);
    CHECK_FALSE(b.Bmi1);
    CHECK_FALSE(b.Bmi2);
    CHECK_FALSE(b.Lzcnt);
    CHECK_FALSE(b.F16c);
}

// Процессор с AVX, но без BMI1/LZCNT/AVX2 (Sandy Bridge) этой сборке подходит;
// без SSE4.2 — нет, и сообщение называет, чего не хватает.
TEST(Cpu_missing_features_are_named) {
    sage::cpu::Features sandy;
    sandy.X86 = true;
    sandy.Sse42 = sandy.Popcnt = sandy.Avx = true;
    CHECK_TRUE(sage::cpu::MissingForBuild(sandy, sage::cpu::BuildBaseline()).empty());

    sage::cpu::Baseline modern;
    modern.Sse42 = modern.Avx2 = modern.Bmi1 = true;
    const std::string missing = sage::cpu::MissingForBuild(sandy, modern);
    CHECK_TRUE(missing.find("AVX2") != std::string::npos);
    CHECK_TRUE(missing.find("BMI1") != std::string::npos);
    CHECK_TRUE(missing.find("SSE4.2") == std::string::npos);

    sage::cpu::Features old;
    old.X86 = true;
    sage::cpu::Baseline sse;
    sse.Sse42 = true;
    CHECK_TRUE(sage::cpu::MissingForBuild(old, sse) == "SSE4.2");
    // Не x86 (Apple Silicon): флаги x86 к нему не относятся.
    sage::cpu::Features arm;
    CHECK_TRUE(sage::cpu::MissingForBuild(arm, modern).empty());
}

// Определение на этой машине не падает и даёт осмысленную строку.
TEST(Cpu_detects_this_machine) {
    const sage::cpu::Features& f = sage::cpu::Detect();
    CHECK_TRUE(f.LogicalCores >= 1);
    CHECK_FALSE(f.Brand.empty());
    CHECK_TRUE(sage::cpu::MissingForBuild(f, sage::cpu::BuildBaseline()).empty());
    CHECK_FALSE(sage::cpu::Describe(f).empty());
}
