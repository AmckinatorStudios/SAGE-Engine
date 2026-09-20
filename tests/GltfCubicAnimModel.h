#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Оснастка: glTF со скином и КУБИЧЕСКОЙ анимацией (CUBICSPLINE).
//
// Зачем именно такой файл. У CUBICSPLINE на каждый ключ приходится ТРОЙКА
// значений: касательная на входе, значение, касательная на выходе
// (спецификация glTF, 3.11). Разбор, который об этом не знает, берёт первую из
// тройки — и позой становится касательная. Поэтому касательные здесь заведомо
// НЕ равны значениям: если разбор ошибётся, поза окажется ими, и проверка это
// увидит. На файле, где касательные нулевые, ошибка была бы незаметна.
//
// Чужую модель в репозиторий не положить (вес и лицензия), а CI без сети —
// файл собирается здесь и собирается минимальным: треугольник, две кости, один
// клип из двух ключей.
// ---------------------------------------------------------------------------

namespace sage_test {

// Пишет пару .gltf/.bin и возвращает путь к .gltf ("" при неудаче).
// Поворот кости: от 0° до 90° вокруг Y; касательные — заведомо чужие числа.
inline std::string WriteCubicAnimGltf(const std::filesystem::path& dir, const std::string& stem) {
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

    const size_t posAt = bin.size();
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    put(pos, sizeof(pos));

    const size_t jointsAt = bin.size();
    const uint8_t joints[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    put(joints, sizeof(joints));
    pad4();

    const size_t weightsAt = bin.size();
    const float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    put(weights, sizeof(weights));

    const size_t idxAt = bin.size();
    const uint16_t idx[3] = {0, 1, 2};
    put(idx, sizeof(idx));
    pad4();

    const size_t ibmAt = bin.size();
    float ibm[32] = {};
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < 4; ++i) ibm[m * 16 + i * 5] = 1.0f;   // две единичные матрицы
    put(ibm, sizeof(ibm));

    // Времена ключей: 0 и 1 секунда.
    const size_t timeAt = bin.size();
    const float times[2] = {0.0f, 1.0f};
    put(times, sizeof(times));

    // Значения: по ТРОЙКЕ на ключ — вход, значение, выход.
    const float s45 = std::sin(3.14159265f * 0.25f);   // sin(45°) — половина от 90°
    const float c45 = std::cos(3.14159265f * 0.25f);
    const size_t rotAt = bin.size();
    const float rot[24] = {
        // ключ 0: касательная на входе (заведомо чужая), значение (поворот 0),
        // касательная на выходе
        0.5f, 0.5f, 0.5f, 0.5f,   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 0.0f,
        // ключ 1: вход, значение (поворот 90° вокруг Y), выход
        0.0f, 0.0f, 0.0f, 0.0f,   0.0f, s45,  0.0f, c45,    0.25f, 0.25f, 0.25f, 0.25f,
    };
    put(rot, sizeof(rot));

    const std::string binName = stem + ".bin";
    {
        std::ofstream f(dir / binName, std::ios::binary);
        if (!f) return {};
        f.write((const char*)bin.data(), (std::streamsize)bin.size());
    }

    auto view = [&](size_t off, size_t len) {
        return std::string("{\"buffer\":0,\"byteOffset\":") + std::to_string(off) +
               ",\"byteLength\":" + std::to_string(len) + "}";
    };

    const std::string json =
        std::string("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,")
        + "\"scenes\":[{\"nodes\":[0,1]}],"
          "\"nodes\":["
          "{\"mesh\":0,\"skin\":0,\"name\":\"part\"},"
          "{\"children\":[2],\"name\":\"root\"},"
          "{\"name\":\"bone\",\"translation\":[0,1,0]}"
          "],"
          "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":4}],"
          "\"meshes\":[{\"name\":\"part\",\"primitives\":[{\"attributes\":"
          "{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]}],"
          "\"animations\":[{\"name\":\"Turn\","
          "\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"CUBICSPLINE\"}],"
          "\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}],"
          "\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
          "\"min\":[0,0,0],\"max\":[1,1,0]},"
          "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
          "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
          "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\","
          "\"min\":[0],\"max\":[1]},"
          // СЧЁТЧИК ВЫХОДА У CUBICSPLINE — ВТРОЕ БОЛЬШЕ ЧИСЛА КЛЮЧЕЙ (так
          // требует спецификация: на ключ приходится тройка «касательная,
          // значение, касательная»). Ключей два, значений шесть — и ровно на
          // этом несовпадении и ломался разбор.
          "{\"bufferView\":6,\"componentType\":5126,\"count\":6,\"type\":\"VEC4\"}"
          "],"
          "\"bufferViews\":["
        + view(posAt, 36) + "," + view(jointsAt, 12) + "," + view(weightsAt, 48) + "," +
          view(idxAt, 6) + "," + view(ibmAt, 128) + "," + view(timeAt, 8) + "," +
          view(rotAt, 96) +
          "],"
          "\"buffers\":[{\"uri\":\"" + binName + "\",\"byteLength\":" +
          std::to_string(bin.size()) + "}]}";

    const fs::path out = dir / (stem + ".gltf");
    {
        std::ofstream f(out, std::ios::binary);
        if (!f) return {};
        f << json;
    }
    return out.string();
}

} // namespace sage_test
