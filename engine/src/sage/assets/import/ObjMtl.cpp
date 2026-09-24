#include "sage/assets/import/ObjMtl.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include "sage/core/Paths.h"

namespace sage::assets {

namespace {

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Строка начинается с ключевого слова, за которым пробел (map_Kd не должно
// считаться строкой Kd, а Kd — строкой K).
bool Keyword(const std::string& line, const char* kw, std::string* rest = nullptr) {
    const size_t n = std::char_traits<char>::length(kw);
    if (line.compare(0, n, kw) != 0) return false;
    if (line.size() > n && line[n] != ' ' && line[n] != '\t') return false;
    if (rest) *rest = Trim(line.substr(n));
    return true;
}

} // namespace

std::unordered_set<std::string> MtlTexturedWithoutKd(const std::string& objPath) {
    namespace fs = std::filesystem;
    std::unordered_set<std::string> out;
    std::ifstream obj(sage::PathFromUtf8(objPath));
    if (!obj) return out;

    // mtllib может стоять где угодно, но пишут её в шапке; читаем строки целиком,
    // сравнение идёт по первым символам — это дёшево и на большом файле.
    std::vector<std::string> libs;
    std::string line;
    while (std::getline(obj, line)) {
        std::string rest;
        if (Keyword(Trim(line), "mtllib", &rest) && !rest.empty()) libs.push_back(rest);
    }
    const fs::path dir = sage::PathFromUtf8(objPath).parent_path();
    for (const std::string& lib : libs) {
        std::ifstream mtl(dir / sage::PathFromUtf8(lib));
        if (!mtl) continue;
        std::string name;
        bool hasKd = false, hasMap = false;
        auto flush = [&]() {
            if (!name.empty() && hasMap && !hasKd) out.insert(name);
        };
        while (std::getline(mtl, line)) {
            const std::string t = Trim(line);
            std::string rest;
            if (Keyword(t, "newmtl", &rest)) {
                flush();
                name = rest;
                hasKd = hasMap = false;
            } else if (Keyword(t, "Kd")) {
                hasKd = true;
            } else if (Keyword(t, "map_Kd")) {
                hasMap = true;
            }
        }
        flush();
    }
    return out;
}

} // namespace sage::assets
