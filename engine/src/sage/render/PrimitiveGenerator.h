#pragma once

#include "sage/scene/Components.h"

#include <memory>
#include <iterator>
#include <string>
#include <unordered_map>

class Mesh;

namespace sage::render {

// Кэш и создание процедурных форм. Изолирован от файловых ресурсов, текстур
// и GPU-бюджета: новый тип формы меняется только здесь.
class PrimitiveGenerator {
public:
    std::shared_ptr<Mesh> Get(MeshRef::Type type, bool keepCpuData);
    std::shared_ptr<Mesh> Get(MeshRef::Type type, const PrimitiveParams& params, bool keepCpuData);
    void Clear();

private:
    static constexpr size_t kCount = (size_t)MeshRef::Type::Model + 1;
    std::shared_ptr<Mesh> m_defaults[kCount];
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_variants;
};

} // namespace sage::render
