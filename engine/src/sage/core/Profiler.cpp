#include "sage/core/Profiler.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "sage/rhi/GraphicsDevice.h"

namespace sage::profile {
namespace {

using clock = std::chrono::steady_clock;

// Сколько кадров ждать готовности меток GPU. Два мало на конвейере с глубокой
// очередью, четыре — уже заметная задержка показаний. Три — компромисс,
// проверенный тем, что метки в этот срок успевают созреть на llvmpipe (самый
// медленный из наших случаев).
constexpr int kFramesInFlight = 3;
// Сколько кадров усреднять. Покадровые значения скачут на десятки процентов
// от планировщика ОС, и смотреть на них бессмысленно.
constexpr int kAverageWindow = 30;

struct Sample {
    std::string Name;
    int Depth = 0;
    clock::time_point CpuBegin;
    double CpuMs = 0.0;
    sage::rhi::QueryHandle GpuBegin; // хендлы меток; невалидные — таймеров нет
    sage::rhi::QueryHandle GpuEnd;
};

struct FrameSlot {
    std::vector<Sample> Samples;
    bool Pending = false; // ждём созревания меток GPU

    // Пул меток У КАЖДОГО КАДРА СВОЙ, и это не мелочь. Общий пул на все кадры
    // в полёте выглядит экономнее, но ломает саму идею отложенного чтения:
    // следующий кадр перезаписывает те самые метки, готовности которых мы
    // ждём, и glQueryCounter сбрасывает их «готовность» обратно в false. На
    // медленной отрисовке метка не успевает созреть НИКОГДА — результат
    // всегда пустой, а времена GPU показываются нулями. Ровно это и было.
    std::vector<sage::rhi::QueryHandle> Pool;
    size_t PoolUsed = 0;
};

struct State {
    bool Enabled = false;
    bool GpuTimers = false;
    int FrameIndex = 0;

    FrameSlot Slots[kFramesInFlight];
    std::vector<int> Stack;      // индексы открытых участков текущего кадра
    std::vector<Entry> Ready;    // последний кадр с готовыми метками
    std::vector<Entry> Averaged;
    std::vector<std::vector<Entry>> History;

    double CpuMs = 0.0;
    double GpuMs = 0.0;
    clock::time_point FrameBegin;

    // Сколько кадров подряд метки GPU не созрели вовремя. Нужен, чтобы
    // профилировщик не молчал вечно: раньше при неготовности мы просто
    // пропускали кадр, и на драйвере, который стабильно опаздывает, панель
    // оставалась пустой навсегда — без единого намёка на причину.
    int LateFrames = 0;
};

// Через сколько подряд опозданий признать, что таймеров GPU у нас нет.
// Разово опоздать метка может (кадр вышел тяжёлым), но десять раз подряд —
// это не случайность, а свойство драйвера.
constexpr int kLateFramesGiveUp = 10;

State& Get() {
    static State state;
    return state;
}

// Метка из пула ТЕКУЩЕГО кадра. Пул растёт до числа участков и дальше просто
// переиспользуется: создавать и удалять запросы каждый кадр дороже.
sage::rhi::QueryHandle TakeQuery(FrameSlot& slot) {
    State& s = Get();
    if (!s.GpuTimers) return {};
    if (slot.PoolUsed == slot.Pool.size()) {
        slot.Pool.push_back(sage::rhi::GraphicsDevice::Get().CreateTimestampQuery());
    }
    return slot.Pool[slot.PoolUsed++];
}

// Забирает результаты кадра, если метки созрели. false — ещё не готовы.
bool Collect(FrameSlot& slot, std::vector<Entry>& out, double& gpuTotal) {
    State& s = Get();
    if (slot.Samples.empty()) return false;

    if (s.GpuTimers) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        for (const Sample& sample : slot.Samples) {
            if (sample.GpuEnd.Valid() && !device.TimestampReady(sample.GpuEnd)) return false;
        }
    }

    out.clear();
    gpuTotal = 0.0;
    sage::rhi::GraphicsDevice* device =
        s.GpuTimers ? &sage::rhi::GraphicsDevice::Get() : nullptr;
    for (const Sample& sample : slot.Samples) {
        Entry e;
        e.Name = sample.Name;
        e.Depth = sample.Depth;
        e.CpuMs = sample.CpuMs;
        e.Calls = 1;
        if (device && sample.GpuBegin.Valid() && sample.GpuEnd.Valid()) {
            const unsigned long long begin = device->TimestampNs(sample.GpuBegin);
            const unsigned long long end = device->TimestampNs(sample.GpuEnd);
            e.GpuMs = end > begin ? (double)(end - begin) / 1e6 : 0.0;
        }
        // В сумму кадра идут только участки ВЕРХНЕГО уровня: вложенные уже
        // посчитаны внутри родителя, и складывать их значило бы считать одно
        // и то же время дважды.
        if (e.Depth == 0) gpuTotal += e.GpuMs;
        out.push_back(std::move(e));
    }
    return true;
}

void UpdateAverage() {
    State& s = Get();
    s.History.push_back(s.Ready);
    if ((int)s.History.size() > kAverageWindow) s.History.erase(s.History.begin());

    s.Averaged.clear();
    if (s.History.empty()) return;
    // Усредняем по ИМЕНИ, а не по позиции: состав проходов меняется (эффект
    // включили, тени выключили), и усреднение «третьей строки с третьей»
    // складывало бы разные вещи.
    for (const std::vector<Entry>& frame : s.History) {
        for (const Entry& e : frame) {
            auto it = std::find_if(s.Averaged.begin(), s.Averaged.end(),
                                   [&](const Entry& a) { return a.Name == e.Name; });
            if (it == s.Averaged.end()) {
                Entry sum = e;
                sum.Calls = 1;
                s.Averaged.push_back(std::move(sum));
            } else {
                it->CpuMs += e.CpuMs;
                it->GpuMs += e.GpuMs;
                ++it->Calls;
            }
        }
    }
    for (Entry& e : s.Averaged) {
        if (e.Calls > 0) {
            e.CpuMs /= e.Calls;
            e.GpuMs /= e.Calls;
        }
    }
}

} // namespace

