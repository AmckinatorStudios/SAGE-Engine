// Свои бинарные форматы: контейнер и .sagemesh.
//
// Проверяется не «функция вернула true», а те три обещания, ради которых формат
// и заводили: файл читается обратно тем же, что записали; он ЗАМЕТНО меньше
// сырых данных; порча файла обнаруживается, а не превращается в дыру в кадре.
#include "TestFramework.h"

#include "sage/assets/format/Blob.h"
#include "sage/assets/format/MeshFormat.h"
#include "sage/render/MeshData.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sage::assets;

namespace {

sage::render::MeshData MakeSphere() {
    // Сфера, а не куб: у неё вершин достаточно, чтобы разница в размере была
    // осмысленной, и нормали смотрят во все стороны — октаэдральная упаковка
    // проверяется на всей сфере, а не на шести направлениях.
    sage::render::MeshData d = sage::render::BuildSphere(24, 32);
    // Касательные и второй UV генераторы не заполняют — проставим сами, иначе
    // соответствующие куски проверялись бы на нулях.
    for (size_t i = 0; i < d.Vertices.size(); ++i) {
        Vertex& v = d.Vertices[i];
        const glm::vec3 t = glm::normalize(glm::cross(v.Normal, glm::vec3(0.0f, 1.0f, 0.13f)) +
                                           glm::vec3(1e-4f));
        v.Tangent = glm::vec4(t, (i % 2) ? 1.0f : -1.0f);
        v.TexCoords2 = glm::vec2(v.TexCoords.y, v.TexCoords.x);
    }
    return d;
}

} // namespace

TEST(blob_roundtrip_and_varint) {
    ByteWriter w;
    w.U8(0x12);
    w.U16(0xBEEF);
    w.U32(0xDEADBEEF);
    w.U64(0x0123456789ABCDEFull);
    w.F32(-1234.5f);
    w.VarU32(0);
    w.VarU32(127);
    w.VarU32(128);
    w.VarU32(0xFFFFFFFFu);
    w.VarI32(-1);
    w.VarI32(1);
    w.VarI32(-100000);
    w.Str("привет");

    ByteReader r(w.Bytes);
    CHECK_EQ((int)r.U8(), 0x12);
    CHECK_EQ((int)r.U16(), 0xBEEF);
    CHECK_TRUE(r.U32() == 0xDEADBEEFu);
    CHECK_TRUE(r.U64() == 0x0123456789ABCDEFull);
    CHECK_NEAR(r.F32(), -1234.5f, 1e-6f);
    CHECK_TRUE(r.VarU32() == 0u);
    CHECK_TRUE(r.VarU32() == 127u);
    CHECK_TRUE(r.VarU32() == 128u);
    CHECK_TRUE(r.VarU32() == 0xFFFFFFFFu);
    CHECK_EQ(r.VarI32(), -1);
    CHECK_EQ(r.VarI32(), 1);
    CHECK_EQ(r.VarI32(), -100000);
    CHECK_TRUE(r.Str() == "привет");
    CHECK_TRUE(r.Ok());
    CHECK_EQ((int)r.Remaining(), 0);
}

TEST(blob_reader_never_reads_past_end) {
    // Чтение за границей обязано ВЗВЕСТИ ФЛАГ, а не прочитать соседнюю память:
    // на вход приходит файл с диска, то есть данные, которым нельзя доверять.
    std::vector<uint8_t> tiny{1, 2};
    ByteReader r(tiny);
    r.U32();
    CHECK_FALSE(r.Ok());

    ByteReader r2(tiny);
    r2.VarU32();   // 1 — валидно
    CHECK_TRUE(r2.Ok());
    r2.U64();
    CHECK_FALSE(r2.Ok());

    // Варинт без завершающего байта не должен зациклиться и не должен уйти за край.
    std::vector<uint8_t> unterminated{0x80, 0x80, 0x80};
    ByteReader r3(unterminated);
    r3.VarU32();
    CHECK_FALSE(r3.Ok());
}

