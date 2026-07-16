#pragma once
#include "Log.h"
#include "JobSystem.h"
#include "../asset/AssetManager.h"
#include <string>

// ---------------------------------------------------------------------
// EngineRuntime — формальное владение жизненным циклом движковых подсистем.
//
// Раньше порядок инициализации/остановки (Log::Init -> JobSystem::Start ->
// AssetManager::SetAssetRoot -> ... -> JobSystem::Shutdown ->
// AssetManager::Clear) держался только на комментариях в main.cpp. Нарушение
// порядка на выходе (например, AssetManager::Clear() до JobSystem::Shutdown())
// — use-after-free: фоновая задача обращается к уже очищенному кэшу ассетов.
//
// EngineRuntime — RAII-объект: конструктор поднимает подсистемы в правильном
// порядке, деструктор гасит их в ОБРАТНОМ порядке гарантированно (даже при
// исключении, размотавшем стек). Использование:
//
//   EngineRuntime engine(EngineConfig{"assets", hotReloadFlag});
//   ... engine.Shutdown() не нужен — вызовется в деструкторе при выходе
//       из scope main(), в правильном порядке ...
//
// ВАЖНОЕ ОГРАНИЧЕНИЕ (осознанно, не переобещаем): AssetManager и JobSystem
// остаются process-wide синглтонами. EngineRuntime формализует и защищает
// порядок их инициализации/остановки В РАМКАХ ОДНОГО ПРОЦЕССА, но не даёт
// полной поддержки нескольких независимых экземпляров движка в одном
// процессе (это отдельная, более глубокая работа — превращение синглтонов
// в member-объекты, передаваемые по ссылке всюду).
// ---------------------------------------------------------------------
struct EngineConfig {
    std::string AssetRoot = "assets";
    bool HotReload = false;
    unsigned int WorkerThreads = 0; // 0 — авто (см. JobSystem::Start)
    std::string LogFile = "sage_engine.log";
};

class EngineRuntime {
public:
    explicit EngineRuntime(const EngineConfig& config = {}) {
        Log::Init(config.LogFile);
        JobSystem::Instance().Start(config.WorkerThreads);

        AssetManager::Instance().SetAssetRoot(config.AssetRoot);
        if (config.HotReload) {
            AssetManager::Instance().EnableHotReload(true);
            LOG_INFO("Engine") << "Hot-reload ассетов включён";
        }
        LOG_INFO("Engine") << "EngineRuntime готов (asset root: " << config.AssetRoot << ")";
    }

    ~EngineRuntime() { Shutdown(); }

    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    // Гасит подсистемы в порядке, ОБРАТНОМ инициализации: сначала JobSystem
    // (join воркеров — чтобы ни одна фоновая задача не трогала ресурсы после
    // этой точки), затем AssetManager::Clear() (пока GL-контекст ещё жив).
    // Идемпотентно — безопасно звать явно (например, перед self-тестом,
    // который сам управляет своим выходом) и/или положиться на деструктор.
    void Shutdown() {
        if (m_shutdown) return;
        m_shutdown = true;
        JobSystem::Instance().Shutdown();
        AssetManager::Instance().Clear();
    }

private:
    bool m_shutdown = false;
};
