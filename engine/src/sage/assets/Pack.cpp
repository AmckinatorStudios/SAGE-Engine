#include "sage/assets/Pack.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

#include "sage/assets/format/Blob.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>


namespace fs = std::filesystem;

namespace sage::assets {

namespace {

constexpr uint32_t kMagic = FourCC("SPAK");
constexpr uint32_t kVersionPlain = 1;     // старый формат: без шифрования
constexpr uint32_t kVersion = 2;          // защищённый
constexpr size_t kHeaderV1 = 24;
constexpr size_t kHeaderV2 = 56;
// Одноразовое число оглавления — вне диапазона номеров записей.
constexpr uint32_t kIndexNumber = 0xFFFFFFFFu;

// КЛЮЧ ДВИЖКА. Из него и соли пакета выводится ключ шифра. Держать его в
// одном месте, а не россыпью, — чтобы смена ключа была одной правкой.
constexpr uint8_t kEngineKey[32] = {
    0x53, 0x41, 0x47, 0x45, 0x9c, 0x2f, 0x71, 0xe4, 0x0b, 0xd3, 0x58, 0xa6, 0x3e, 0x87, 0x12, 0xf9,
    0x64, 0xc1, 0x2d, 0x9a, 0x7f, 0x05, 0xb8, 0x4e, 0xe3, 0x36, 0x90, 0x5b, 0xca, 0x17, 0x6d, 0xa2};

// Числа пишутся ПОБАЙТНО, little-endian, как и в Blob. Не memcpy структуры:
// раскладка зависит от компилятора, а пакет переезжает между машинами всегда —
// собран он на машине разработчика, а читается у игрока.
void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void PutString(std::vector<uint8_t>& out, const std::string& s) {
    PutU32(out, (uint32_t)s.size());
    out.insert(out.end(), s.begin(), s.end());
}

uint32_t GetU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

// --- ChaCha20 (RFC 8439) -----------------------------------------------------
//
// Свой, а не из библиотеки: зависимостей ради сорока строк движок не тащит, а
// алгоритм короткий, стандартный и проверяется вектором из RFC (см.
// tests/test_pack.cpp).
inline uint32_t Rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
inline void QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = Rotl(d, 16);
    c += d; b ^= c; b = Rotl(b, 12);
    a += b; d ^= a; d = Rotl(d, 8);
    c += d; b ^= c; b = Rotl(b, 7);
}

void ChaChaBlock(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t st[16];
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; ++i) st[4 + i] = GetU32(key + i * 4);
    st[12] = counter;
    for (int i = 0; i < 3; ++i) st[13 + i] = GetU32(nonce + i * 4);
    uint32_t x[16];
    std::memcpy(x, st, sizeof(st));
    for (int i = 0; i < 10; ++i) {
        QuarterRound(x[0], x[4], x[8], x[12]);
        QuarterRound(x[1], x[5], x[9], x[13]);
        QuarterRound(x[2], x[6], x[10], x[14]);
        QuarterRound(x[3], x[7], x[11], x[15]);
        QuarterRound(x[0], x[5], x[10], x[15]);
        QuarterRound(x[1], x[6], x[11], x[12]);
        QuarterRound(x[2], x[7], x[8], x[13]);
        QuarterRound(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = x[i] + st[i];
        out[i * 4 + 0] = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

// Шифрование и расшифровка — одна операция (XOR с потоком).
void ChaChaXor(const uint8_t key[32], const uint8_t nonce[12], uint8_t* data, size_t size) {
    uint8_t block[64];
    uint32_t counter = 1;
    for (size_t pos = 0; pos < size; pos += 64, ++counter) {
        ChaChaBlock(key, counter, nonce, block);
        const size_t n = std::min<size_t>(64, size - pos);
        for (size_t i = 0; i < n; ++i) data[pos + i] ^= block[i];
    }
}

// Одноразовое число записи: номер + вторая половина соли. У каждой записи своё
// — один и тот же поток на двух файлах выдал бы XOR их содержимого.
void NonceFor(const uint8_t salt[16], uint32_t number, uint8_t nonce[12]) {
    nonce[0] = (uint8_t)number;
    nonce[1] = (uint8_t)(number >> 8);
    nonce[2] = (uint8_t)(number >> 16);
    nonce[3] = (uint8_t)(number >> 24);
    std::memcpy(nonce + 4, salt + 8, 8);
}

// Ключ пакета = блок ChaCha20 ключа движка с солью: у каждого пакета свой.
void DeriveKey(const uint8_t salt[16], uint8_t key[32]) {
    uint8_t nonce[12];
    std::memcpy(nonce, salt, 12);
    uint8_t block[64];
    ChaChaBlock(kEngineKey, GetU32(salt + 12), nonce, block);
    std::memcpy(key, block, 32);
}

// Путь внутри пакета: UTF-8, прямой слэш, без ведущего "./" и без "a/../".
std::string Normalize(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) out += (c == '\\') ? '/' : c;
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    while (!out.empty() && out.front() == '/') out.erase(0, 1);
    // «assets/models/../tex/a.png» — так пишут ссылки внутри моделей (путь
    // текстуры относительно файла модели). Без схлопывания такой ключ не
    // совпал бы ни с одной записью пакета.
    if (out.find("..") != std::string::npos || out.find("/./") != std::string::npos) {
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos <= out.size()) {
            const size_t next = out.find('/', pos);
            const std::string part = out.substr(pos, next == std::string::npos ? std::string::npos
                                                                               : next - pos);
            if (part == "..") {
                if (!parts.empty() && parts.back() != "..") parts.pop_back();
                else parts.push_back(part);
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        out.clear();
        for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    }
    return out;
}

bool ReadWholeFile(const fs::path& file, std::vector<uint8_t>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out.resize((size_t)size);
    if (size > 0 && !in.read((char*)out.data(), size)) return false;
    return true;
}

void RandomSalt(uint8_t salt[16]) {
    std::random_device rd;
    const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (int i = 0; i < 16; ++i) salt[i] = (uint8_t)((rd() ^ (t >> ((i % 8) * 8))) & 0xFF);
}

} // namespace

// ============================================================================
//  PackWriter
// ============================================================================

void PackWriter::Add(const std::string& virtualPath, const std::vector<uint8_t>& data) {
    const std::string path = Normalize(virtualPath);
    if (path.empty()) return;
    // Повторное добавление того же пути ЗАМЕНЯЕТ прежнее содержимое. Два файла
    // с одним именем в пакете означали бы, что какой из них прочитают —
    // вопрос порядка обхода каталога, то есть случайность.
    for (Entry& e : m_entries) {
        if (e.Path == path) { e.Data = data; e.Disk.clear(); return; }
    }
    m_entries.push_back({path, data, {}});
}

void PackWriter::AddFile(const std::string& virtualPath, const fs::path& diskFile) {
    const std::string path = Normalize(virtualPath);
    if (path.empty()) return;
    for (Entry& e : m_entries) {
        if (e.Path == path) { e.Data.clear(); e.Disk = diskFile; return; }
    }
    m_entries.push_back({path, {}, diskFile});
}

bool PackWriter::Has(const std::string& virtualPath) const {
    const std::string path = Normalize(virtualPath);
    for (const Entry& e : m_entries)
        if (e.Path == path) return true;
    return false;
}

size_t PackWriter::AddDirectory(const fs::path& root, const std::vector<std::string>& skipSuffixes,
                                const std::string& prefix, const std::vector<fs::path>& skipDirs) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    // Исключённые каталоги — как пути ОТНОСИТЕЛЬНО root с '/' на конце: так
    // «Builds/» не заденет соседний «BuildsOld/».
    std::vector<std::string> skipRel;
    for (const fs::path& d : skipDirs) {
        const fs::path rel = fs::relative(fs::absolute(d, ec), fs::absolute(root, ec), ec);
        const std::string r = Normalize(sage::PathToUtf8(rel));
        if (!ec && !r.empty() && r.rfind("..", 0) != 0) skipRel.push_back(r + "/");
    }

    const std::string base = Normalize(prefix);
    size_t added = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        // UTF-8, а не «строка системы»: у папки «Моя игра» на Windows
        // generic_string() отдаёт байты ANSI (или бросает), и в пакет уходил
        // ключ, который потом не находился ни одним загрузчиком.
        const std::string rel = Normalize(sage::PathToUtf8(fs::relative(entry.path(), root, ec)));
        if (rel.empty()) continue;

        // СЛУЖЕБНЫЕ ПАПКИ В ИГРУ НЕ ЕДУТ. Всё, что лежит в каталоге, чьё имя
        // начинается с точки, — не содержимое проекта, а чей-то кэш: .sage
        // (обложки сцен редактора), .git, .vs, .idea. Раньше такие папки
        // паковались целиком: репозиторий проекта уезжал в собранную игру, и
        // игра весила как история разработки.
        bool hidden = false;
        for (size_t pos = 0, next; (next = rel.find('/', pos)) != std::string::npos;
             pos = next + 1) {
            if (rel[pos] == '.') { hidden = true; break; }
        }
        if (hidden) continue;

        bool skip = false;
        for (const std::string& d : skipRel)
            if (rel.rfind(d, 0) == 0) { skip = true; break; }
        for (const std::string& suffix : skipSuffixes) {
            if (rel.size() >= suffix.size() &&
                rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        AddFile(base.empty() ? rel : base + "/" + rel, entry.path());
        ++added;
    }
    return added;
}

bool PackWriter::Save(const fs::path& file, const Progress& progress) const {
    m_error.clear();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        m_error = "cannot create " + sage::PathToUtf8(file);
        LOG_ERROR("Pack") << "не удалось создать пакет: " << m_error;
        return false;
    }

    uint8_t salt[16];
    RandomSalt(salt);
    uint8_t key[32];
    DeriveKey(salt, key);

    // Заголовок пишется дважды: сейчас — заглушкой, чтобы занять место, и в
    // конце — по-настоящему, когда станут известны оглавление и его сумма.
    const std::vector<uint8_t> placeholder(kHeaderV2, 0);
    out.write((const char*)placeholder.data(), (std::streamsize)placeholder.size());

    struct Prepared {
        std::vector<uint8_t> Bytes;   // то, что ляжет в файл
        uint64_t Original = 0, Hash = 0;
        bool Compressed = false, Ok = true;
    };
    struct Written {
        uint64_t Offset = 0, Stored = 0, Original = 0, Hash = 0;
        bool Compressed = false;
    };
    std::vector<Written> written(m_entries.size());

    // Подготовка файла (чтение, сжатие, сумма, шифр) — ПАРАЛЛЕЛЬНО, пачками;
    // запись — по порядку. deflate на тысячах файлов в один поток — это
    // минуты, а пачкой на все ядра — секунды. Пачка ограничивает память:
    // готовых кусков одновременно не больше, чем потоков вдвое.
    auto prepare = [&](size_t i, Prepared& p) {
        const Entry& e = m_entries[i];
        std::vector<uint8_t> data;
        if (!e.Disk.empty()) {
            if (!ReadWholeFile(e.Disk, data)) { p.Ok = false; return; }
        } else {
            data = e.Data;
        }
        p.Original = data.size();
        p.Hash = HashBytes(data.data(), data.size());
        // Жмём, только если СТАЛО МЕНЬШЕ. Уже сжатые файлы (png, ogg) от
        // deflate обычно растут, и записать их «сжатыми» значило бы платить
        // временем распаковки за увеличенный размер.
        std::vector<uint8_t> packed;
        if (data.size() >= 64 && DeflateBytes(data, packed, 6) && packed.size() < data.size()) {
            p.Compressed = true;
            p.Bytes = std::move(packed);
        } else {
            p.Bytes = std::move(data);
        }
        uint8_t nonce[12];
        NonceFor(salt, (uint32_t)i, nonce);
        ChaChaXor(key, nonce, p.Bytes.data(), p.Bytes.size());
    };

    const size_t threads = std::max<size_t>(1, std::min<size_t>(16, std::thread::hardware_concurrency()));
    const size_t batch = threads * 2;
    for (size_t first = 0; first < m_entries.size(); first += batch) {
        const size_t count = std::min(batch, m_entries.size() - first);
        std::vector<Prepared> ready(count);
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (size_t t = 0; t < std::min(threads, count); ++t) {
            pool.emplace_back([&] {
                for (size_t k; (k = next.fetch_add(1)) < count;) prepare(first + k, ready[k]);
            });
        }
        for (std::thread& th : pool) th.join();

        for (size_t k = 0; k < count; ++k) {
            const size_t i = first + k;
            Prepared& p = ready[k];
            if (!p.Ok) {
                // Файл пропал или заблокирован между обходом и записью. Молча
                // пропустить нельзя: сцена, ссылающаяся на него, в игре
                // окажется сломанной без единого следа в сборке.
                m_error = "cannot read " + sage::PathToUtf8(m_entries[i].Disk);
                LOG_ERROR("Pack") << "не прочитался файл: " << m_error;
                out.close();
                fs::remove(file, ec);
                return false;
            }
            Written& w = written[i];
            w.Offset = (uint64_t)out.tellp();
            w.Stored = p.Bytes.size();
            w.Original = p.Original;
            w.Hash = p.Hash;
            w.Compressed = p.Compressed;
            if (!p.Bytes.empty()) out.write((const char*)p.Bytes.data(), (std::streamsize)p.Bytes.size());
        }
        if (progress && !progress(first + count, m_entries.size())) {
            m_error = "cancelled";
            out.close();
            fs::remove(file, ec);
            return false;
        }
    }

    const uint64_t indexOffset = (uint64_t)out.tellp();
    std::vector<uint8_t> index;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        PutString(index, m_entries[i].Path);
        PutU64(index, written[i].Offset);
        PutU64(index, written[i].Stored);
        PutU64(index, written[i].Original);
        PutU32(index, written[i].Compressed ? 1u : 0u);
        PutU64(index, written[i].Hash);
    }
    const uint64_t indexHash = HashBytes(index.data(), index.size());
    {
        uint8_t nonce[12];
        NonceFor(salt, kIndexNumber, nonce);
        ChaChaXor(key, nonce, index.data(), index.size());
    }
    out.write((const char*)index.data(), (std::streamsize)index.size());

