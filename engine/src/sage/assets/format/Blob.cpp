#include "sage/assets/format/Blob.h"

#include <cstring>
#include <fstream>

#include <miniz.h>

namespace sage::assets {

namespace {
constexpr uint32_t kMagic = FourCC("SAGE");
constexpr uint32_t kContainerVersion = 1;
constexpr size_t kHeaderSize = 24;   // magic+ver+type+assetver+count+flags (6 x u32)
// id(4) + codec(4) + offset(8) + stored(8) + raw(8) + hash(4+4)
constexpr size_t kChunkEntrySize = 40;
} // namespace

std::string FourCCToString(uint32_t cc) {
    char s[5] = {(char)(cc & 0xFF), (char)((cc >> 8) & 0xFF), (char)((cc >> 16) & 0xFF),
                 (char)((cc >> 24) & 0xFF), 0};
    for (int i = 0; i < 4; ++i)
        if (s[i] < 32 || s[i] > 126) s[i] = '?';
    return std::string(s);
}

// --- ByteWriter --------------------------------------------------------------

void ByteWriter::U16(uint16_t v) {
    Bytes.push_back((uint8_t)(v & 0xFF));
    Bytes.push_back((uint8_t)(v >> 8));
}
void ByteWriter::U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) Bytes.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void ByteWriter::U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) Bytes.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void ByteWriter::F32(float v) {
    // Через memcpy, а не через union/reinterpret_cast: это единственный способ
    // переложить биты без нарушения строгого алиасинга.
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    U32(bits);
}
void ByteWriter::Raw(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    Bytes.insert(Bytes.end(), p, p + size);
}
void ByteWriter::VarU32(uint32_t v) {
    while (v >= 0x80) {
        Bytes.push_back((uint8_t)(v | 0x80));
        v >>= 7;
    }
    Bytes.push_back((uint8_t)v);
}
void ByteWriter::VarI32(int32_t v) {
    // Зигзаг: -1,1,-2,2 -> 1,2,3,4. Без него небольшое отрицательное число
    // выглядит как огромное беззнаковое и занимает пять байт вместо одного.
    // Сдвиг влево делается в БЕЗЗНАКОВОМ типе: «v << 1» для отрицательного v —
    // неопределённое поведение, и санитайзер ловит это на первом же
    // отрицательном числе. Арифметический сдвиг вправо на 31 при этом остаётся
    // знаковым — он и даёт маску из одних единиц для отрицательных, на которой
    // держится вся зигзаг-упаковка.
    VarU32(((uint32_t)v << 1) ^ (uint32_t)(v >> 31));
}
void ByteWriter::Str(const std::string& s) {
    VarU32((uint32_t)s.size());
    Raw(s.data(), s.size());
}

// --- ByteReader --------------------------------------------------------------

uint8_t ByteReader::U8() {
    if (Pos + 1 > Size) { Bad = true; return 0; }
    return Data[Pos++];
}
uint16_t ByteReader::U16() {
    if (Pos + 2 > Size) { Bad = true; Pos = Size; return 0; }
    uint16_t v = (uint16_t)Data[Pos] | ((uint16_t)Data[Pos + 1] << 8);
    Pos += 2;
    return v;
}
uint32_t ByteReader::U32() {
    if (Pos + 4 > Size) { Bad = true; Pos = Size; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)Data[Pos + i] << (i * 8);
    Pos += 4;
    return v;
}
uint64_t ByteReader::U64() {
    if (Pos + 8 > Size) { Bad = true; Pos = Size; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)Data[Pos + i] << (i * 8);
    Pos += 8;
    return v;
}
float ByteReader::F32() {
    uint32_t bits = U32();
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    return v;
}
void ByteReader::Raw(void* out, size_t size) {
    if (Pos + size > Size) { Bad = true; Pos = Size; return; }
    std::memcpy(out, Data + Pos, size);
    Pos += size;
}
uint32_t ByteReader::VarU32() {
    uint32_t v = 0;
    int shift = 0;
    for (;;) {
        if (Pos >= Size) { Bad = true; return 0; }
        const uint8_t b = Data[Pos++];
        v |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        // Пятый байт с продолжением означает мусор: 32 бита в семибитных
        // группах — это максимум пять байт.
        if (shift > 28) { Bad = true; return 0; }
    }
    return v;
}
int32_t ByteReader::VarI32() {
    const uint32_t u = VarU32();
    return (int32_t)((u >> 1) ^ (~(u & 1) + 1));
}
std::string ByteReader::Str() {
    const uint32_t n = VarU32();
    if (Bad || Pos + n > Size) { Bad = true; return {}; }
    std::string s((const char*)Data + Pos, n);
    Pos += n;
    return s;
}