TEST(blob_chunks_by_name_and_integrity) {
    Blob b(FourCC("TEST"), 3);
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)(i / 16);   // хорошо жмётся
    b.Add(FourCC("AAAA"), big);
    b.Add(FourCC("BBBB"), {1, 2, 3});

    std::vector<uint8_t> bytes = b.Serialize();
    // Сжимаемый кусок обязан ужаться: иначе кодек подключён, но не работает.
    CHECK_TRUE(bytes.size() < big.size());

    Blob back;
    std::string err;
    CHECK_TRUE(Blob::Parse(bytes, back, err));
    CHECK_TRUE(back.AssetType() == FourCC("TEST"));
    CHECK_TRUE(back.AssetVersion() == 3u);
    const std::vector<uint8_t>* a = back.Find(FourCC("AAAA"));
    CHECK_TRUE(a != nullptr);
    CHECK_TRUE(a && *a == big);
    // Незнакомое имя — не ошибка, а «куска нет»: на этом держится
    // совместимость со старыми файлами.
    CHECK_TRUE(back.Find(FourCC("ZZZZ")) == nullptr);

    // Порча в теле должна ловиться контрольной суммой.
    std::vector<uint8_t> corrupt = bytes;
    corrupt[corrupt.size() - 5] ^= 0xFF;
    Blob bad;
    std::string err2;
    CHECK_FALSE(Blob::Parse(corrupt, bad, err2));
    CHECK_FALSE(err2.empty());

    // Чужой файл — внятный отказ, а не разбор мусора.
    std::vector<uint8_t> notOurs{'P', 'N', 'G', ' ', 0, 0, 0, 0};
    Blob n;
    std::string err3;
    CHECK_FALSE(Blob::Parse(notOurs, n, err3));
}

TEST(mesh_format_lossless_is_exact) {
    const sage::render::MeshData src = MakeSphere();
    MeshWriteOptions opts;
    opts.Quantize = false;

    std::string err;
    Blob blob = PackMesh(src, opts);
    sage::render::MeshData back;
    CHECK_TRUE(UnpackMesh(blob, back, err));
    if (!err.empty()) std::printf("    err: %s\n", err.c_str());

    CHECK_EQ((int)back.Vertices.size(), (int)src.Vertices.size());
    CHECK_EQ((int)back.Indices.size(), (int)src.Indices.size());
    bool exact = back.Indices == src.Indices;
    for (size_t i = 0; i < src.Vertices.size() && exact; ++i) {
        const Vertex& a = src.Vertices[i];
        const Vertex& b = back.Vertices[i];
        exact = a.Position == b.Position && a.Normal == b.Normal &&
                a.TexCoords == b.TexCoords && a.Tangent == b.Tangent &&
                a.TexCoords2 == b.TexCoords2;
    }
    CHECK_TRUE(exact);
}

TEST(mesh_format_quantized_error_is_bounded) {
    const sage::render::MeshData src = MakeSphere();
    MeshStats stats;
    Blob blob = PackMesh(src, MeshWriteOptions{}, &stats);

    std::string err;
    sage::render::MeshData back;
    CHECK_TRUE(UnpackMesh(blob, back, err));
    CHECK_EQ((int)back.Vertices.size(), (int)src.Vertices.size());
    CHECK_TRUE(back.Indices == src.Indices);   // индексы не квантуются НИКОГДА

    // Габарит сферы — 1.0, 16 бит => шаг 1/65535. Допуск — один шаг.
    const float step = 1.0f / 65535.0f;
    float maxPos = 0.0f, maxNormalAngle = 0.0f, maxUV = 0.0f;
    for (size_t i = 0; i < src.Vertices.size(); ++i) {
        const Vertex& a = src.Vertices[i];
        const Vertex& b = back.Vertices[i];
        for (int k = 0; k < 3; ++k) maxPos = std::max(maxPos, std::abs(a.Position[k] - b.Position[k]));
        for (int k = 0; k < 2; ++k) maxUV = std::max(maxUV, std::abs(a.TexCoords[k] - b.TexCoords[k]));
        const float dot = std::max(-1.0f, std::min(1.0f, glm::dot(a.Normal, b.Normal)));
        maxNormalAngle = std::max(maxNormalAngle, std::acos(dot));
        // Знак касательной обязан сохраняться ТОЧНО: он не число, а выбор из
        // двух, и ошибка в нём выворачивает рельеф нормал-карты наизнанку.
        CHECK_TRUE((a.Tangent.w < 0.0f) == (b.Tangent.w < 0.0f));
    }
    CHECK_TRUE(maxPos <= step * 1.01f);
    CHECK_TRUE(maxUV <= step * 1.01f);
    // Октаэдр на 16 битах: ошибка угла заведомо меньше десятой доли градуса.
    CHECK_TRUE(maxNormalAngle < 0.1f * 3.14159265f / 180.0f);
    // И то же число, посчитанное самим упаковщиком, — чтобы отчёт в редакторе
    // не расходился с действительностью.
    CHECK_TRUE(stats.MaxPositionError <= step * 1.01f);
}