    std::vector<uint8_t> header;
    PutU32(header, kMagic);
    PutU32(header, kVersion);
    PutU32(header, (uint32_t)m_entries.size());
    PutU32(header, 1);                  // флаги: 1 — зашифрован
    PutU64(header, indexOffset);
    PutU64(header, (uint64_t)index.size());
    PutU64(header, indexHash);
    header.insert(header.end(), salt, salt + 16);
    out.seekp(0);
    out.write((const char*)header.data(), (std::streamsize)header.size());
    out.close();

    if (!out) {
        m_error = "write failed: " + sage::PathToUtf8(file);
        LOG_ERROR("Pack") << "запись пакета не завершилась: " << m_error;
        return false;
    }
    return true;
}

// ============================================================================
//  PackReader
// ============================================================================

bool PackReader::Open(const fs::path& file) {
    m_open = false;
    m_index.clear();
    m_version = 0;

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    uint8_t header[kHeaderV2]{};
    if (!in.read((char*)header, kHeaderV1)) return false;
    if (GetU32(header) != kMagic) {
        LOG_ERROR("Pack") << "не пакет SAGE: " << sage::PathToUtf8(file);
        return false;
    }
    const uint32_t version = GetU32(header + 4);
    if (version != kVersion && version != kVersionPlain) {
        // Версия формата — не «предупреждение». Прочитать пакет чужой версии по
        // старым правилам означает получить мусор, который выглядит как данные.
        LOG_ERROR("Pack") << "версия пакета " << version << " не поддерживается (ожидалась "
                          << kVersion << "): " << sage::PathToUtf8(file);
        return false;
    }
    const uint32_t count = GetU32(header + 8);
    const uint64_t indexOffset = GetU64(header + 16);

    std::vector<uint8_t> index;
    if (version == kVersion) {
        if (!in.read((char*)header + kHeaderV1, kHeaderV2 - kHeaderV1)) return false;
        const uint64_t indexSize = GetU64(header + 24);
        const uint64_t indexHash = GetU64(header + 32);
        std::memcpy(m_salt, header + 40, 16);
        DeriveKey(m_salt, m_key);
        in.seekg(0, std::ios::end);
        const uint64_t fileSize = (uint64_t)in.tellg();
        if (indexOffset > fileSize || indexSize > fileSize - indexOffset) {
            LOG_ERROR("Pack") << "пакет обрезан или повреждён: " << sage::PathToUtf8(file);
            return false;
        }
        index.resize((size_t)indexSize);
        in.seekg((std::streamoff)indexOffset);
        if (indexSize && !in.read((char*)index.data(), (std::streamsize)indexSize)) return false;
        uint8_t nonce[12];
        NonceFor(m_salt, kIndexNumber, nonce);
        ChaChaXor(m_key, nonce, index.data(), index.size());
        if (HashBytes(index.data(), index.size()) != indexHash) {
            // Оглавление не сошлось — значит пакет правили или он побит. Читать
            // по такому оглавлению — получить чужие байты под чужими именами.
            LOG_ERROR("Pack") << "оглавление пакета повреждено или изменено: "
                              << sage::PathToUtf8(file);
            return false;
        }
    } else {
        in.seekg(0, std::ios::end);
        const uint64_t fileSize = (uint64_t)in.tellg();
        if (indexOffset > fileSize) return false;
        index.resize((size_t)(fileSize - indexOffset));
        in.seekg((std::streamoff)indexOffset);
        if (!index.empty() && !in.read((char*)index.data(), (std::streamsize)index.size()))
            return false;
    }

    const size_t record = version == kVersion ? 36 : 28;
    size_t pos = 0;
    m_index.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 4 > index.size()) return false;
        const uint32_t len = GetU32(index.data() + pos);
        pos += 4;
        if (pos + len + record > index.size()) return false;
        std::string path((const char*)index.data() + pos, len);
        pos += len;
        const uint8_t* rec = index.data() + pos;
        pos += record;
        IndexEntry e;
        e.Offset = GetU64(rec);
        e.Stored = GetU64(rec + 8);
        e.Original = GetU64(rec + 16);
        e.Compressed = GetU32(rec + 24) != 0;
        e.Number = i;
        if (version == kVersion) e.Hash = GetU64(rec + 28);
        m_index.emplace_back(std::move(path), e);
    }

    // Оглавление сортируется по пути: поиск двоичный, а не перебором. На пакете
    // в тысячи файлов перебор на каждое чтение текстуры заметен.
    std::sort(m_index.begin(), m_index.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    m_file = file;
    m_version = version;
    m_open = true;
    return true;
}

