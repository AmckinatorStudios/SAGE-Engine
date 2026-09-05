#include "ui/Commands.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace Sage::UI {

namespace {

// Сравнение без учёта регистра. Не std::tolower по одному байту: кириллица в
// UTF-8 занимает два, и побайтная свёртка сложила бы регистр только у
// латиницы — то есть поиск по-русски работал бы «через раз».
std::string Fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out.push_back((char)std::tolower(c));
            ++i;
        } else if (c == 0xD0 && i + 1 < s.size()) {
            // А-П это D0 90..9F, Р-Я это D0 A0..AF, Ё это D0 81.
            const unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F) {
                out.push_back((char)0xD0);
                out.push_back((char)(d + 0x20));
            } else if (d >= 0xA0 && d <= 0xAF) {
                out.push_back((char)0xD1);
                out.push_back((char)(d - 0x20));
            } else if (d == 0x81) {
                out.push_back((char)0xD1);
                out.push_back((char)0x91);
            } else {
                out.push_back((char)c);
                out.push_back((char)d);
            }
            i += 2;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            out.push_back((char)c);
            out.push_back(s[i + 1]);
            i += 2;
        } else {
            out.push_back((char)c);
            ++i;
        }
    }
    return out;
}

} // namespace

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