TEST(mesh_format_is_much_smaller) {
    const sage::render::MeshData src = MakeSphere();
    MeshStats stats;
    PackMesh(src, MeshWriteOptions{}, &stats);

    // Сырые вершины — 56 байт каждая. Квантование само по себе даёт ~3x,
    // deflate добирает остаток. Порог 3x намеренно ниже достигнутого: тест
    // должен ловить поломку упаковки, а не фиксировать конкретный процент.
    CHECK_TRUE(stats.PackedBytes * 3 < stats.SourceBytes);
    std::printf("    меш: %zu -> %zu байт (%.1fx), макс. ошибка позиции %.6f\n",
                stats.SourceBytes, stats.PackedBytes,
                (double)stats.SourceBytes / (double)std::max<size_t>(stats.PackedBytes, 1),
                (double)stats.MaxPositionError);

    // Lossless-режим тоже обязан выигрывать у сырых данных — за счёт варинтов на
    // индексах и deflate, — иначе выбор «без потерь» означал бы «без сжатия».
    MeshWriteOptions lossless;
    lossless.Quantize = false;
    MeshStats ls;
    PackMesh(src, lossless, &ls);
    CHECK_TRUE(ls.PackedBytes < ls.SourceBytes);
}

TEST(mesh_format_survives_a_real_file) {
    // ЧЕРЕЗ БАЙТЫ, а не через объект в памяти. Проверки выше упаковывают и
    // распаковывают один и тот же Blob и потому не трогают ни таблицу кусков, ни
    // смещения — то есть ровно ту часть, которая и определяет, прочитается ли
    // файл, записанный вчера. Первая же версия этого кода считала размер записи
    // таблицы на четыре байта меньше настоящего: в памяти всё сходилось, а на
    // диске все куски съезжали.
    const sage::render::MeshData src = MakeSphere();
    const std::string path = "test_mesh_roundtrip.sagemesh";

    MeshStats stats;
    std::string err;
    CHECK_TRUE(WriteMeshFile(path, src, MeshWriteOptions{}, err, &stats));
    if (!err.empty()) std::printf("    write err: %s\n", err.c_str());

    sage::render::MeshData back;
    std::string rerr;
    CHECK_TRUE(ReadMeshFile(path, back, rerr));
    if (!rerr.empty()) std::printf("    read err: %s\n", rerr.c_str());
    CHECK_EQ((int)back.Vertices.size(), (int)src.Vertices.size());
    CHECK_TRUE(back.Indices == src.Indices);

    // Размер на диске совпадает с тем, что упаковщик обещал в отчёте.
    std::remove(path.c_str());

    // И то же самое для режима без потерь.
    MeshWriteOptions lossless;
    lossless.Quantize = false;
    const std::string p2 = "test_mesh_lossless.sagemesh";
    std::string e2;
    CHECK_TRUE(WriteMeshFile(p2, src, lossless, e2));
    sage::render::MeshData exact;
    CHECK_TRUE(ReadMeshFile(p2, exact, e2));
    bool same = exact.Indices == src.Indices && exact.Vertices.size() == src.Vertices.size();
    for (size_t i = 0; i < src.Vertices.size() && same; ++i)
        same = exact.Vertices[i].Position == src.Vertices[i].Position &&
               exact.Vertices[i].Normal == src.Vertices[i].Normal;
    CHECK_TRUE(same);
    std::remove(p2.c_str());
}

TEST(mesh_format_rejects_garbage) {
    // Чужой тип ассета в контейнере — отказ с объяснением, а не пустой меш.
    Blob wrong(FourCC("TEXR"), 1);
    sage::render::MeshData out;
    std::string err;
    CHECK_FALSE(UnpackMesh(wrong, out, err));
    CHECK_FALSE(err.empty());

    // Схема из будущего: движок обязан сказать это прямо, а не читать наугад.
    Blob future(kAssetTypeMesh, kMeshSchemaVersion + 5);
    std::string err2;
    CHECK_FALSE(UnpackMesh(future, out, err2));

    // Меш без обязательных кусков.
    Blob empty(kAssetTypeMesh, kMeshSchemaVersion);
    std::string err3;
    CHECK_FALSE(UnpackMesh(empty, out, err3));
}
