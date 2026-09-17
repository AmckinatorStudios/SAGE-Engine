#include "EditorRecovery.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

void EditorRecovery::ScanOnStartup() {
    // --- Отчёт о падении ----------------------------------------------------
    std::error_code ec;
    fs::path newest;
    fs::file_time_type newestTime{};
    m_crashCount = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(".", ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = sage::PathToUtf8(e.path().filename());
        if (name.rfind("sage-crash-", 0) != 0 || e.path().extension() != ".txt") continue;
        ++m_crashCount;
        const fs::file_time_type t = fs::last_write_time(e.path(), ec);
        if (newest.empty() || t > newestTime) {
            newest = e.path();
            newestTime = t;
        }
    }
    if (!newest.empty()) {
        std::ifstream in(newest, std::ios::binary);
        if (in.is_open()) {
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            // Отчёт короткий по устройству, но файл на диске мог оказаться каким
            // угодно: читаем с ограничением, чтобы случайный чужой
            // sage-crash-*.txt на гигабайт не занял память.
            if (text.size() > 512 * 1024) text.resize(512 * 1024);
            m_crashPath = sage::PathToUtf8(newest);
            m_crashText = std::move(text);
            m_crashPrompt = true;
            LOG_WARN("Editor") << "Прошлый запуск завершился аварийно, отчёт: " << m_crashPath;
        }
    }

    // --- Файл восстановления ------------------------------------------------
    //
    // Аварийное сохранение идёт первым: оно записано В МОМЕНТ падения, а
    // автосохранение — до минуты назад. Разница в минуту работы и есть то, ради
    // чего восстановление существует.
    for (const char* candidate : {kEmergencyFile, kAutosaveFile}) {
        if (fs::exists(candidate, ec)) {
            m_recoveryFile = candidate;
            m_recoveryPrompt = true;
            LOG_WARN("Editor") << "Найден файл восстановления: " << candidate;
            break;
        }
    }
}

void EditorRecovery::Tick(float dt, bool dirty, bool inPlayMode,
                          const std::function<bool(const std::string&)>& save) {
    if (m_interval <= 0.0f || !dirty || inPlayMode) return;
    m_timer += dt;
    if (m_timer < m_interval) return;
    m_timer = 0.0f;
    if (save(kAutosaveFile)) {
        m_last = kAutosaveFile;
        LOG_DEBUG("Editor") << "Автосохранение: " << kAutosaveFile;
    } else {
        // Не сумели — не беда для кадра, но сказать надо: молчащее
        // автосохранение хуже отсутствующего, на него рассчитывают.
        LOG_WARN("Editor") << "Автосохранение не удалось — выключено до конца сеанса";
        m_interval = 0.0f; // не долбить диск каждую минуту без надежды
    }
}

void EditorRecovery::DeleteRecoveryFile() {
    std::error_code ec;
    if (!m_recoveryFile.empty()) fs::remove(sage::PathFromUtf8(m_recoveryFile), ec);
    m_recoveryPrompt = false;
}

void EditorRecovery::DeleteCrashReport() {
    std::error_code ec;
    if (!m_crashPath.empty()) fs::remove(sage::PathFromUtf8(m_crashPath), ec);
    m_crashPrompt = false;
}
