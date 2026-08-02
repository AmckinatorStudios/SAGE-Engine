#include "sage/assets/format/MeshFormat.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sage::assets {

namespace {

constexpr uint32_t kChunkInfo = FourCC("INFO");
constexpr uint32_t kChunkPos  = FourCC("POSI");
constexpr uint32_t kChunkNorm = FourCC("NORM");
constexpr uint32_t kChunkUV0  = FourCC("TEX0");
constexpr uint32_t kChunkUV1  = FourCC("TEX1");
constexpr uint32_t kChunkTang = FourCC("TANG");
constexpr uint32_t kChunkIdx  = FourCC("INDX");

// Флаги в INFO.
enum InfoFlags : uint32_t {
    FlagQuantized = 1u << 0,
};

// --- Октаэдральная упаковка нормали -----------------------------------------
//
// Единичный вектор имеет две степени свободы, а хранится тремя числами. Проекция
// на октаэдр раскладывает сферу на квадрат без швов и с почти равномерной
// плотностью: при 16 битах на компоненту ошибка угла — доли градуса, что для
// освещения неотличимо. Простое «выкинуть Z и восстановить по длине» так не
// умеет: у него теряется знак и рвётся точность у экватора.
void EncodeOct(const glm::vec3& n, int16_t& outX, int16_t& outY) {
    const float len = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (len < 1e-20f) { outX = 0; outY = 0; return; }
    float px = n.x / len;
    float py = n.y / len;
    if (n.z < 0.0f) {
        const float sx = px >= 0.0f ? 1.0f : -1.0f;
        const float sy = py >= 0.0f ? 1.0f : -1.0f;
        const float ox = (1.0f - std::abs(py)) * sx;
        const float oy = (1.0f - std::abs(px)) * sy;
        px = ox;
        py = oy;
    }
    auto q = [](float v) {
        v = std::max(-1.0f, std::min(1.0f, v));
        return (int16_t)std::lround(v * 32767.0f);
    };
    outX = q(px);
    outY = q(py);
}

glm::vec3 DecodeOct(int16_t ix, int16_t iy) {
    float px = (float)ix / 32767.0f;
    float py = (float)iy / 32767.0f;
    glm::vec3 n(px, py, 1.0f - std::abs(px) - std::abs(py));
    if (n.z < 0.0f) {
        const float sx = n.x >= 0.0f ? 1.0f : -1.0f;
        const float sy = n.y >= 0.0f ? 1.0f : -1.0f;
        const float ox = (1.0f - std::abs(n.y)) * sx;
        const float oy = (1.0f - std::abs(n.x)) * sy;
        n.x = ox;
        n.y = oy;
    }
    const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return l > 1e-20f ? n / l : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Квантование в отрезок [lo, hi] на bits бит. Вырожденный отрезок (плоская
// модель, все UV в одной точке) даёт нулевой шаг — тогда пишем нули и при
// распаковке возвращаем lo, иначе получилось бы деление на ноль.
struct Quantizer {
    float Lo = 0.0f, Scale = 0.0f, Inv = 0.0f;
    uint32_t Max = 0;

    void Init(float lo, float hi, int bits) {
        Lo = lo;
        Max = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        const float span = hi - lo;
        Scale = span > 0.0f ? span / (float)Max : 0.0f;
        Inv = Scale > 0.0f ? 1.0f / Scale : 0.0f;
    }
    uint32_t Encode(float v) const {
        if (Inv == 0.0f) return 0;
        const float t = (v - Lo) * Inv;
        const long r = std::lround(t);
        if (r < 0) return 0;
        return (uint32_t)std::min<long>(r, (long)Max);
    }
    float Decode(uint32_t q) const { return Lo + (float)q * Scale; }
};

} // namespace

Blob PackMesh(const sage::render::MeshData& mesh, const MeshWriteOptions& opts, MeshStats* stats) {
    Blob blob(kAssetTypeMesh, kMeshSchemaVersion);

    const size_t vcount = mesh.Vertices.size();
    const size_t icount = mesh.Indices.size();
    const int posBits = std::max(8, std::min(16, opts.PositionBits));

    // Габариты позиций и UV — база для квантования и заодно полезные метаданные:
    // по ним считается отсечение и подгонка камеры, и пересчитывать их при
    // загрузке незачем, раз они уже известны здесь.
    glm::vec3 lo(0.0f), hi(0.0f);
    glm::vec2 uvLo(0.0f), uvHi(0.0f), uv2Lo(0.0f), uv2Hi(0.0f);
    if (vcount > 0) {
        lo = hi = mesh.Vertices[0].Position;
        uvLo = uvHi = mesh.Vertices[0].TexCoords;
        uv2Lo = uv2Hi = mesh.Vertices[0].TexCoords2;
        for (const Vertex& v : mesh.Vertices) {
            lo = glm::min(lo, v.Position);
            hi = glm::max(hi, v.Position);
            uvLo = glm::min(uvLo, v.TexCoords);
            uvHi = glm::max(uvHi, v.TexCoords);
            uv2Lo = glm::min(uv2Lo, v.TexCoords2);
            uv2Hi = glm::max(uv2Hi, v.TexCoords2);
        }
    }

    const bool hasTangents = opts.WriteTangents && vcount > 0;
    const bool hasUV2 = opts.WriteLightmapUV && vcount > 0;

    {
        ByteWriter w;
        w.U32(opts.Quantize ? FlagQuantized : 0u);
        w.U32((uint32_t)vcount);
        w.U32((uint32_t)icount);
        w.U8((uint8_t)posBits);
        w.U8(hasTangents ? 1 : 0);
        w.U8(hasUV2 ? 1 : 0);
        w.U8(0);   // выравнивание/резерв
        for (int i = 0; i < 3; ++i) w.F32(lo[i]);
        for (int i = 0; i < 3; ++i) w.F32(hi[i]);
        for (int i = 0; i < 2; ++i) w.F32(uvLo[i]);
        for (int i = 0; i < 2; ++i) w.F32(uvHi[i]);
        for (int i = 0; i < 2; ++i) w.F32(uv2Lo[i]);
        for (int i = 0; i < 2; ++i) w.F32(uv2Hi[i]);
        blob.Add(kChunkInfo, std::move(w.Bytes), Codec::Raw);   // десятки байт, сжимать нечего
    }

    float maxErr = 0.0f;

    // --- Позиции ---
    {
        ByteWriter w;
        if (opts.Quantize) {
            Quantizer qx, qy, qz;
            qx.Init(lo.x, hi.x, posBits);
            qy.Init(lo.y, hi.y, posBits);
            qz.Init(lo.z, hi.z, posBits);
            for (const Vertex& v : mesh.Vertices) {
                const uint32_t ex = qx.Encode(v.Position.x);
                const uint32_t ey = qy.Encode(v.Position.y);
                const uint32_t ez = qz.Encode(v.Position.z);
                w.U16((uint16_t)ex);
                w.U16((uint16_t)ey);
                w.U16((uint16_t)ez);
                maxErr = std::max(maxErr, std::abs(qx.Decode(ex) - v.Position.x));
                maxErr = std::max(maxErr, std::abs(qy.Decode(ey) - v.Position.y));
                maxErr = std::max(maxErr, std::abs(qz.Decode(ez) - v.Position.z));
            }
        } else {
            for (const Vertex& v : mesh.Vertices) {
                w.F32(v.Position.x);
                w.F32(v.Position.y);
                w.F32(v.Position.z);
            }
        }
        blob.Add(kChunkPos, std::move(w.Bytes));
    }

    // --- Нормали ---
    {
        ByteWriter w;
        for (const Vertex& v : mesh.Vertices) {
            if (opts.Quantize) {
                int16_t a, b;
                EncodeOct(v.Normal, a, b);
                w.I16(a);
                w.I16(b);
            } else {
                w.F32(v.Normal.x);
                w.F32(v.Normal.y);
                w.F32(v.Normal.z);
            }
        }
        blob.Add(kChunkNorm, std::move(w.Bytes));
    }

    // --- UV ---
    auto writeUV = [&](uint32_t id, const glm::vec2& l, const glm::vec2& h, bool second) {
        ByteWriter w;
        Quantizer qu, qv;
        qu.Init(l.x, h.x, 16);
        qv.Init(l.y, h.y, 16);
        for (const Vertex& vx : mesh.Vertices) {
            const glm::vec2 t = second ? vx.TexCoords2 : vx.TexCoords;
            if (opts.Quantize) {
                w.U16((uint16_t)qu.Encode(t.x));
                w.U16((uint16_t)qv.Encode(t.y));
            } else {
                w.F32(t.x);
                w.F32(t.y);
            }
        }
        blob.Add(id, std::move(w.Bytes));
    };
    writeUV(kChunkUV0, uvLo, uvHi, false);
    if (hasUV2) writeUV(kChunkUV1, uv2Lo, uv2Hi, true);

    // --- Касательные ---
    if (hasTangents) {
        ByteWriter w;
        for (const Vertex& v : mesh.Vertices) {
            if (opts.Quantize) {
                int16_t a, b;
                EncodeOct(glm::vec3(v.Tangent), a, b);
                w.I16(a);
                w.I16(b);
                w.U8(v.Tangent.w < 0.0f ? 0 : 1);
            } else {
                w.F32(v.Tangent.x);
                w.F32(v.Tangent.y);
                w.F32(v.Tangent.z);
                w.F32(v.Tangent.w);
            }
        }
        blob.Add(kChunkTang, std::move(w.Bytes));
    }

    // --- Индексы: дельта к предыдущему + переменная длина ---
    {
        ByteWriter w;
        int32_t prev = 0;
        for (unsigned int i : mesh.Indices) {
            w.VarI32((int32_t)i - prev);
            prev = (int32_t)i;
        }
        blob.Add(kChunkIdx, std::move(w.Bytes));
    }

    if (stats) {
        stats->SourceBytes = vcount * sizeof(Vertex) + icount * sizeof(unsigned int);
        stats->PackedBytes = blob.Serialize().size();
        stats->MaxPositionError = opts.Quantize ? maxErr : 0.0f;
    }
    return blob;
}

bool UnpackMesh(const Blob& blob, sage::render::MeshData& out, std::string& err) {
    if (blob.AssetType() != kAssetTypeMesh) {
        err = "это не меш (тип " + FourCCToString(blob.AssetType()) + ")";
        return false;
    }
    if (blob.AssetVersion() > kMeshSchemaVersion) {
        err = "схема меша новее, чем понимает движок (v" + std::to_string(blob.AssetVersion()) + ")";
        return false;
    }
    const std::vector<uint8_t>* info = blob.Find(kChunkInfo);
    if (!info) { err = "нет куска INFO"; return false; }

    ByteReader ir(*info);
    const uint32_t flags = ir.U32();
    const uint32_t vcount = ir.U32();
    const uint32_t icount = ir.U32();
    const int posBits = (int)ir.U8();
    const bool hasTangents = ir.U8() != 0;
    const bool hasUV2 = ir.U8() != 0;
    ir.U8();
    glm::vec3 lo, hi;
    glm::vec2 uvLo, uvHi, uv2Lo, uv2Hi;
    for (int i = 0; i < 3; ++i) lo[i] = ir.F32();
    for (int i = 0; i < 3; ++i) hi[i] = ir.F32();
    for (int i = 0; i < 2; ++i) uvLo[i] = ir.F32();
    for (int i = 0; i < 2; ++i) uvHi[i] = ir.F32();
    for (int i = 0; i < 2; ++i) uv2Lo[i] = ir.F32();
    for (int i = 0; i < 2; ++i) uv2Hi[i] = ir.F32();
    if (!ir.Ok()) { err = "кусок INFO обрывается"; return false; }

    const bool quantized = (flags & FlagQuantized) != 0;

    const std::vector<uint8_t>* posc = blob.Find(kChunkPos);
    const std::vector<uint8_t>* normc = blob.Find(kChunkNorm);
    const std::vector<uint8_t>* uvc = blob.Find(kChunkUV0);
    const std::vector<uint8_t>* idxc = blob.Find(kChunkIdx);
    if (!posc || !idxc) { err = "нет обязательных кусков (POSI/INDX)"; return false; }

    out.Vertices.assign(vcount, Vertex{});
    out.Indices.clear();
    out.Indices.reserve(icount);

    {
        ByteReader r(*posc);
        Quantizer qx, qy, qz;
        qx.Init(lo.x, hi.x, posBits);
        qy.Init(lo.y, hi.y, posBits);
        qz.Init(lo.z, hi.z, posBits);
        for (uint32_t i = 0; i < vcount; ++i) {
            if (quantized) {
                const uint32_t a = r.U16(), b = r.U16(), c = r.U16();
                out.Vertices[i].Position = {qx.Decode(a), qy.Decode(b), qz.Decode(c)};
            } else {
                out.Vertices[i].Position = {r.F32(), r.F32(), r.F32()};
            }
        }
        if (!r.Ok()) { err = "кусок POSI обрывается"; return false; }
    }

    if (normc) {
        ByteReader r(*normc);
        for (uint32_t i = 0; i < vcount; ++i) {
            if (quantized) {
                const int16_t a = r.I16(), b = r.I16();
                out.Vertices[i].Normal = DecodeOct(a, b);
            } else {
                out.Vertices[i].Normal = {r.F32(), r.F32(), r.F32()};
            }
        }
        if (!r.Ok()) { err = "кусок NORM обрывается"; return false; }
    }

    auto readUV = [&](const std::vector<uint8_t>* chunk, const glm::vec2& l, const glm::vec2& h,
                      bool second) -> bool {
        if (!chunk) return true;
        ByteReader r(*chunk);
        Quantizer qu, qv;
        qu.Init(l.x, h.x, 16);
        qv.Init(l.y, h.y, 16);
        for (uint32_t i = 0; i < vcount; ++i) {
            glm::vec2 t;
            if (quantized) {
                const uint32_t a = r.U16(), b = r.U16();
                t = {qu.Decode(a), qv.Decode(b)};
            } else {
                t = {r.F32(), r.F32()};
            }
            if (second) out.Vertices[i].TexCoords2 = t;
            else out.Vertices[i].TexCoords = t;
        }
        return r.Ok();
    };
    if (!readUV(uvc, uvLo, uvHi, false)) { err = "кусок TEX0 обрывается"; return false; }
    if (hasUV2 && !readUV(blob.Find(kChunkUV1), uv2Lo, uv2Hi, true)) {
        err = "кусок TEX1 обрывается";
        return false;
    }

    if (hasTangents) {
        if (const std::vector<uint8_t>* tc = blob.Find(kChunkTang)) {
            ByteReader r(*tc);
            for (uint32_t i = 0; i < vcount; ++i) {
                if (quantized) {
                    const int16_t a = r.I16(), b = r.I16();
                    const glm::vec3 t = DecodeOct(a, b);
                    const float sign = r.U8() ? 1.0f : -1.0f;
                    out.Vertices[i].Tangent = glm::vec4(t, sign);
                } else {
                    out.Vertices[i].Tangent = {r.F32(), r.F32(), r.F32(), r.F32()};
                }
            }
            if (!r.Ok()) { err = "кусок TANG обрывается"; return false; }
        }
    }

    {
        ByteReader r(*idxc);
        int32_t prev = 0;
        for (uint32_t i = 0; i < icount; ++i) {
            prev += r.VarI32();
            if (prev < 0 || (uint32_t)prev >= vcount) {
                err = "индекс " + std::to_string(prev) + " вне диапазона вершин";
                return false;
            }
            out.Indices.push_back((unsigned int)prev);
        }
        if (!r.Ok()) { err = "кусок INDX обрывается"; return false; }
    }
    return true;
}

bool WriteMeshFile(const std::string& path, const sage::render::MeshData& mesh,
                   const MeshWriteOptions& opts, std::string& err, MeshStats* stats) {
    const Blob blob = PackMesh(mesh, opts, stats);
    return blob.Write(path, err);
}

bool ReadMeshFile(const std::string& path, sage::render::MeshData& out, std::string& err) {
    Blob blob;
    if (!Blob::Read(path, blob, err)) return false;
    return UnpackMesh(blob, out, err);
}

} // namespace sage::assets
