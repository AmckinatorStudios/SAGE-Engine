#pragma once
#include "Mesh.h"
#include <memory>
#include <string>

namespace ModelLoader {
    // Загружает .obj файл и возвращает Mesh. Бросает std::runtime_error при ошибке.
    std::shared_ptr<Mesh> LoadObj(const std::string& path);
}
