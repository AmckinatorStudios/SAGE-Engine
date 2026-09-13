#include "TextureSet.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace sage::editor {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Назначение карты по суффиксу имени. Порядок внутри каждой строки — от более
// длинного варианта к короткому: «basecolor» обязан проверяться раньше, чем
// «base», иначе короткий вариант съедал бы длинный и корень имени выходил бы
// неверным.
enum class Slot { None, Albedo, Normal, Metallic, Roughness, AO, Emissive, Unused };

struct SuffixRule {
    const char* Text;
    Slot Which;
};

// Имена взяты у источников, откуда наборы и берут: ambientCG, Poliigon, Quixel,
// Poly Haven, texture.ninja, Substance. Список намеренно длинный — короткий
// означал бы «понимаю только свои файлы».
const SuffixRule kSuffixes[] = {
    {"basecolor", Slot::Albedo},   {"base_color", Slot::Albedo},
    {"albedo", Slot::Albedo},      {"diffuse", Slot::Albedo},
    {"diff", Slot::Albedo},        {"color", Slot::Albedo},
    {"col", Slot::Albedo},

    {"normalgl", Slot::Normal},    {"normal-ogl", Slot::Normal},
    {"normal_ogl", Slot::Normal},  {"normaldx", Slot::Normal},
    {"normal-dx", Slot::Normal},   {"normal_dx", Slot::Normal},
    {"normalmap", Slot::Normal},   {"normal", Slot::Normal},
    {"nrm", Slot::Normal},         {"nor", Slot::Normal},

    {"metallic", Slot::Metallic},  {"metalness", Slot::Metallic},
    {"metal", Slot::Metallic},

    {"roughness", Slot::Roughness}, {"rough", Slot::Roughness},
    {"gloss", Slot::Unused},        // глянец — обратная шероховатость, не она

    {"ambientocclusion", Slot::AO}, {"occlusion", Slot::AO},
    {"ao", Slot::AO},

    {"emissive", Slot::Emissive},  {"emission", Slot::Emissive},
    {"emit", Slot::Emissive},

    // Есть в наборах, но движок их не применяет — об этом сообщается вслух.
    {"height", Slot::Unused},      {"displacement", Slot::Unused},
    {"disp", Slot::Unused},        {"bump", Slot::Unused},
    {"preview", Slot::Unused},
};

bool IsSeparator(char c) { return c == '_' || c == '-' || c == '.' || c == ' '; }

// Отрезает от имени известный суффикс карты. Возвращает назначение и корень.
Slot SplitSuffix(const std::string& stem, std::string& outBase, bool& outDirectX) {
    const std::string lower = Lower(stem);
    Slot best = Slot::None;
    size_t bestCut = std::string::npos;
    size_t bestLen = 0;
    for (const SuffixRule& rule : kSuffixes) {
        const std::string suffix = rule.Text;
        if (lower.size() <= suffix.size()) continue;
        const size_t at = lower.size() - suffix.size();
        if (lower.compare(at, suffix.size(), suffix) != 0) continue;
        // Суффикс обязан быть ОТДЕЛЬНЫМ словом: иначе «metal.png» (имя
        // материала) читалось бы как карта металличности файла «».
        if (!IsSeparator(lower[at - 1])) continue;
        if (suffix.size() <= bestLen) continue;   // берём самое длинное совпадение
        best = rule.Which;
        bestLen = suffix.size();
        bestCut = at - 1;
    }
    if (best == Slot::None) {
        outBase = stem;
        return Slot::None;
    }
    outBase = stem.substr(0, bestCut);
    if (best == Slot::Normal) {
        const std::string tail = lower.substr(bestCut);
        outDirectX = tail.find("dx") != std::string::npos;
    }
    return best;
}

bool IsImage(const fs::path& p) {
    static const char* kExts[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    const std::string ext = Lower(p.extension().string());
    for (const char* e : kExts)
        if (ext == e) return true;
    return false;
}

} // namespace

TextureSet FindTextureSet(const fs::path& anyMap) {
    TextureSet set;
    std::error_code ec;
    if (anyMap.empty()) return set;

    bool dx = false;
    std::string base;
    SplitSuffix(anyMap.stem().string(), base, dx);
    set.Base = base;
    // Суффикса нет — набор из одной карты: файл идёт в albedo, и это честнее,
    // чем отказ «имя не по правилам».
    if (base.empty() || base == anyMap.stem().string()) {
        set.Base = anyMap.stem().string();
        set.Albedo = anyMap.string();
        return set;
    }

    const fs::path dir = anyMap.parent_path();
    const std::string baseLower = Lower(base);
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || !IsImage(entry.path())) continue;

        bool fileDx = false;
        std::string fileBase;
        const Slot slot = SplitSuffix(entry.path().stem().string(), fileBase, fileDx);
        if (slot == Slot::None || Lower(fileBase) != baseLower) continue;

        const std::string path = entry.path().string();
        switch (slot) {
            case Slot::Albedo:    if (set.Albedo.empty()) set.Albedo = path; break;
            case Slot::Metallic:  if (set.Metallic.empty()) set.Metallic = path; break;
            case Slot::Roughness: if (set.Roughness.empty()) set.Roughness = path; break;
            case Slot::AO:        if (set.AO.empty()) set.AO = path; break;
            case Slot::Emissive:  if (set.Emissive.empty()) set.Emissive = path; break;
            case Slot::Normal:
                // OpenGL-вариант ПРЕДПОЧТИТЕЛЕН: движок ждёт именно его. Набор
                // сплошь и рядом кладёт обе карты рядом, и взять «первую
                // попавшуюся» значит через раз получать рельеф наизнанку.
                if (set.Normal.empty() || (set.NormalIsDirectX && !fileDx)) {
                    set.Normal = path;
                    set.NormalIsDirectX = fileDx;
                }
                break;
            case Slot::Unused:    set.Unused.push_back(entry.path().filename().string()); break;
            case Slot::None:      break;
        }
    }

    // Файл, с которого начали, обязан попасть в набор даже если его суффикс не
    // опознан: он ведь лежит в руках у человека.
    if (set.Found() == 0) set.Albedo = anyMap.string();
    return set;
}

} // namespace sage::editor
