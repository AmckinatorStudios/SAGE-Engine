#include "TestFramework.h"

// Единая точка входа: тесты регистрируются статически в test_*.cpp через TEST(),
// здесь только запускаем весь набор и возвращаем 0/1 (для CTest и CI).
int main() {
    return sagetest::RunAll();
}
