#pragma once
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// СПИСКИ ПУТЕЙ ДИАЛОГА ФАЙЛОВ: избранное и история.
//
// Хранятся в настройках редактора одной строкой (пути через перевод строки):
// перевод строки в имени папки не встречается, а отдельный формат ради двух
// списков был бы лишним. Правила — здесь, без ImGui и без диска, чтобы их
// проверял модульный тест.
// ---------------------------------------------------------------------------
namespace sage::editor::pathlist {

inline std::vector<std::string> Split(const std::string& joined) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= joined.size()) {
        const size_t nl = joined.find('\n', start);
        const std::string item = joined.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!item.empty()) out.push_back(item);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

inline std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) {
        if (!out.empty()) out += '\n';
        out += s;
    }
    return out;
}

// История: путь — в начало, прежняя запись того же пути убирается (иначе
// список забивался бы одной и той же папкой), хвост сверх cap отрезается.
inline void Remember(std::vector<std::string>& list, const std::string& path, size_t cap) {
    if (path.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > cap) list.resize(cap);
}

inline bool Contains(const std::vector<std::string>& list, const std::string& path) {
    return std::find(list.begin(), list.end(), path) != list.end();
}

// Избранное: добавить, если нет; убрать, если есть. Возвращает, лежит ли
// путь в списке после вызова. Новое — в конец: порядок избранного задаёт
// человек, и новая папка не должна сдвигать привычные.
inline bool Toggle(std::vector<std::string>& list, const std::string& path) {
    if (Contains(list, path)) {
        list.erase(std::remove(list.begin(), list.end(), path), list.end());
        return false;
    }
    list.push_back(path);
    return true;
}

} // namespace sage::editor::pathlist
