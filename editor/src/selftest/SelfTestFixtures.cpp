#include "SelfTestCommon.h"

namespace sage_selftest {


// ---------------------------------------------------------------------------
// МОДЕЛЬ СО СКЕЛЕТОМ — эталон для самопроверки.
//
// Готового такого файла в репозитории нет и быть не должно: скелетные наборы
// весят десятки мегабайт, а проверять надо не чужую модель, а НАШ путь от
// файла до готового к игре персонажа. Поэтому файл пишется здесь — маленький,
// но настоящий: скин из двух костей, треугольник с весами, обратные матрицы
// привязки и ДВА клипа.
//
// Две особенности сделаны нарочно:
//   • клип «Idle» лежит ВТОРЫМ. Выбор клипа по умолчанию обязан искать позу
//     покоя по имени, а не брать первый попавшийся: персонаж, поставленный в
//     сцену, не должен встречать человека прыжком или смертью.
//   • развёрток ДВЕ (TEXCOORD_0 и TEXCOORD_1) с РАЗНЫМИ координатами — ровно
//     как в наборах, экспортированных из 3D-редакторов и магазинов моделей. Движок обязан
//     брать нулевую: перепутанная развёртка выглядит как «текстура наехала не
//     туда», и заметить это на одной развёртке нечем.
// ---------------------------------------------------------------------------
void WriteFloats(std::ofstream& out, const std::vector<float>& v) {
    out.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(v.size() * sizeof(float)));
}

// skinned == false — та же модель БЕЗ скина: декорация. Нужна затем, что
// «не навешивать Animation на камень» — отдельное требование, и проверить его
// можно только на модели, которая скелета не несёт.
bool WriteRigFixture(const fs::path& dir, const std::string& name, bool skinned,
                     std::string& err) {
    std::error_code ec;
    fs::create_directories(dir, ec);

    {
        std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
        if (!bin) { err = "не открыть rig.bin"; return false; }
        // POSITION (3 вершины)
        WriteFloats(bin, {0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f});
        // NORMAL
        WriteFloats(bin, {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f});
        // TEXCOORD_0 — рабочая развёртка
        WriteFloats(bin, {0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f});
        // TEXCOORD_1 — ВТОРАЯ, намеренно другая: если движок возьмёт её,
        // проверка это увидит.
        WriteFloats(bin, {0.25f, 0.75f,  0.5f, 0.75f,  0.25f, 0.5f});
        // JOINTS_0 (ubyte x4 на вершину)
        const unsigned char joints[12] = {0,1,0,0,  0,1,0,0,  0,1,0,0};
        bin.write(reinterpret_cast<const char*>(joints), sizeof(joints));
        // WEIGHTS_0
        WriteFloats(bin, {0.5f,0.5f,0.0f,0.0f,  0.5f,0.5f,0.0f,0.0f,  0.5f,0.5f,0.0f,0.0f});
        // INDICES + выравнивание до четырёх байт
        const unsigned short idx[3] = {0, 1, 2};
        bin.write(reinterpret_cast<const char*>(idx), sizeof(idx));
        const unsigned short pad = 0;
        bin.write(reinterpret_cast<const char*>(&pad), sizeof(pad));
        // inverseBindMatrices: единичная для первой кости, сдвиг на -1 по Y для второй
        WriteFloats(bin, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1});
        WriteFloats(bin, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1});
        // Время клипа и два поворота (кватернионы)
        WriteFloats(bin, {0.0f, 1.0f});
        WriteFloats(bin, {0,0,0,1,  0.0f, 0.0f, 0.3827f, 0.9239f});
    }

    const std::string skinAttrs =
        skinned ? std::string(",\n                     \"JOINTS_0\": 4, \"WEIGHTS_0\": 5")
                : std::string();
    const std::string skins =
        skinned ? std::string("  \"skins\": [{\"inverseBindMatrices\": 7, \"joints\": [1, 2], "
                              "\"skeleton\": 1}],\n")
                : std::string();
    const std::string meshNode =
        skinned
            ? std::string("{\"name\": \"RigMeshNode\", \"mesh\": 0, \"skin\": 0, "
                          "\"scale\": [2, 2, 2]}")
            : std::string("{\"name\": \"RigMeshNode\", \"mesh\": 0, \"scale\": [2, 2, 2]}");

    static const char* kRigGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Rig", "children": [1, 3]},
    {"name": "Bone0", "children": [2]},
    {"name": "Bone1", "translation": [0, 1, 0]},
    @MESHNODE@
  ],