// --- Сжатие ------------------------------------------------------------------

bool DeflateBytes(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int level) {
    if (in.empty()) { out.clear(); return true; }
    mz_ulong bound = mz_compressBound((mz_ulong)in.size());
    out.resize(bound);
    mz_ulong outLen = bound;
    const int rc = mz_compress2(out.data(), &outLen, in.data(), (mz_ulong)in.size(), level);
    if (rc != MZ_OK) { out.clear(); return false; }
    out.resize(outLen);
    return true;
}

bool InflateBytes(const std::vector<uint8_t>& in, size_t rawSize, std::vector<uint8_t>& out) {
    out.assign(rawSize, 0);
    if (rawSize == 0) return true;
    mz_ulong outLen = (mz_ulong)rawSize;
    const int rc = mz_uncompress(out.data(), &outLen, in.data(), (mz_ulong)in.size());
    if (rc != MZ_OK || outLen != rawSize) { out.clear(); return false; }
    return true;
}

uint64_t HashBytes(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// --- Blob --------------------------------------------------------------------

void Blob::Add(uint32_t id, std::vector<uint8_t> data, Codec preferred) {
    BlobChunk c;
    c.Id = id;
    c.Data = std::move(data);
    c.Preferred = preferred;
    m_chunks.push_back(std::move(c));
}

const std::vector<uint8_t>* Blob::Find(uint32_t id) const {
    for (const BlobChunk& c : m_chunks)
        if (c.Id == id) return &c.Data;
    return nullptr;
}

std::vector<uint8_t> Blob::Serialize() const {
    // Сначала готовим тела кусков (возможно, сжатые), потом заголовок: смещения
    // известны только когда известны итоговые размеры.
    struct Prepared {
        uint32_t Id;
        Codec Used;
        uint64_t RawSize;
        uint64_t Hash;
        std::vector<uint8_t> Stored;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(m_chunks.size());

    for (const BlobChunk& c : m_chunks) {
        Prepared p;
        p.Id = c.Id;
        p.RawSize = c.Data.size();
        p.Hash = HashBytes(c.Data.data(), c.Data.size());
        p.Used = Codec::Raw;
        p.Stored = c.Data;
        if (c.Preferred == Codec::Deflate && c.Data.size() >= 64) {
            std::vector<uint8_t> packed;
            // Сжимаем ТОЛЬКО если выиграли. Квантованные позиции и без того
            // плотные, deflate на них порой добавляет байт — платить за это
            // распаковкой при каждой загрузке незачем.
            if (DeflateBytes(c.Data, packed) && packed.size() < c.Data.size()) {
                p.Used = Codec::Deflate;
                p.Stored = std::move(packed);
            }
        }
        prepared.push_back(std::move(p));
    }

    ByteWriter w;
    w.U32(kMagic);
    w.U32(kContainerVersion);
    w.U32(m_assetType);
    w.U32(m_assetVersion);
    w.U32((uint32_t)prepared.size());
    w.U32(0);   // флаги, зарезервировано

    uint64_t offset = kHeaderSize + kChunkEntrySize * prepared.size();
    for (const Prepared& p : prepared) {
        w.U32(p.Id);
        w.U32((uint32_t)p.Used);
        w.U64(offset);
        w.U64((uint64_t)p.Stored.size());
        w.U64(p.RawSize);
        w.U32((uint32_t)(p.Hash & 0xFFFFFFFFu));
        w.U32((uint32_t)(p.Hash >> 32));
        offset += p.Stored.size();
    }
    for (const Prepared& p : prepared) w.Raw(p.Stored.data(), p.Stored.size());
    return std::move(w.Bytes);
}

bool Blob::Parse(const std::vector<uint8_t>& bytes, Blob& out, std::string& err) {
    ByteReader r(bytes);
    if (r.U32() != kMagic) {
        err = "не файл SAGE (неверная сигнатура)";
        return false;
    }
    const uint32_t containerVersion = r.U32();
    if (containerVersion > kContainerVersion) {
        err = "файл записан более новой версией движка (контейнер v" +
              std::to_string(containerVersion) + ", поддерживается до v" +
              std::to_string(kContainerVersion) + ")";
        return false;
    }
    out = Blob();
    out.m_assetType = r.U32();
    out.m_assetVersion = r.U32();
    const uint32_t count = r.U32();
    r.U32();   // флаги

    if (!r.Ok()) { err = "файл обрывается в заголовке"; return false; }
    // Защита от заведомо невозможного числа кусков: иначе испорченный счётчик
    // заставил бы зарезервировать гигабайты до первой же проверки границ.
    if ((uint64_t)count * kChunkEntrySize + kHeaderSize > bytes.size()) {
        err = "таблица кусков не помещается в файл (файл повреждён)";
        return false;
    }

    struct Entry {
        uint32_t Id, CodecId;
        uint64_t Offset, StoredSize, RawSize, Hash;
    };
    std::vector<Entry> entries(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry& e = entries[i];
        e.Id = r.U32();
        e.CodecId = r.U32();
        e.Offset = r.U64();
        e.StoredSize = r.U64();
        e.RawSize = r.U64();
        const uint32_t lo = r.U32();
        const uint32_t hi = r.U32();
        e.Hash = (uint64_t)lo | ((uint64_t)hi << 32);
    }
    if (!r.Ok()) { err = "файл обрывается в таблице кусков"; return false; }

    for (const Entry& e : entries) {
        if (e.Offset > bytes.size() || e.StoredSize > bytes.size() - e.Offset) {
            err = "кусок " + FourCCToString(e.Id) + " выходит за границы файла";
            return false;
        }
        std::vector<uint8_t> stored(bytes.begin() + (size_t)e.Offset,
                                    bytes.begin() + (size_t)(e.Offset + e.StoredSize));
        std::vector<uint8_t> raw;
        if ((Codec)e.CodecId == Codec::Deflate) {
            if (!InflateBytes(stored, (size_t)e.RawSize, raw)) {
                err = "кусок " + FourCCToString(e.Id) + " не распаковывается";
                return false;
            }
        } else if ((Codec)e.CodecId == Codec::Raw) {
            raw = std::move(stored);
        } else {
            err = "кусок " + FourCCToString(e.Id) + ": неизвестный кодек " +
                  std::to_string(e.CodecId);
            return false;
        }
        if (raw.size() != e.RawSize || HashBytes(raw.data(), raw.size()) != e.Hash) {
            err = "кусок " + FourCCToString(e.Id) + " не сходится с контрольной суммой";
            return false;
        }
        out.Add(e.Id, std::move(raw));
    }
    return true;
}

bool Blob::Write(const std::string& path, std::string& err) const {
    const std::vector<uint8_t> bytes = Serialize();
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "не открыть на запись: " + path; return false; }
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!f) { err = "ошибка записи: " + path; return false; }
    return true;
}

bool Blob::Read(const std::string& path, Blob& out, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { err = "не открыть: " + path; return false; }
    const std::streamsize size = f.tellg();
    if (size < 0) { err = "не прочитать размер: " + path; return false; }
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)size);
    if (size > 0) f.read((char*)bytes.data(), size);
    if (!f) { err = "ошибка чтения: " + path; return false; }
    return Parse(bytes, out, err);
}

} // namespace sage::assets
