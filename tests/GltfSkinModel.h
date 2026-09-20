#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Оснастка: glTF с ДВУМЯ скинами и ВОСЕМЬЮ влияниями на вершину.
//
// Зачем такой файл. Персонажа экспортёры делят на скины (тело отдельно, одежда
// отдельно), и номер кости в JOINTS_n — это место в списке СВОЕГО скина, а не
// общий номер по файлу. Плюс спецификация разрешает несколько наборов влияний
// (JOINTS_0/WEIGHTS_0, JOINTS_1/WEIGHTS_1 …) — у мягких частей главная кость
// вполне может оказаться во втором.
//
// В файле:
//   • кости-узлы A, B, C, D, E (узлы 3..7);
//   • скин 0 — кости A, B, C, D, E; скин 1 — КОСТИ C и A, именно в таком
//     порядке: номер 0 в его JOINTS_0 обязан привести к кости C, а не к A;
//   • меш «soft» (скин 0) — вершина с ВОСЕМЬЮ влияниями: четыре мелких в
//     нулевом наборе и одно главное (вес 0.8, кость E) в первом. Плюс вершина
//     с БИТЫМ номером кости (99) и весом 0.5 — вес обязан пропасть, а не
//     достаться кости 0;
//   • меш «cloth» (скин 1) — все веса на его кость номер 0.
// ---------------------------------------------------------------------------

namespace sage_test {

// Пишет пару .gltf/.bin и возвращает путь к .gltf ("" при неудаче).
inline std::string WriteTwoSkinGltf(const std::filesystem::path& dir, const std::string& stem) {
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

    // Три вершины на меш: [0] — с восемью влияниями, [1] — с битым номером,
    // [2] — обычная.
    const size_t posAt = bin.size();
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    put(pos, sizeof(pos));

    // Меш «soft», нулевой набор: кости 0..3 с мелкими весами.
    const size_t j0At = bin.size();
    const uint8_t j0[12] = {0, 1, 2, 3,   1, 99, 0, 0,   0, 0, 0, 0};
    put(j0, sizeof(j0));
    pad4();
    const size_t w0At = bin.size();
    const float w0[12] = {0.05f, 0.05f, 0.05f, 0.05f,   0.5f, 0.5f, 0, 0,   1, 0, 0, 0};
    put(w0, sizeof(w0));

    // Первый набор: главная кость E (номер 4) с весом 0.8 — только у вершины 0.
    const size_t j1At = bin.size();
    const uint8_t j1[12] = {4, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0};
    put(j1, sizeof(j1));
    pad4();
    const size_t w1At = bin.size();
    const float w1[12] = {0.8f, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0};
    put(w1, sizeof(w1));

    // Меш «cloth» (скин 1): всё на его кость номер 0.
    const size_t jcAt = bin.size();
    const uint8_t jc[12] = {0, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0};
    put(jc, sizeof(jc));
    pad4();
    const size_t wcAt = bin.size();
    const float wc[12] = {1, 0, 0, 0,   1, 0, 0, 0,   1, 0, 0, 0};
    put(wc, sizeof(wc));

    const size_t idxAt = bin.size();
    const uint16_t idx[3] = {0, 1, 2};
    put(idx, sizeof(idx));
    pad4();

    // Обратные bind-матрицы: пять единичных на скин 0, две на скин 1.
    const size_t ibm0At = bin.size();
    float ibm0[80] = {};
    for (int m = 0; m < 5; ++m)
        for (int i = 0; i < 4; ++i) ibm0[m * 16 + i * 5] = 1.0f;
    put(ibm0, sizeof(ibm0));
    const size_t ibm1At = bin.size();
    float ibm1[32] = {};
    for (int m = 0; m < 2; ++m)
        for (int i = 0; i < 4; ++i) ibm1[m * 16 + i * 5] = 1.0f;
    put(ibm1, sizeof(ibm1));

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
        + "\"scenes\":[{\"nodes\":[0,1,2]}],"
          "\"nodes\":["
          "{\"mesh\":0,\"skin\":0,\"name\":\"soft\"},"           // 0
          "{\"mesh\":1,\"skin\":1,\"name\":\"cloth\"},"          // 1
          "{\"children\":[3],\"name\":\"armature\"},"            // 2
          "{\"children\":[4],\"name\":\"boneA\"},"               // 3
          "{\"children\":[5],\"name\":\"boneB\"},"               // 4
          "{\"children\":[6],\"name\":\"boneC\"},"               // 5
          "{\"children\":[7],\"name\":\"boneD\"},"               // 6
          "{\"name\":\"boneE\"}"                                 // 7
          "],"
          "\"skins\":["
          "{\"joints\":[3,4,5,6,7],\"inverseBindMatrices\":7},"
          "{\"joints\":[5,3],\"inverseBindMatrices\":8}"
          "],"
          "\"meshes\":["
          "{\"name\":\"soft\",\"primitives\":[{\"attributes\":{\"POSITION\":0,"
          "\"JOINTS_0\":1,\"WEIGHTS_0\":2,\"JOINTS_1\":3,\"WEIGHTS_1\":4},\"indices\":9}]},"
          "{\"name\":\"cloth\",\"primitives\":[{\"attributes\":{\"POSITION\":0,"
          "\"JOINTS_0\":5,\"WEIGHTS_0\":6},\"indices\":9}]}"
          "],"
          "\"accessors\":["
          "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
          "\"min\":[0,0,0],\"max\":[1,1,0]},"
          "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":5,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
          "{\"bufferView\":7,\"componentType\":5126,\"count\":5,\"type\":\"MAT4\"},"
          "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
          "{\"bufferView\":9,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
          "],"
          "\"bufferViews\":["
        + view(posAt, 36) + "," + view(j0At, 12) + "," + view(w0At, 48) + "," +
          view(j1At, 12) + "," + view(w1At, 48) + "," + view(jcAt, 12) + "," +
          view(wcAt, 48) + "," + view(ibm0At, 320) + "," + view(ibm1At, 128) + "," +
          view(idxAt, 6) +
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