@SKINS@
  "meshes": [{"name": "RigMesh", "primitives": [{
      "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TEXCOORD_1": 3@SKINATTRS@},
      "indices": 6, "material": 0}]}],
  "materials": [{"name": "RigMat", "pbrMetallicRoughness": {
      "baseColorFactor": [0.8, 0.6, 0.2, 1], "metallicFactor": 0, "roughnessFactor": 0.7}}],
  "animations": [
    {"name": "Jumpscare",
     "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
     "samplers": [{"input": 8, "output": 9, "interpolation": "LINEAR"}]},
    {"name": "Rig|Idle",
     "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
     "samplers": [{"input": 8, "output": 9, "interpolation": "LINEAR"}]}
  ],
  "buffers": [{"uri": "@BIN@", "byteLength": 356}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,   "byteLength": 36},
    {"buffer": 0, "byteOffset": 36,  "byteLength": 36},
    {"buffer": 0, "byteOffset": 72,  "byteLength": 24},
    {"buffer": 0, "byteOffset": 96,  "byteLength": 24},
    {"buffer": 0, "byteOffset": 120, "byteLength": 12},
    {"buffer": 0, "byteOffset": 132, "byteLength": 48},
    {"buffer": 0, "byteOffset": 180, "byteLength": 6},
    {"buffer": 0, "byteOffset": 188, "byteLength": 128},
    {"buffer": 0, "byteOffset": 316, "byteLength": 8},
    {"buffer": 0, "byteOffset": 324, "byteLength": 32}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 4, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 6, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "MAT4"},
    {"bufferView": 8, "componentType": 5126, "count": 2, "type": "SCALAR",
     "min": [0], "max": [1]},
    {"bufferView": 9, "componentType": 5126, "count": 2, "type": "VEC4"}
  ]
})";
    std::string text = kRigGltf;
    auto put = [&text](const std::string& mark, const std::string& value) {
        const size_t at = text.find(mark);
        if (at != std::string::npos) text.replace(at, mark.size(), value);
    };
    put("@MESHNODE@", meshNode);
    put("@SKINS@", skins);
    put("@SKINATTRS@", skinAttrs);
    put("@BIN@", name + ".bin");

    std::ofstream gltf(dir / (name + ".gltf"));
    if (!gltf) { err = "не открыть " + name + ".gltf"; return false; }
    gltf << text;
    return true;
}

// Маленький настоящий .zip, встроенный байтами: самопроверка не должна зависеть
// от того, есть ли на машине архиватор.
void WriteSelfTestZip(const std::filesystem::path& path) {
    static const unsigned char kZip[] = {
        0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x5c, 0x16,
        0x2d, 0x5d, 0x25, 0x2f, 0x84, 0xe3, 0x0c, 0x00, 0x00, 0x00, 0x0a, 0x00,
        0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x72, 0x65, 0x61, 0x64, 0x6d, 0x65,
        0x2e, 0x74, 0x78, 0x74, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0x4e,
        0x4c, 0x4f, 0x05, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x5c, 0x16, 0x2d, 0x5d, 0x25, 0x2f, 0x84, 0xe3,
        0x0c, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x72, 0x65, 0x61, 0x64, 0x6d, 0x65, 0x2e, 0x74, 0x78, 0x74,
        0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x38, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(kZip), (std::streamsize)sizeof(kZip));
}

} // namespace sage_selftest
