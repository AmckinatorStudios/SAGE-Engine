// GLB, у которого врёт шапка.
//
// Живой случай: модель Bed_HN.glb из набора ассетов не открывалась ни в
// редакторе, ни в игре — «не удалось разобрать glTF: Invalid glTF binary». В
// шапке GLB лежит поле «общая длина файла», и tinygltf сверяет его с размером
// файла побайтно. Упаковщики ассетов это поле портят постоянно: дописывают
// выравнивание в конец, оставляют длину от файла ДО правки, кладут за концом
// хвост. Куски (JSON и BIN) при этом целы и читаются — теряется только доверие
// к одному числу в заголовке.
//
// Поэтому GLB пересобирается перед разбором: куски вынимаются, выравнивание и
// длина пишутся заново. Обрезанный файл — единственное, что чинить нельзя, и
// про него надо сказать прямо, а не «формат не тот».
#include "TestFramework.h"

#include "sage/assets/import/GltfFile.h"
#include "sage/assets/import/Importer.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sage::assets;

namespace {

void PutU32(std::string& d, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    d.append(b, 4);
}

// Двоичные данные одного треугольника: три вершины (VEC3 float) и три индекса.
std::string TriangleBin() {
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::uint16_t idx[3] = {0, 1, 2};
    std::string bin;
    bin.append(reinterpret_cast<const char*>(pos), sizeof(pos));
    bin.append(reinterpret_cast<const char*>(idx), sizeof(idx));
    return bin; // 42 байта — нарочно НЕ кратно четырём, выравнивание добавит сборщик
}

std::string TriangleJson() {
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
           R"("nodes":[{"mesh":0}],)"
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
           R"("buffers":[{"byteLength":42}],)"
           R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
           R"({"buffer":0,"byteOffset":36,"byteLength":6}],)"
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
           R"("min":[0,0,0],"max":[1,1,0]},)"
           R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
}

// Собирает GLB. lengthDelta — на сколько соврать в поле «общая длина»,
// padJson — добивать ли кусок JSON до четырёх байт (как требует спецификация).
std::string MakeGlb(int lengthDelta = 0, bool padJson = true, const std::string& tail = "") {
    std::string json = TriangleJson();
    if (padJson) json.append((4 - json.size() % 4) % 4, ' ');
    std::string bin = TriangleBin();
    std::string padBin = bin;
    padBin.append((4 - bin.size() % 4) % 4, '\0');

    std::string glb;
    glb.append("glTF", 4);
    PutU32(glb, 2);
    PutU32(glb, 0); // длина — ниже
    PutU32(glb, (std::uint32_t)json.size());
    PutU32(glb, 0x4E4F534A);
    glb += json;
    PutU32(glb, (std::uint32_t)padBin.size());
    PutU32(glb, 0x004E4942);
    glb += padBin;

    const std::uint32_t total = (std::uint32_t)((int)glb.size() + lengthDelta);
    std::memcpy(&glb[8], &total, 4);
    glb += tail;
    return glb;
}

std::string Write(const std::string& name, const std::string& data) {
    std::ofstream f(name, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    return name;
}

bool Load(const std::string& path, std::string& err, size_t& triangles) {
    ImportedScene scene;
    const bool ok = ImportGltf(path, scene, err);
    triangles = 0;
    for (const auto& n : scene.Nodes) triangles += n.Mesh.Indices.size() / 3;
    return ok;
}

} // namespace

TEST(Glb_opens_when_header_length_is_correct) {
    const std::string path = Write("sage_test_ok.glb", MakeGlb());
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    CHECK_TRUE(ok);
    CHECK_EQ((int)tris, 1);
}

TEST(Glb_with_lying_header_length_still_opens) {
    // Ровно тот файл, на котором движок отвечал «Invalid glTF binary»:
    // в шапке длина БОЛЬШЕ файла. Все данные на месте — модель обязана открыться.
    const std::string path = Write("sage_test_long.glb", MakeGlb(+64));
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    CHECK_EQ((int)tris, 1);
}

TEST(Glb_with_short_header_length_and_trailing_bytes_still_opens) {
    // Обратная беда: длина МЕНЬШЕ файла, а за концом лежит хвост выравнивания.
    const std::string path = Write("sage_test_tail.glb", MakeGlb(-8, true, std::string(8, '\0')));
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    CHECK_EQ((int)tris, 1);
}

TEST(Glb_with_unaligned_json_chunk_still_opens) {
    // Кусок JSON не добит до четырёх байт — спецификацию файл нарушает, но
    // читается он без потерь, и отказываться от модели из-за этого незачем.
    const std::string path = Write("sage_test_unaligned.glb", MakeGlb(0, false));
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    CHECK_EQ((int)tris, 1);
}

TEST(Glb_truncated_file_says_so_with_numbers) {
    // А вот тут данных ДЕЙСТВИТЕЛЬНО нет. «Починить» это молча значит отдать
    // движку геометрию с дырой — ответ обязан назвать причину.
    std::string data = MakeGlb();
    data.resize(data.size() - 20);
    const std::string path = Write("sage_test_cut.glb", data);
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    CHECK_FALSE(ok);
    CHECK_TRUE(err.find("обрезан") != std::string::npos);
}

TEST(Text_gltf_named_glb_opens_anyway) {
    // Формат — по содержимому, а не по расширению: текстовый glTF, сохранённый
    // с именем .glb, уходил в двоичный разбор и не открывался никогда.
    std::string json = TriangleJson();
    // Данные буфера — прямо в файле, чтобы проверка не зависела от спутников.
    const std::string bin = TriangleBin();
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < bin.size(); i += 3) {
        const unsigned a = (unsigned char)bin[i];
        const unsigned b = i + 1 < bin.size() ? (unsigned char)bin[i + 1] : 0;
        const unsigned c = i + 2 < bin.size() ? (unsigned char)bin[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        b64 += kB64[(v >> 18) & 63];
        b64 += kB64[(v >> 12) & 63];
        b64 += i + 1 < bin.size() ? kB64[(v >> 6) & 63] : '=';
        b64 += i + 2 < bin.size() ? kB64[v & 63] : '=';
    }
    const std::string uri = "data:application/octet-stream;base64," + b64;
    const std::string needle = R"("buffers":[{"byteLength":42}])";
    json.replace(json.find(needle), needle.size(),
                 R"("buffers":[{"byteLength":42,"uri":")" + uri + R"("}])");

    const std::string path = Write("sage_test_text.glb", json);
    std::string err;
    size_t tris = 0;
    const bool ok = Load(path, err, tris);
    std::remove(path.c_str());
    if (!ok) std::printf("       %s\n", err.c_str());
    CHECK_TRUE(ok);
    CHECK_EQ((int)tris, 1);
}

TEST(Glb_repack_reports_what_it_fixed) {
    // Починка не молчит: в журнале должно остаться, ЧТО было не так с файлом —
    // иначе битый экспорт живёт вечно.
    std::string out, err, warn;
    CHECK_TRUE(RepackGlb(MakeGlb(+64), out, err, warn));
    CHECK_TRUE(warn.find("заголовок GLB") != std::string::npos);

    std::string err2, warn2, out2;
    CHECK_FALSE(RepackGlb("not a model at all", out2, err2, warn2));
    CHECK_TRUE(err2.find("не GLB") != std::string::npos);
}
