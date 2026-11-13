#include "sage/core/Cpu.h"

#include <cstring>
#include <stdexcept>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SAGE_CPU_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define SAGE_CPU_X86 0
#endif

namespace sage::cpu {

namespace {

#if SAGE_CPU_X86
void Cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
#if defined(_MSC_VER)
    int out[4];
    __cpuidex(out, (int)leaf, (int)sub);
    for (int i = 0; i < 4; ++i) r[i] = (unsigned)out[i];
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

// XCR0: сохраняет ли ОС регистры YMM. Без этого AVX у процессора есть, а
// пользоваться им нельзя. Байтами, а не _xgetbv: тому нужен флаг -mxsave,
// которого в базовой сборке нет.
unsigned long long Xcr0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax = 0, edx = 0;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | eax;
#endif
}
#endif

Features Probe() {
    Features f;
    f.LogicalCores = (int)std::thread::hardware_concurrency();
#if SAGE_CPU_X86
    f.X86 = true;
    unsigned r[4] = {0, 0, 0, 0};
    Cpuid(0, 0, r);
    const unsigned maxLeaf = r[0];
    Cpuid(0x80000000u, 0, r);
    const unsigned maxExt = r[0];
    if (maxExt >= 0x80000004u) {
        char brand[49] = {0};
        for (unsigned i = 0; i < 3; ++i) {
            Cpuid(0x80000002u + i, 0, r);
            std::memcpy(brand + i * 16, r, 16);
        }
        // Имя приходит с пробелами спереди и подряд — в журнале они ни к чему.
        std::string s;
        bool space = true;
        for (const char* p = brand; *p; ++p) {
            if (*p == ' ') {
                if (!space) s += ' ';
                space = true;
            } else {
                s += *p;
                space = false;
            }
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        f.Brand = s;
    }
    bool osYmm = false;
    if (maxLeaf >= 1) {
        Cpuid(1, 0, r);
        f.Sse42 = (r[2] >> 20) & 1;
        f.Popcnt = (r[2] >> 23) & 1;
        f.F16c = (r[2] >> 29) & 1;
        f.Fma = (r[2] >> 12) & 1;
        const bool osxsave = (r[2] >> 27) & 1;
        const bool avx = (r[2] >> 28) & 1;
        osYmm = osxsave && (Xcr0() & 0x6) == 0x6;
        f.Avx = avx && osYmm;
        f.Fma = f.Fma && osYmm;
        f.F16c = f.F16c && osYmm;
    }
    if (maxLeaf >= 7) {
        Cpuid(7, 0, r);
        f.Bmi1 = (r[1] >> 3) & 1;
        f.Avx2 = ((r[1] >> 5) & 1) && osYmm;
        f.Bmi2 = (r[1] >> 8) & 1;
    }
    if (maxExt >= 0x80000001u) {
        Cpuid(0x80000001u, 0, r);
        f.Lzcnt = (r[2] >> 5) & 1;
    }
#endif
    if (f.Brand.empty()) f.Brand = f.X86 ? "x86-64" : "не x86-64";
    return f;
}

} // namespace

const Features& Detect() {
    static const Features f = Probe();
    return f;
}

Baseline BuildBaseline() {
    Baseline b;
#if defined(__SSE4_2__)
    b.Sse42 = true;
#endif
#if defined(__POPCNT__)
    b.Popcnt = true;
#endif
#if defined(__AVX__)
    b.Avx = true;
#endif
#if defined(__AVX2__)
    b.Avx2 = true;
#endif
#if defined(__FMA__)
    b.Fma = true;
#endif
#if defined(__BMI__)
    b.Bmi1 = true;
#endif
#if defined(__BMI2__)
    b.Bmi2 = true;
#endif
#if defined(__LZCNT__)
    b.Lzcnt = true;
#endif
#if defined(__F16C__)
    b.F16c = true;
#endif
    return b;
}

std::string MissingForBuild(const Features& cpu, const Baseline& build) {
    if (!cpu.X86) return {};
    std::string missing;
    auto need = [&](bool required, bool has, const char* name) {
        if (required && !has) missing += (missing.empty() ? "" : ", ") + std::string(name);
    };
    need(build.Sse42, cpu.Sse42, "SSE4.2");
    need(build.Popcnt, cpu.Popcnt, "POPCNT");
    need(build.Avx, cpu.Avx, "AVX");
    need(build.Avx2, cpu.Avx2, "AVX2");
    need(build.Fma, cpu.Fma, "FMA");
    need(build.Bmi1, cpu.Bmi1, "BMI1");
    need(build.Bmi2, cpu.Bmi2, "BMI2");
    need(build.Lzcnt, cpu.Lzcnt, "LZCNT");
    need(build.F16c, cpu.F16c, "F16C");
    return missing;
}

std::string Describe(const Features& cpu) {
    std::string s = cpu.Brand + ", потоков " + std::to_string(cpu.LogicalCores);
    if (!cpu.X86) return s;
    s += "; инструкции:";
    auto put = [&](bool has, const char* name) { s += std::string(" ") + name + (has ? "+" : "−"); };
    put(cpu.Sse42, "SSE4.2");
    put(cpu.Avx, "AVX");
    put(cpu.Avx2, "AVX2");
    put(cpu.Fma, "FMA");
    put(cpu.Bmi1, "BMI1");
    put(cpu.Lzcnt, "LZCNT");
    return s;
}

void RequireBaseline() {
    const std::string missing = MissingForBuild(Detect(), BuildBaseline());
    if (missing.empty()) return;
    throw std::runtime_error("Процессор «" + Detect().Brand + "» не поддерживает инструкции, нужные этой сборке: " +
                             missing + ". Нужен процессор с SSE4.2 (Intel Core i 2008 года и новее, AMD FX и новее).");
}

} // namespace sage::cpu