bool PackReader::Contains(const std::string& virtualPath) const {
    if (!m_open) return false;
    const std::string path = Normalize(virtualPath);
    auto it = std::lower_bound(m_index.begin(), m_index.end(), path,
                               [](const auto& entry, const std::string& key) {
                                   return entry.first < key;
                               });
    return it != m_index.end() && it->first == path;
}

bool PackReader::Read(const std::string& virtualPath, std::vector<uint8_t>& out) const {
    out.clear();
    if (!m_open) return false;
    const std::string path = Normalize(virtualPath);

    auto it = std::lower_bound(m_index.begin(), m_index.end(), path,
                               [](const auto& entry, const std::string& key) {
                                   return entry.first < key;
                               });
    if (it == m_index.end() || it->first != path) return false;

    const IndexEntry& e = it->second;
    std::ifstream in(m_file, std::ios::binary);
    if (!in) return false;
    in.seekg((std::streamoff)e.Offset);

    std::vector<uint8_t> stored((size_t)e.Stored);
    if (e.Stored && !in.read((char*)stored.data(), (std::streamsize)e.Stored)) return false;

    if (m_version == kVersion) {
        uint8_t nonce[12];
        NonceFor(m_salt, e.Number, nonce);
        ChaChaXor(m_key, nonce, stored.data(), stored.size());
    }
    if (!e.Compressed) {
        out = std::move(stored);
    } else if (!InflateBytes(stored, (size_t)e.Original, out)) {
        LOG_ERROR("Pack") << "файл в пакете повреждён: " << path;
        out.clear();
        return false;
    }
    if (m_version == kVersion && HashBytes(out.data(), out.size()) != e.Hash) {
        // Изменённый файл НЕ ОТДАЁТСЯ: «чуть другая» сцена или скрипт хуже
        // отказа — игра пошла бы по чужим правилам молча.
        LOG_ERROR("Pack") << "файл в пакете изменён или повреждён: " << path;
        out.clear();
        return false;
    }
    return true;
}