void SetEnabled(bool enabled) {
    State& s = Get();
    if (s.Enabled == enabled) return;
    s.Enabled = enabled;
    if (enabled) {
        s.GpuTimers = sage::rhi::GraphicsDevice::Get().SupportsGpuTimers();
    }
}

bool Enabled() { return Get().Enabled; }
bool GpuTimersAvailable() { return Get().GpuTimers; }

void BeginFrame() {
    State& s = Get();
    if (!s.Enabled) return;
    s.FrameBegin = clock::now();
    s.Stack.clear();

    FrameSlot& slot = s.Slots[s.FrameIndex % kFramesInFlight];
    slot.Samples.clear();
    slot.PoolUsed = 0;
    slot.Pending = true;
}

void Push(const char* name) {
    State& s = Get();
    if (!s.Enabled) return;
    FrameSlot& slot = s.Slots[s.FrameIndex % kFramesInFlight];

    Sample sample;
    sample.Name = name ? name : "?";
    sample.Depth = (int)s.Stack.size();
    sample.CpuBegin = clock::now();
    sample.GpuBegin = TakeQuery(slot);
    if (sample.GpuBegin.Valid()) sage::rhi::GraphicsDevice::Get().WriteTimestamp(sample.GpuBegin);

    slot.Samples.push_back(std::move(sample));
    s.Stack.push_back((int)slot.Samples.size() - 1);
}

void Pop() {
    State& s = Get();
    if (!s.Enabled || s.Stack.empty()) return;
    FrameSlot& slot = s.Slots[s.FrameIndex % kFramesInFlight];

    Sample& sample = slot.Samples[(size_t)s.Stack.back()];
    s.Stack.pop_back();
    sample.CpuMs = std::chrono::duration<double, std::milli>(clock::now() - sample.CpuBegin).count();
    sample.GpuEnd = TakeQuery(slot);
    if (sample.GpuEnd.Valid()) sage::rhi::GraphicsDevice::Get().WriteTimestamp(sample.GpuEnd);
}

void EndFrame() {
    State& s = Get();
    if (!s.Enabled) return;
    s.CpuMs = std::chrono::duration<double, std::milli>(clock::now() - s.FrameBegin).count();
    ++s.FrameIndex;

    // Забираем САМЫЙ СТАРЫЙ кадр из очереди: у него метки уже созрели, и
    // ожидания не будет.
    FrameSlot& oldest = s.Slots[s.FrameIndex % kFramesInFlight];
    if (oldest.Pending) {
        double gpu = 0.0;
        if (Collect(oldest, s.Ready, gpu)) {
            s.GpuMs = gpu;
            s.LateFrames = 0;
            oldest.Pending = false;
            UpdateAverage();
        } else if (++s.LateFrames >= kLateFramesGiveUp) {
            // Сдаёмся по части GPU и продолжаем показывать время CPU: пустая
            // панель — худший из возможных ответов на вопрос «где тормозит».
            s.GpuTimers = false;
            s.GpuMs = 0.0;
            s.LateFrames = 0;
        }
    }
}

const std::vector<Entry>& Frame() { return Get().Ready; }
const std::vector<Entry>& Average() { return Get().Averaged; }
double FrameCpuMs() { return Get().CpuMs; }
double FrameGpuMs() { return Get().GpuMs; }

std::string Summary() {
    State& s = Get();
    if (!s.Enabled) return "профилировщик выключен";
    char buffer[128];
    if (s.GpuTimers) {
        std::snprintf(buffer, sizeof(buffer), "кадр: CPU %.2f мс, GPU %.2f мс", s.CpuMs, s.GpuMs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "кадр: CPU %.2f мс (таймеров GPU нет)", s.CpuMs);
    }
    return buffer;
}


size_t ResidentBytes() {
#if defined(_WIN32)
    // K32GetProcessMemoryInfo живёт в kernel32 (с Windows 7), а не в psapi.dll:
    // psapi пришлось бы добавлять в список разрешённых библиотек поставки, и
    // ради одного числа в статусной строке это лишнее.
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::K32GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (size_t)pmc.WorkingSetSize;
    return 0;
#elif defined(__linux__)
    // statm: вторая цифра — резидентные СТРАНИЦЫ. Не /proc/self/status: там то
    // же число, но его пришлось бы искать разбором строк.
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long long total = 0, resident = 0;
    const int got = std::fscanf(f, "%lld %lld", &total, &resident);
    std::fclose(f);
    if (got != 2 || resident <= 0) return 0;
    return (size_t)resident * (size_t)sysconf(_SC_PAGESIZE);
#else
    return 0;
#endif
}

} // namespace sage::profile
