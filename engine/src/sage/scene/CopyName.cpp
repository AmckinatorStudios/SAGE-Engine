#include "sage/scene/CopyName.h"

#include <cctype>

namespace sage::scene {

namespace {

constexpr const char* kSuffix = " Copy";

// Отбрасывает хвост « Copy» или « Copy <число>». Возвращает настоящую основу
// имени — ту, от которой копия и произошла.
std::string BaseOf(const std::string& name) {
    std::string s = name;
    // Цикл, а не одна проверка: имена вида «Куб Copy Copy 2» уже лежат в чужих
    // сценах, сохранённых прежним правилом, и их тоже надо уметь сокращать.
    for (;;) {
        std::size_t end = s.size();
        // Сначала снимаем возможный номер: «… Copy 2».
        std::size_t digits = end;
        while (digits > 0 && std::isdigit((unsigned char)s[digits - 1])) --digits;
        if (digits < end && digits > 0 && s[digits - 1] == ' ') end = digits - 1;

        const std::size_t suffixLen = std::char_traits<char>::length(kSuffix);
        if (end < suffixLen) return s;
        if (s.compare(end - suffixLen, suffixLen, kSuffix) != 0) return s;
        s = s.substr(0, end - suffixLen);
        if (s.empty()) return name;   // имя состояло из одного «Copy» — не трогаем
    }
}

} // namespace

std::string CopyName(const std::string& sourceName,
                     const std::function<bool(const std::string&)>& taken) {
    const std::string base = BaseOf(sourceName);

    std::string candidate = base + kSuffix;
    if (!taken || !taken(candidate)) return candidate;

    // Номера начинаются с 2: первая копия называется просто «Copy», и «Copy 1»
    // рядом с ней читалось бы как «а где тогда нулевая».
    for (int n = 2; n < 100000; ++n) {
        candidate = base + kSuffix + " " + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
    return base + kSuffix;   // сто тысяч копий — берите какое есть
}

} // namespace sage::scene
