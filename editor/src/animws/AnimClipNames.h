#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ОТОБРАЖАЕМЫЕ ИМЕНА КЛИПОВ МОДЕЛИ. Только для списка в интерфейсе: настоящие
// имена, индексы и ссылки на клипы не меняются — по ним клип ищут, назначают
// и сохраняют.
//
// Экспорт из DCC-пакетов склеивает имя из пути узлов: «Hero|Hero|Hero|Walk».
// Список из таких строк не читается: различающая часть спрятана за краем
// строки. Здесь делаются два шага:
//   1. подряд идущие одинаковые части «A|A|A|Walk» схлопываются в «A|Walk»;
//   2. начальные части, общие у клипов списка, выносятся в заголовок —
//      у каждого клипа остаётся то, чем он отличается.
// Одинаковые отображаемые имена различаются номером клипа в списке.
// ---------------------------------------------------------------------------
namespace animnames {

inline std::vector<std::string> Split(const std::string& name) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t bar = name.find('|', start);
        parts.push_back(name.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return parts;
}

inline std::string Join(const std::vector<std::string>& parts, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < parts.size(); ++i) {
        if (i > from) out += '|';
        out += parts[i];
    }
    return out;
}

// names — настоящие имена клипов; sharedPrefix (если задан) получает общую
// начальную часть, вынесенную из строк («» — выносить нечего).
inline std::vector<std::string> DisplayNames(const std::vector<std::string>& names,
                                             std::string* sharedPrefix = nullptr) {
    std::vector<std::vector<std::string>> parts;
    parts.reserve(names.size());
    for (const std::string& n : names) {
        std::vector<std::string> raw = Split(n), collapsed;
        for (std::string& p : raw)
            if (collapsed.empty() || collapsed.back() != p) collapsed.push_back(std::move(p));
        parts.push_back(std::move(collapsed));
    }
    // Общая начальная часть — по клипам, у которых есть что оставить после неё
    // (хотя бы две части); клип из одной части показывается как есть.
    size_t common = 0;
    int multi = 0;
    for (const auto& p : parts)
        if (p.size() >= 2) ++multi;
    if (multi >= 2) {
        const std::vector<std::string>* first = nullptr;
        size_t limit = 0;
        for (const auto& p : parts) {
            if (p.size() < 2) continue;
            if (!first) { first = &p; limit = p.size() - 1; continue; }
            size_t k = 0;
            while (k < limit && k < p.size() - 1 && (*first)[k] == p[k]) ++k;
            limit = k;
        }
        common = limit;
    }
    if (sharedPrefix) *sharedPrefix = common && !parts.empty()
        ? [&] { for (const auto& p : parts) if (p.size() >= 2) return Join(p, 0, common); return std::string(); }()
        : std::string();
    std::vector<std::string> out;
    out.reserve(parts.size());
    for (const auto& p : parts)
        out.push_back(p.size() >= 2 && common ? Join(p, common, p.size()) : Join(p, 0, p.size()));
    // Одинаковые имена — с номером: без него два «Idle» нельзя отличить.
    for (size_t i = 0; i < out.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < out.size() && !dup; ++j) dup = j != i && out[j] == out[i];
        if (dup) out[i] += " (#" + std::to_string(i + 1) + ")";
    }
    return out;
}

} // namespace animnames
