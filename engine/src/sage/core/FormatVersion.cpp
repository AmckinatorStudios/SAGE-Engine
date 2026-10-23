#include "sage/core/FormatVersion.h"

namespace sage::format {

int VersionOf(const nlohmann::json& doc, const Spec& spec) {
    if (!doc.is_object()) return 0;
    auto it = doc.find(spec.Key);
    if (it == doc.end()) return 1;
    if (!it->is_number_integer()) return 0;
    return it->get<int>();
}

bool Upgrade(nlohmann::json& doc, const Spec& spec, std::string& err) {
    const int version = VersionOf(doc, spec);
    if (version <= 0) {
        err = std::string(spec.What) + ": номер версии формата испорчен (поле " + spec.Key + ")";
        return false;
    }
    if (version > spec.Current) {
        err = std::string(spec.What) + ": файл сохранён более новой версией движка (формат " +
              std::to_string(version) + ", этот движок понимает до " + std::to_string(spec.Current) +
              ") — обновите редактор";
        return false;
    }
    // Работаем на копии: шаг, упавший посередине, не должен оставить
    // документ наполовину переложенным.
    nlohmann::json work = doc;
    for (int v = version; v < spec.Current; ++v) {
        const Step* step = nullptr;
        for (const Step& s : spec.Steps)
            if (s.From == v) step = &s;
        if (!step || !step->Apply) {
            err = std::string(spec.What) + ": нет миграции формата " + std::to_string(v) + " -> " +
                  std::to_string(v + 1);
            return false;
        }
        try {
            step->Apply(work);
        } catch (const std::exception& e) {
            err = std::string(spec.What) + ": миграция " + std::to_string(v) + " -> " + std::to_string(v + 1) +
                  " не удалась: " + e.what();
            return false;
        }
    }
    work[spec.Key] = spec.Current;
    doc = std::move(work);
    return true;
}

void Stamp(nlohmann::json& doc, const Spec& spec) { doc[spec.Key] = spec.Current; }

} // namespace sage::format
