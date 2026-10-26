#pragma once
// ---------------------------------------------------------------------------
// Минимальный фреймворк модульных тестов SAGE — без внешних зависимостей.
// Тест регистрируется макросом TEST(имя) через статический инициализатор;
// main.cpp зовёт sagetest::RunAll(). Проверки (CHECK_*) не прерывают тест —
// они считают провалы, так что один тест сообщает про все несовпадения сразу.
// Специально лёгкий: тесты — быстрые, БЕЗ GL-контекста (математика, сериализация,
// анимация), в отличие от headless smoke-тестов (scripts/ci_smoke_test.sh),
// которым нужен рендер.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sagetest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

// Провалы ТЕКУЩЕГО теста (сбрасывается перед каждым тестом в RunAll).
inline int& CurrentFailures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const std::string& n, std::function<void()> fn) {
        Registry().push_back({n, std::move(fn)});
    }
};

// Все провалы прогона — чтобы повторить их в конце. Лог CI отдаёт хвост, а
// провал посреди тысячи тестов уходит выше него: итог «1 провалено» без
// имени теста чинить нечем.
inline std::vector<std::string>& FailLog() {
    static std::vector<std::string> v;
    return v;
}

inline void ReportFail(const char* file, int line, const std::string& msg) {
    ++CurrentFailures();
    std::printf("    [FAIL] %s:%d: %s\n", file, line, msg.c_str());
    FailLog().push_back(std::string(file) + ":" + std::to_string(line) + ": " + msg);
}

inline int RunAll() {
    int passed = 0, failed = 0;
    std::vector<std::string> failedNames;
    std::printf("=== sage_tests: %zu тестов ===\n", Registry().size());
    for (auto& tc : Registry()) {
        CurrentFailures() = 0;
        try {
            tc.fn();
        } catch (const std::exception& e) {
            ReportFail(__FILE__, __LINE__, std::string("исключение: ") + e.what());
        } catch (...) {
            ReportFail(__FILE__, __LINE__, "неизвестное исключение");
        }
        if (CurrentFailures() == 0) {
            std::printf("[ ok ] %s\n", tc.name.c_str());
            ++passed;
        } else {
            std::printf("[FAIL] %s (%d проверок провалено)\n", tc.name.c_str(), CurrentFailures());
            ++failed;
            failedNames.push_back(tc.name);
        }
    }
    if (failed) {
        std::printf("\n=== Провалы ===\n");
        for (const std::string& n : failedNames) std::printf("[FAIL] %s\n", n.c_str());
        for (const std::string& m : FailLog()) std::printf("    %s\n", m.c_str());
    }
    std::printf("\n=== Итог: %d прошло, %d провалено (всего %zu) ===\n",
                passed, failed, Registry().size());
    return failed == 0 ? 0 : 1;
}

} // namespace sagetest

// Регистрирует и определяет тело теста.
#define TEST(name)                                                        \
    static void name();                                                   \
    static ::sagetest::Registrar sagetest_reg_##name(#name, name);        \
    static void name()

#define CHECK_TRUE(cond)                                                  \
    do { if (!(cond)) ::sagetest::ReportFail(__FILE__, __LINE__,          \
        "CHECK_TRUE(" #cond ")"); } while (0)

#define CHECK_FALSE(cond)                                                 \
    do { if (cond) ::sagetest::ReportFail(__FILE__, __LINE__,             \
        "CHECK_FALSE(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                    \
    do { if (!((a) == (b))) ::sagetest::ReportFail(__FILE__, __LINE__,    \
        "CHECK_EQ(" #a ", " #b ")"); } while (0)

// Числовое сравнение с допуском (float/double).
#define CHECK_NEAR(a, b, eps)                                             \
    do { double _d = (double)(a) - (double)(b); if (_d < 0) _d = -_d;     \
         if (_d > (double)(eps)) ::sagetest::ReportFail(__FILE__, __LINE__, \
            std::string("CHECK_NEAR(" #a ", " #b ") diff=") +             \
            std::to_string(_d)); } while (0)
