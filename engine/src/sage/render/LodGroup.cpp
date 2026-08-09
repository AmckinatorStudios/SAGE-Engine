#include "sage/render/LodGroup.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/render/MeshSimplify.h"

namespace sage::render {

bool BuildAutoLods(LodComponent& lod, const Mesh& source, int coarseLevels) {
    lod.Meshes.clear();
    lod.Levels.ScreenHeights.clear();

    const std::vector<Vertex>* verts = source.CpuVertices();
    const std::vector<unsigned int>* idx = source.CpuIndices();
    if (!verts || !idx || idx->size() < 3) {
        // Молчать здесь нельзя: «уровни не построились» выглядит в кадре как
        // «LOD не работает», и без сообщения искать причину пришлось бы долго.
        LOG_WARN("LOD") << "Уровни не построены: у меша нет копии геометрии "
                           "(создавайте Mesh с keepCpuData=true)";
        return false;
    }

    coarseLevels = std::clamp(coarseLevels, 0, 4);

    // Порог самого подробного уровня. Ноль означает «пока объект крупнее
    // следующего порога» — то есть исходный меш работает до первого перехода.
    float screenHeight = 0.25f; // около четверти высоты кадра
    float ratio = 1.0f;
    lod.Levels.Add(screenHeight);

    for (int i = 0; i < coarseLevels; ++i) {
        ratio *= 0.25f;
        screenHeight *= 0.5f;

        // Разметку по материалам передаём вниз: грубый уровень
        // многоматериальной модели обязан остаться многоматериальным, иначе
        // переход на LOD выглядел бы как перекраска всей модели разом.
        SimplifyResult r = SimplifyToRatio(*verts, *idx, ratio,
                                           source.HasExplicitSubmeshes()
                                               ? source.Submeshes()
                                               : std::vector<Submesh>{});
        if (r.Triangles < 4) break; // дальше упрощать нечего — уровень был бы пустым

        lod.Meshes.push_back(std::make_shared<Mesh>(r.Vertices, r.Indices, r.Submeshes));
        lod.Levels.Add(screenHeight);
    }

    // Отсечение по размеру: объект мельче четверти процента высоты кадра
    // (около трёх пикселей на 1080p) не рисуется. Это самая дешёвая экономия из
    // всех — такой объект стоит полного вызова отрисовки, а виден не бывает.
    lod.Levels.CullBelow = 0.0025f;
    return !lod.Meshes.empty();
}

} // namespace sage::render
