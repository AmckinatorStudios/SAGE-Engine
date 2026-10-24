#pragma once
#include <mutex>
#include <string>

#include "../ConsoleLog.h"

// Панель Console — живой сток лога движка. Полностью самодостаточна: сама
// регистрирует Log-sink в Attach() и снимает его в Detach() (сток ссылается
// на this — снять обязательно ДО разрушения панели).
//
// Всё в ней — ради одного: чтобы ошибку было видно. Единственное
// предупреждение о непрочитанном шейдере тонуло среди тысяч строк загрузки, а
// именно оно и объясняло, почему сцена выглядит не так. Отсюда уровни со
// счётчиками, поиск и сворачивание повторов (правила — в ConsoleLog.h).
class ConsolePanel {
public:
    void Attach();
    void Detach();
    // Имя окна — от пространства (см. PanelWindowId.h).
    void Draw(bool* open, const std::string& windowId);

    // Сколько пришло за СЕССИЮ (не уменьшается при очистке): самопроверка
    // сравнивает «до» и «после» действия.
    int WarnCount() const { return m_warnTotal; }
    int ErrorCount() const { return m_errorTotal; }
    // Сколько их СЕЙЧАС в журнале — то, что видит человек (статусная строка):
    // очистил консоль — и счётчик в статусной строке тоже обнулился.
    int WarningsShown();
    int ErrorsShown();
    // Есть ли новые ошибки с прошлого взгляда (для подсветки вкладки).
    bool HasUnseenErrors() const { return m_errorTotal > m_seenErrors; }
    void MarkSeen() { m_seenErrors = m_errorTotal; }

private:
    void DrawToolbar();
    void DrawRows();
    void Clear();

    ConsoleLog m_log;
    ConsoleLog::Filter m_filter;
    std::mutex m_mutex;
    char m_search[128] = {0};

    bool m_autoScroll = true;
    bool m_collapse = true;   // сворачивать одинаковые подряд идущие строки
    bool m_showTime = true;
    int m_selected = -1;      // выделенная строка (номер в журнале)

    int m_warnTotal = 0;
    int m_errorTotal = 0;
    int m_seenErrors = 0;
};
