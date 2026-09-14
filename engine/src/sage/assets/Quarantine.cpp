#include "sage/assets/Quarantine.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::assets::quarantine {
namespace {

// Метка «сейчас разбираю» и список карантина лежат РЯДОМ С РЕДАКТОРОМ, а не в
// проекте: проект может не открыться вовсе, а список нужен раньше него.
constexpr const char* kMarkFile = "sage-loading.txt";
constexpr const char* kListFile = "sage-quarantine.txt";

struct State {
    std::mutex Mx;
    fs::path Dir;
    bool Enabled = false;
    // Файл -> время его правки на момент посадки в карантин. По нему карантин
    // снимается сам: человек переэкспортировал модель — она снова обычный файл,
    // и запрещать её было бы наказанием за чужую ошибку.
    std::unordered_map<std::string, long long> Blocked;
    int Depth = 0;   // вложенность Begin: разбор модели тянет её текстуры
};

State& S() {
    static State s;
    return s;
}

long long StampOf(const std::string& ref) {
    std::error_code ec;
    const fs::path disk = sage::PathFromUtf8(sage::AssetDatabase::Instance().LocatePath(ref));
    const auto when = fs::last_write_time(disk, ec);
    return ec ? 0 : (long long)when.time_since_epoch().count();
}

void SaveList(State& s) {
    if (!s.Enabled) return;
    std::error_code ec;
    const fs::path path = s.Dir / kListFile;
    if (s.Blocked.empty()) {
        fs::remove(path, ec);
        return;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& [ref, stamp] : s.Blocked) f << stamp << '\t' << ref << '\n';
}

void LoadList(State& s) {
    std::ifstream f(s.Dir / kListFile, std::ios::binary);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string ref = line.substr(tab + 1);
        if (ref.empty()) continue;
        s.Blocked[ref] = std::strtoll(line.substr(0, tab).c_str(), nullptr, 10);
    }
}

} // namespace

void SetDirectory(const std::string& dir) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    s.Dir = sage::PathFromUtf8(dir);
    s.Enabled = !dir.empty();
    s.Blocked.clear();
    // Незакрытая вложенность — это и есть прерванная работа прошлого запуска;
    // в новом её нет. Без сброса следующий Begin считал бы себя вложенным и
    // метки не поставил — то есть предохранитель молча перестал бы работать.
    s.Depth = 0;
    if (s.Enabled) LoadList(s);
}

std::string TakeUnfinished() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    if (!s.Enabled) return {};
    const fs::path mark = s.Dir / kMarkFile;
    std::error_code ec;
    if (!fs::exists(mark, ec)) return {};

    std::string ref;
    {
        std::ifstream f(mark, std::ios::binary);
        std::getline(f, ref);
    }
    fs::remove(mark, ec);
    while (!ref.empty() && (ref.back() == '\r' || ref.back() == '\n')) ref.pop_back();
    if (ref.empty()) return {};

    s.Blocked[ref] = StampOf(ref);
    SaveList(s);
    LOG_ERROR("Assets") << "Прошлый запуск оборвался на файле «" << ref
                        << "» — он в карантине и загружаться не будет. "
                        << "Поправьте или перезапишите файл: как только он изменится на диске, "
                        << "карантин снимется сам.";
    return ref;
}

void Begin(const std::string& ref) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    if (!s.Enabled || ref.empty()) return;
    // ВЛОЖЕННОСТЬ: разбор модели тянет её текстуры, и метка обязана называть
    // ВНЕШНЮЮ работу — ту, которую человек затеял. Пометив текстуру, мы после
    // падения посадили бы в карантин её, а виновата была бы модель.
    if (s.Depth++ > 0) return;
    std::ofstream f(s.Dir / kMarkFile, std::ios::binary | std::ios::trunc);
    if (f.is_open()) f << ref << '\n';
}

void End() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    if (!s.Enabled) return;
    if (s.Depth > 0 && --s.Depth > 0) return;
    std::error_code ec;
    fs::remove(s.Dir / kMarkFile, ec);
}

bool Blocked(const std::string& ref) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    auto it = s.Blocked.find(ref);
    if (it == s.Blocked.end()) return false;
    // Файл изменился — карантин снят. Карантин это предохранитель, а не
    // приговор: человек починил файл, и движок обязан это заметить сам.
    if (StampOf(ref) != it->second) {
        s.Blocked.erase(it);
        SaveList(s);
        LOG_INFO("Assets") << "Файл «" << ref << "» изменился — карантин снят";
        return false;
    }
    return true;
}

std::vector<std::string> List() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    std::vector<std::string> out;
    out.reserve(s.Blocked.size());
    for (const auto& [ref, stamp] : s.Blocked) out.push_back(ref);
    std::sort(out.begin(), out.end());
    return out;
}

void Release(const std::string& ref) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    if (s.Blocked.erase(ref) > 0) SaveList(s);
}

void Clear() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    s.Blocked.clear();
    SaveList(s);
}

} // namespace sage::assets::quarantine
