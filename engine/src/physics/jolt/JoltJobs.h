#pragma once
#include <memory>

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>

// Мост между планировщиком Jolt и единой системой задач движка.
//
// Раньше JoltWorld поднимал JPH::JobSystemThreadPool на «ядра − 1» потоков —
// второй пул рядом с пулом движка. Каждый шаг физики оба пула делили одни и те
// же ядра, ничего не зная друг о друге: потоков вдвое больше, чем ядер, и
// планировщик ОС переключает их посреди кадра. Теперь задачи Jolt — обычные
// кадровые задачи движка (полоса Worker, высокий приоритет, подсистема
// Physics): они видны в профилировщике, крадутся свободными воркерами и не
// плодят своих потоков.
namespace sage::physics {

// Создать планировщик Jolt поверх JobSystem движка. Без контекста движка
// (утилита, тест без EngineContext) задачи выполняет сам ждущий барьер на
// своём потоке — так же поступает JobSystemThreadPool без потоков.
std::unique_ptr<JPH::JobSystem> CreateJoltJobSystem(unsigned maxJobs, unsigned maxBarriers);

} // namespace sage::physics
