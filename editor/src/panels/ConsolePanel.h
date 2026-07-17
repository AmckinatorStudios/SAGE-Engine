#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "sage/core/Log.h"

// Панель Console — живой сток лога движка. Полностью самодостаточна: сама
// регистрирует Log-sink в Attach() и снимает его в Detach() (сток ссылается
// на this — снять обязательно ДО разрушения панели).
class ConsolePanel {
public:
    void Attach();
    void Detach();
    void Draw();

private:
    struct Entry {
        LogLevel Level;
        std::string Category;
        std::string Message;
    };

    std::vector<Entry> m_entries;
    std::mutex m_mutex;
    bool m_autoScroll = true;
};