std::vector<std::string> PackReader::Paths() const {
    std::vector<std::string> paths;
    paths.reserve(m_index.size());
    for (const auto& [path, entry] : m_index) paths.push_back(path);
    return paths;
}

// ============================================================================
//  vfs
// ============================================================================

namespace vfs {

namespace {
std::unique_ptr<PackReader> g_pack;
std::string g_packDir;   // каталог пакета (UTF-8, прямой слэш)

std::string Slashes(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}
} // namespace

bool Mount(const fs::path& packFile) {
    Unmount();
    if (packFile.empty()) return false;
    std::error_code ec;
    if (!fs::exists(packFile, ec)) return false;   // не ошибка: так работает редактор

    auto reader = std::make_unique<PackReader>();
    if (!reader->Open(packFile)) return false;
    LOG_INFO("Pack") << "пакет подключён: " << sage::PathToUtf8(packFile.filename()) << " ("
                     << reader->Count() << " файлов)";
    g_pack = std::move(reader);
    g_packDir = Slashes(sage::PathToUtf8(fs::absolute(packFile, ec).parent_path()));
    return true;
}

void Unmount() {
    g_pack.reset();
    g_packDir.clear();
}
bool Mounted() { return g_pack != nullptr; }

std::string PackedKey(const std::string& path) {
    if (!g_pack || path.empty()) return {};
    if (g_pack->Contains(path)) return Normalize(path);
    // Абсолютный путь внутри каталога игры (или текущего каталога) — это тот
    // же относительный: так ищут файлы «рядом с exe» (шрифт, шейдеры).
    const std::string slashed = Slashes(path);
    std::error_code ec;
    for (const std::string& root : {g_packDir, Slashes(sage::PathToUtf8(fs::current_path(ec)))}) {
        if (root.empty() || slashed.size() <= root.size() + 1) continue;
        if (slashed.compare(0, root.size(), root) != 0 || slashed[root.size()] != '/') continue;
        const std::string rel = slashed.substr(root.size() + 1);
        if (g_pack->Contains(rel)) return Normalize(rel);
    }
    return {};
}

bool Exists(const std::string& path) {
    if (!PackedKey(path).empty()) return true;
    std::error_code ec;
    return fs::exists(sage::PathFromUtf8(path), ec);
}

std::vector<std::string> ListFiles(const std::string& directory, const std::string& extension) {
    std::vector<std::string> found;
    std::string dir = Slashes(directory);
    // Абсолютный каталог внутри папки игры — тот же относительный (см. PackedKey).
    if (g_pack) {
        std::error_code ec;
        for (const std::string& root : {g_packDir, Slashes(sage::PathToUtf8(fs::current_path(ec)))}) {
            if (root.empty() || dir.compare(0, root.size(), root) != 0) continue;
            if (dir.size() == root.size()) { dir.clear(); break; }
            if (dir[root.size()] == '/') { dir = dir.substr(root.size() + 1); break; }
        }
    }
    const std::string prefix = Normalize(dir).empty() ? "" : Normalize(dir) + "/";

    if (g_pack) {
        for (const std::string& path : g_pack->Paths()) {
            if (path.rfind(prefix, 0) != 0) continue;
            // Только НЕПОСРЕДСТВЕННО в каталоге: вложенные подпапки — не то, что
            // спрашивали, и класть их в один список значило бы, что «главной
            // сценой» может стать что-то из scenes/old/.
            if (path.find('/', prefix.size()) != std::string::npos) continue;
            if (extension.empty() ||
                (path.size() >= extension.size() &&
                 path.compare(path.size() - extension.size(), extension.size(), extension) == 0)) {
                found.push_back(path);
            }
        }
    } else {
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(sage::PathFromUtf8(directory), ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string path = Normalize(sage::PathToUtf8(e.path()));
            if (extension.empty() || sage::PathToUtf8(e.path().extension()) == extension) {
                found.push_back(path);
            }
        }
    }
    // Порядок обхода каталога не определён, а «первая сцена по алфавиту» —
    // обещание движка. Сортируем здесь, чтобы вызывающие не помнили об этом.
    std::sort(found.begin(), found.end());
    return found;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    // Пакет ПЕРВЫМ. Рядом с собранной игрой могут остаться файлы от прошлой
    // сборки, и прочитать их вместо упакованных значило бы запустить вчерашнюю
    // логику с сегодняшними данными — дефект, который ищут очень долго.
    if (g_pack) {
        const std::string key = PackedKey(path);
        if (!key.empty()) return g_pack->Read(key, out);
    }
    return ReadWholeFile(sage::PathFromUtf8(path), out);
}

bool ReadText(const std::string& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) return false;
    out.assign((const char*)bytes.data(), bytes.size());
    return true;
}

} // namespace vfs

} // namespace sage::assets
