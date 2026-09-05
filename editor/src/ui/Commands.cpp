#include "ui/Commands.h"

#include "ui/UI.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace Sage::UI {

void CommandRegistry::Add(Command cmd) {
    if (cmd.Id.empty()) return;
    // Повтор — ошибка сборки списка, а не повод завести вторую такую же
    // команду: в палитре появились бы два одинаковых пункта.
    for (Command& c : m_commands) {
        if (c.Id == cmd.Id) {
            c = std::move(cmd);
            return;
        }
    }
    m_commands.push_back(std::move(cmd));
}

const Command* CommandRegistry::Find(const std::string& id) const {
    for (const Command& c : m_commands)
        if (c.Id == id) return &c;
    return nullptr;
}

bool CommandRegistry::Run(const std::string& id) {
    const Command* c = Find(id);
    if (!c || !c->Run) return false;
    if (c->Enabled && !c->Enabled()) return false;
    c->Run();
    return true;
}

std::vector<int> CommandRegistry::Search(const std::string& query) const {
    std::vector<int> out;
    const std::string q = Fold(query);
    if (q.empty()) {
        for (int i = 0; i < (int)m_commands.size(); ++i) out.push_back(i);
        return out;
    }
    // Совпадение С НАЧАЛА подписи важнее совпадения в середине: набирая «соз»,
    // человек ищет «Создать...», а не «Пересоздать раскладку».
    std::vector<std::pair<int, int>> ranked;   // (вес, индекс)
    for (int i = 0; i < (int)m_commands.size(); ++i) {
        const std::string title = Fold(m_commands[i].Title);
        const std::string cat = Fold(m_commands[i].Category);
        const size_t inTitle = title.find(q);
        if (inTitle == 0) ranked.push_back({0, i});
        else if (inTitle != std::string::npos) ranked.push_back({1, i});
        else if (cat.find(q) != std::string::npos) ranked.push_back({2, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                         return a.first < b.first;
                     });
    for (const std::pair<int, int>& r : ranked) out.push_back(r.second);
    return out;
}

} // namespace Sage::UI
