#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Оснастка: glTF с РАЗРЕЖЕННЫМ аксессором — тот самый файл, на котором редактор
// падал.
//
// Разреженный аксессор (sparse) — законная и ЧАСТАЯ вещь: у него нет поля
// bufferView вовсе, сам он считается нулевым, а настоящие значения лежат
// парами «номер вершины — значение». Blender пишет так КАЖДЫЙ ключ формы
// (shape key), потому что у детали из тысячи вершин сдвинуты обычно три.
//
// Чужую модель в репозиторий не положить (вес и лицензия), а скачивать нечего —
// CI без сети. Поэтому файл собирается здесь, и собирается минимальным: один
// треугольник, две кости, один ключ формы. Всё, что в нём есть, — это ровно то,
// на чём ломался разбор.
// ---------------------------------------------------------------------------

namespace sage_test {

// Пишет пару .gltf/.bin в каталог и возвращает путь к .gltf ("" при неудаче).
// sparseMorph = true — дельта ключа формы лежит разреженным аксессором.
inline std::string WriteSparseGltf(const std::filesystem::path& dir, const std::string& stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::vector<unsigned char> bin;
    auto put = [&](const void* p, size_t n) {
        const size_t at = bin.size();
        bin.resize(at + n);
        std::memcpy(bin.data() + at, p, n);
    };
    auto pad4 = [&] { while (bin.size() % 4) bin.push_back(0); };

    // 0: POSITION — треугольник в плоскости XY.
    const size_t posAt = bin.size();
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    put(pos, sizeof(pos));
    // 1: JOINTS_0 — все вершины на кости 0.
    const size_t jointsAt = bin.size();
    const uint8_t joints[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    put(joints, sizeof(joints));
    pad4();
    // 2: WEIGHTS_0
    const size_t weightsAt = bin.size();
    const float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    put(weights, sizeof(weights));
    // 3: индексы
    const size_t idxAt = bin.size();
    const uint16_t idx[3] = {0, 1, 2};
    put(idx, sizeof(idx));
    pad4();
    // 4: обратные bind-матрицы (две единичные)
    const size_t ibmAt = bin.size();
    float ibm[32] = {};
    for (int j = 0; j < 2; ++j)
        for (int c = 0; c < 4; ++c) ibm[j * 16 + c * 4 + c] = 1.0f;
    put(ibm, sizeof(ibm));
    // 5: номера вершин для sparse — сдвинута ОДНА, третья.
    const size_t sIdxAt = bin.size();
    const uint8_t sIdx[1] = {2};
    put(sIdx, sizeof(sIdx));
    pad4();
    // 6: значения для sparse
    const size_t sValAt = bin.size();
    const float sVal[3] = {0.0f, 0.0f, 0.5f};
    put(sVal, sizeof(sVal));

    const std::string binName = stem + ".bin";
    {
        std::ofstream f(dir / binName, std::ios::binary);
        if (!f) return {};
        f.write((const char*)bin.data(), (std::streamsize)bin.size());
    }

    auto view = [](size_t off, size_t len) {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
               ",\"byteLength\":" + std::to_string(len) + "}";
    };
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0,1]}],"
        "\"nodes\":["
        "{\"mesh\":0,\"skin\":0,\"name\":\"part\"},"
        "{\"children\":[2],\"name\":\"root\"},"
        "{\"name\":\"bone\",\"translation\":[0,1,0]}"
        "],"
        "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":4}],"
        "\"meshes\":[{\"name\":\"part\",\"extras\":{\"targetNames\":[\"SuitMode\"]},"
        "\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},"
        "\"indices\":3,\"targets\":[{\"POSITION\":5}]}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
        // ВОТ ОН: аксессора без bufferView. Разбор, который не знает про sparse,
        // лезет в bufferViews[-1] и роняет процесс.
        "{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[0,0,0.5],"
        "\"sparse\":{\"count\":1,"
        "\"indices\":{\"bufferView\":5,\"componentType\":5121},"
        "\"values\":{\"bufferView\":6}}}"
        "],"
        "\"bufferViews\":[" +
        view(posAt, 36) + "," + view(jointsAt, 12) + "," + view(weightsAt, 48) + "," +
        view(idxAt, 6) + "," + view(ibmAt, 128) + "," + view(sIdxAt, 1) + "," + view(sValAt, 12) +
        "],"
        "\"buffers\":[{\"uri\":\"" + binName + "\",\"byteLength\":" + std::to_string(bin.size()) +
        "}]}";

    const fs::path out = dir / (stem + ".gltf");
    {
        std::ofstream f(out, std::ios::binary);
        if (!f) return {};
        f << json;
    }
    return out.string();
}

} // namespace sage_test
