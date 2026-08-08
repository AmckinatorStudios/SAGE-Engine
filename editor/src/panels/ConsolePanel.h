#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "sage/core/Log.h"

// Панель Console — живой сток лога движка. Полностью самодостаточна: сама
// регистрирует Log-sink в Attach() и снимает его в Detach() (сток ссылается
// на this — снять обязательно ДО разрушения панели).
//
// Раньше панель была списком строк без фильтров, и в этом была практическая
// беда: единственное предупреждение о непрочитанном шейдере тонуло среди
// нескольких тысяч сообщений загрузки, а именно оно и объясняло, почему сцена
// выглядит не так. Поэтому здесь фильтры по уровню, поиск, счётчики и
// сворачивание повторов — всё ради одного: чтобы ошибку было видно.
class ConsolePanel {
public:
    void Attach();
    void Detach();
    void Draw(bool* open);

    // Сколько было предупреждений и ошибок за сессию — редактор показывает это
    // в статусной строке, чтобы не приходилось открывать панель ради проверки,
    // всё ли в порядке.
    int WarnCount() const { return m_warnCount; }
    int ErrorCount() const { return m_errorCount; }
    // Есть ли новые ошибки с прошлого взгляда (для подсветки вкладки).
    bool HasUnseenErrors() const { return m_errorCount > m_seenErrors; }
    void MarkSeen() { m_seenErrors = m_errorCount; }

private:
    struct Entry {
        LogLevel Level;
        std::string Category;
        std::string Message;
        int Repeats = 1;   // сколько раз подряд повторилось одно и то же
    };

    bool Passes(const Entry& e) const;

    std::vector<Entry> m_entries;
    std::mutex m_mutex;
    bool m_autoScroll = true;
    // Фильтры по уровню. Debug выключен по умолчанию: он полезен точечно, а
    // включённым всегда просто вытесняет из окна всё остальное.
    bool m_showDebug = false;
    bool m_showInfo = true;
    bool m_showWarn = true;
    bool m_showError = true;
    bool m_collapse = true;    // сворачивать одинаковые подряд идущие строки
    char m_filter[128] = {0};

    int m_warnCount = 0;
    int m_errorCount = 0;
    int m_seenErrors = 0;
};
