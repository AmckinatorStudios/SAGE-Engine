#pragma once
#include "Mesh.h"
#include <memory>
#include <string>

namespace ModelLoader {
    // CPU-часть: читает и парсит .obj в MeshData (вершины/индексы) — БЕЗ GL,
    // поэтому безопасно вызывать на фоновом потоке (JobSystem). Бросает
    // std::runtime_error при ошибке чтения/разбора.
    MeshData LoadObjData(const std::string& path);

    // Загружает .obj и сразу создаёт Mesh (CPU-парсинг + GL-загрузка).
    // Вызывать в главном потоке. Бросает std::runtime_error при ошибке.
    std::shared_ptr<Mesh> LoadObj(const std::string& path);
}
